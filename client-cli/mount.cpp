#include "mount.hpp"
#include <silicium/ptr_observable.hpp>
#include <silicium/error_or.hpp>
#include <silicium/connecting_observable.hpp>
#include <silicium/coroutine.hpp>
#include <silicium/virtualized_observable.hpp>
#include <silicium/received_from_socket_source.hpp>
#include <silicium/sending_observable.hpp>
#include <silicium/virtualized_source.hpp>
#include <silicium/observable_source.hpp>
#include <silicium/http/http.hpp>
#include <silicium/thread.hpp>
#include <silicium/std_threading.hpp>
#include <server/path.hpp>
#include <server/directory_listing.hpp>
#include <fuse.h>
#include <future>
#include <boost/ref.hpp>
#include <boost/algorithm/string/split.hpp>

namespace fileserver
{
	enum class service_error
	{
		file_not_found
	};

	struct service_error_category : boost::system::error_category
	{
		virtual const char *name() const BOOST_SYSTEM_NOEXCEPT SILICIUM_OVERRIDE
		{
			return "service error";
		}

		virtual std::string message(int ev) const SILICIUM_OVERRIDE
		{
			switch (ev)
			{
			case static_cast<int>(service_error::file_not_found): return "file not found";
			}
			SILICIUM_UNREACHABLE();
		}

		virtual bool equivalent(
			boost::system::error_code const &code,
			int condition) const BOOST_SYSTEM_NOEXCEPT SILICIUM_OVERRIDE
		{
			boost::ignore_unused_variable_warning(code);
			boost::ignore_unused_variable_warning(condition);
			return false;
		}
	};

	inline boost::system::error_category const &get_system_error_category()
	{
		static service_error_category const instance;
		return instance;
	}

	inline boost::system::error_code make_error_code(service_error error)
	{
		return boost::system::error_code(static_cast<int>(error), get_system_error_category());
	}
}

namespace boost
{
	namespace system
	{
		template<>
	    struct is_error_code_enum<fileserver::service_error> : std::true_type
		{
		};
	}
}

namespace fileserver
{
	namespace
	{

		using file_offset = std::intmax_t;

		struct linear_file
		{
			file_offset size;
			Si::unique_observable<Si::error_or<Si::incoming_bytes>> content;
		};

		struct file_service
		{
			virtual ~file_service();
			virtual Si::unique_observable<Si::error_or<linear_file>> open(unknown_digest const &name) = 0;
		};

		file_service::~file_service()
		{
		}

		struct http_file_service : file_service
		{
			explicit http_file_service(boost::asio::io_service &io, boost::asio::ip::tcp::endpoint server)
				: io(&io)
				, server(server)
			{
			}

			virtual Si::unique_observable<Si::error_or<linear_file>> open(unknown_digest const &name) SILICIUM_OVERRIDE
			{
				return Si::erase_unique(Si::make_coroutine<Si::error_or<linear_file>>(std::bind(&http_file_service::open_impl, this, std::placeholders::_1, name)));
			}

		private:

			boost::asio::io_service *io = nullptr;
			boost::asio::ip::tcp::endpoint server;

			void open_impl(
				Si::yield_context<Si::error_or<linear_file>> yield,
				unknown_digest const &requested_name)
			{
				auto socket = std::make_shared<boost::asio::ip::tcp::socket>(*io);
				Si::connecting_observable connector(*socket, server);
				{
					boost::optional<boost::system::error_code> const ec = yield.get_one(connector);
					assert(ec);
					if (*ec)
					{
						return yield(*ec);
					}
				}

				std::vector<char> request_buffer;
				{
					Si::http::request_header request;
					request.http_version = "HTTP/1.0";
					request.method = "GET";
					request.path = "/";
					encode_ascii_hex_digits(requested_name.begin(), requested_name.end(), std::back_inserter(request.path));
					request.arguments["Host"] = server.address().to_string();
					auto request_sink = Si::make_container_sink(request_buffer);
					Si::http::write_header(request_sink, request);
				}
				Si::sending_observable sending(*socket, boost::make_iterator_range(request_buffer.data(), request_buffer.data() + request_buffer.size()));
				{
					boost::optional<Si::error_or<std::size_t>> const ec = yield.get_one(sending);
					assert(ec);
					if (ec->error())
					{
						return yield(*ec->error());
					}
					assert(ec->get() == request_buffer.size());
				}

				std::array<char, 8192> buffer;
				Si::socket_observable receiving(*socket, boost::make_iterator_range(buffer.data(), buffer.data() + buffer.size()));
				auto receiving_source = Si::virtualize_source(Si::make_observable_source(std::move(receiving), yield));
				Si::received_from_socket_source response_source(receiving_source);
				boost::optional<Si::http::response_header> const response_header = Si::http::parse_response_header(response_source);
				if (!response_header)
				{
					throw std::logic_error("todo 1");
				}

				if (response_header->status != 200)
				{
					return yield(boost::system::error_code(service_error::file_not_found));
				}

				auto content_length_header = response_header->arguments.find("Content-Length");
				if (content_length_header == response_header->arguments.end())
				{
					throw std::logic_error("todo 2");
				}

				std::vector<byte> first_part(response_source.buffered().begin, response_source.buffered().end);
				file_offset const file_size = boost::lexical_cast<file_offset>(content_length_header->second);
				linear_file file{file_size, Si::erase_unique(Si::make_coroutine<Si::error_or<Si::incoming_bytes>>(
					[first_part, socket, file_size]
						(Si::yield_context<Si::error_or<Si::incoming_bytes>> yield)
					{
						yield(Si::incoming_bytes(
							reinterpret_cast<char const *>(first_part.data()),
							reinterpret_cast<char const *>(first_part.data() + first_part.size())));
						file_offset receive_counter = first_part.size();
						std::array<char, 8192> buffer;
						Si::socket_observable receiving(*socket, boost::make_iterator_range(buffer.data(), buffer.data() + buffer.size()));
						while (receive_counter < file_size)
						{
							auto piece = yield.get_one(receiving);
							if (!piece)
							{
								break;
							}
							if (!piece->is_error())
							{
								receive_counter += piece->get().size();
							}
							yield(*piece);
						}
					}))};
				yield(std::move(file));
			}
		};

		struct file_system
		{
			boost::asio::io_service io;
			std::unique_ptr<file_service> backend;
			std::future<void> worker;
			boost::optional<boost::asio::io_service::work> keep_running;
			unknown_digest root;
		};

		char const * const hello_path = "/hello";
		char const * const hello_str = "Hello, fuse!\n";

		unknown_digest root;

		void *init(struct fuse_conn_info *conn)
		{
			assert(!root.empty());
			auto fs = Si::make_unique<file_system>();
			fs->backend = Si::make_unique<http_file_service>(fs->io, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 8080));
			fs->keep_running = boost::in_place(boost::ref(fs->io));
			auto &io = fs->io;
			fs->worker = std::async(std::launch::async, [&io]()
			{
				for (;;)
				{
					try
					{
						io.run();
						break;
					}
					catch (...)
					{
						continue;
					}
				}
			});
			fs->root = std::move(root);
			return fs.release();
		}

		void destroy(void *private_data)
		{
			std::unique_ptr<file_system>(static_cast<file_system *>(private_data));
		}

		Si::error_or<linear_file> read_file(file_service &service, unknown_digest const &name)
		{
			Si::error_or<linear_file> file;
			Si::detail::event<Si::std_threading> waiting;
			waiting.block(Si::transform(service.open(name), [&file](Si::error_or<linear_file> opened_file)
			{
				file = std::move(opened_file);
				return Si::nothing();
			}));
			return file;
		}

		struct local_yield_context : Si::detail::yield_context_impl<Si::nothing>
		{
			virtual void push_result(Si::nothing result) SILICIUM_OVERRIDE
			{
				boost::ignore_unused(result);
				SILICIUM_UNREACHABLE();
			}

			virtual void get_one(Si::observable<Si::nothing> &target) SILICIUM_OVERRIDE
			{
				Si::detail::event<Si::std_threading> waiting;
				waiting.block(Si::ref(target));
			}
		};

		auto parse_directory_listing(linear_file file)
		{
			local_yield_context yield_impl;
			Si::yield_context<Si::nothing> yield(yield_impl);
			auto receiving_source = Si::virtualize_source(Si::make_observable_source(Si::ref(file.content), yield));
			Si::received_from_socket_source content_source(receiving_source);
			return deserialize_json(std::move(content_source));
		}

		int hello_getattr(const char *path, struct stat *stbuf)
		{
			memset(stbuf, 0, sizeof(*stbuf));
			file_system * const fs = static_cast<file_system *>(fuse_get_context()->private_data);
			std::vector<std::string> path_components;
			boost::algorithm::split(path_components, path, [](char c) { return c == '/'; });

			content_type last_type = json_listing_content_type;
			{
				unknown_digest current_directory = fs->root;
				for (auto component = path_components.begin() + 1; component != path_components.end(); ++component)
				{
					auto file = read_file(*fs->backend, current_directory);
					if (file.is_error())
					{
						return -EIO;
					}
					auto parsed = parse_directory_listing(std::move(file).get());
					if (!Si::visit<bool>(
						parsed,
						[&current_directory, &last_type, component](std::unique_ptr<directory_listing> const &listing)
					{
						auto found = listing->entries.find(*component);
						if (found == listing->entries.end())
						{
							return false;
						}
						last_type = found->second.type;
						current_directory = to_unknown_digest(found->second.referenced);
						return true;
					},
						[](std::size_t)
					{
						return false;
					}))
					{
						return -EIO;
					}
				}
			}

			if (last_type == blob_content_type)
			{
				stbuf->st_mode = S_IFREG | 0444;
				stbuf->st_nlink = 1;
				stbuf->st_size = 1;
				return 0;
			}
			else if (last_type == json_listing_content_type)
			{
				stbuf->st_mode = S_IFDIR | 0555;
				stbuf->st_nlink = 2;
				return 0;
			}

			return -ENOENT;
		}

		int hello_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
					 off_t offset, struct fuse_file_info *fi)
		{
			(void) offset;
			(void) fi;

			try
			{
				file_system * const fs = static_cast<file_system *>(fuse_get_context()->private_data);
				auto file = read_file(*fs->backend, fs->root);
				if (file.is_error())
				{
					return -ENOENT;
				}

				auto parsed = parse_directory_listing(std::move(file).get());
				return Si::visit<int>(
					parsed,
					[buf, filler](std::unique_ptr<directory_listing> &listing)
				{
					filler(buf, ".", NULL, 0);
					filler(buf, "..", NULL, 0);
					for (auto const &entry : listing->entries)
					{
						struct stat s{};
						s.st_size = 100;
						s.st_mode = 0777 | S_IFREG;
						s.st_nlink = 1;
						filler(buf, entry.first.c_str(), &s, 0);
					}
					return 0;
				},
					[](std::size_t)
				{
					return -ENOENT;
				});
			}
			catch (...)
			{
				return -EIO;
			}
		}

		int hello_open(const char *path, struct fuse_file_info *fi)
		{
			if (strcmp(path, hello_path) != 0)
				return -ENOENT;

			if ((fi->flags & 3) != O_RDONLY)
				return -EACCES;

			return 0;
		}

		int hello_read(const char *path, char *buf, size_t size, off_t offset,
					  struct fuse_file_info *fi)
		{
			size_t len;
			(void) fi;
			if(strcmp(path, hello_path) != 0)
				return -ENOENT;

			len = strlen(hello_str);
			if (static_cast<size_t>(offset) < len) {
				if (offset + size > len)
					size = len - offset;
				memcpy(buf, hello_str + offset, size);
			} else
				size = 0;

			return static_cast<int>(size);
		}

		struct user_data_for_fuse
		{

		};

		struct chan_deleter
		{
			fileserver::path mount_point;

			void operator()(fuse_chan *chan) const
			{
				fuse_unmount(mount_point.c_str(), chan);
			}
		};

		struct fuse_deleter
		{
			void operator()(fuse *f) const
			{
				fuse_destroy(f);
			}
		};
	}

	void mount_directory(unknown_digest const &root_digest, boost::filesystem::path const &mount_point)
	{
		chan_deleter deleter;
		deleter.mount_point = fileserver::path(mount_point);
		fuse_args args{};
		std::unique_ptr<fuse_chan, chan_deleter> chan(fuse_mount(mount_point.c_str(), &args), std::move(deleter));
		if (!chan)
		{
			throw std::runtime_error("fuse_mount failure");
		}
		fuse_operations operations{};
		operations.init = init;
		operations.destroy = destroy;
		operations.getattr = hello_getattr;
		operations.readdir = hello_readdir;
		operations.open = hello_open;
		operations.read = hello_read;
		root = root_digest;
		user_data_for_fuse user_data;
		std::unique_ptr<fuse, fuse_deleter> const f(fuse_new(chan.get(), &args, &operations, sizeof(operations), &user_data));
		if (!f)
		{
			throw std::runtime_error("fuse_new failure");
		}

		//fuse_new seems to take ownership of the fuse_chan
		chan.release();

		fuse_loop(f.get());
	}
}
