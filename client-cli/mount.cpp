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
#include <silicium/to_unique.hpp>
#include <server/path.hpp>
#include <server/directory_listing.hpp>
#include <future>
#include <boost/ref.hpp>
#include <boost/algorithm/string/split.hpp>

#ifdef __linux__
#	include <fuse.h>
#endif

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

#ifdef _MSC_VER
			linear_file()
			{
			}

			linear_file(file_offset size, Si::unique_observable<Si::error_or<Si::incoming_bytes>> content)
				: size(size)
				, content(std::move(content))
			{
			}

			linear_file(linear_file &&other)
				: size(other.size)
				, content(std::move(other.content))
			{
			}

			linear_file &operator = (linear_file &&other)
			{
				size = other.size;
				content = std::move(other.content);
				return *this;
			}

			SILICIUM_DELETED_FUNCTION(linear_file(linear_file const &))
			SILICIUM_DELETED_FUNCTION(linear_file &operator = (linear_file const &))
#endif
		};

		struct file_service
		{
			virtual ~file_service();
			virtual Si::unique_observable<Si::error_or<linear_file>> open(unknown_digest const &name) = 0;
			virtual Si::unique_observable<Si::error_or<file_offset>> size(unknown_digest const &name) = 0;
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

			virtual Si::unique_observable<Si::error_or<file_offset>> size(unknown_digest const &name) SILICIUM_OVERRIDE
			{
				return Si::erase_unique(Si::make_coroutine<Si::error_or<file_offset>>(std::bind(&http_file_service::size_impl, this, std::placeholders::_1, name)));
			}

		private:

			boost::asio::io_service *io = nullptr;
			boost::asio::ip::tcp::endpoint server;

			Si::error_or<std::shared_ptr<boost::asio::ip::tcp::socket>> connect(
				Si::yield_context yield)
			{
				auto socket = std::make_shared<boost::asio::ip::tcp::socket>(*io);
				Si::connecting_observable connector(*socket, server);
				{
					boost::optional<boost::system::error_code> const ec = yield.get_one(connector);
					assert(ec);
					if (*ec)
					{
						return *ec;
					}
				}
				return socket;
			}

			std::vector<char> serialize_request(std::string const &method, unknown_digest const &requested)
			{
				std::vector<char> request_buffer;
				Si::http::request_header request;
				request.http_version = "HTTP/1.0";
				request.method = method;
				request.path = "/";
				encode_ascii_hex_digits(requested.begin(), requested.end(), std::back_inserter(request.path));
				request.arguments["Host"] = server.address().to_string();
				auto request_sink = Si::make_container_sink(request_buffer);
				Si::http::write_header(request_sink, request);
				return request_buffer;
			}

			Si::error_or<Si::nothing> send_all(
				Si::yield_context yield,
				boost::asio::ip::tcp::socket &socket,
				std::vector<char> const &buffer)
			{
				Si::sending_observable sending(socket, boost::make_iterator_range(buffer.data(), buffer.data() + buffer.size()));
				boost::optional<Si::error_or<std::size_t>> const ec = yield.get_one(sending);
				assert(ec);
				if (ec->error())
				{
					return *ec->error();
				}
				assert(ec->get() == buffer.size());
				return Si::nothing();
			}

			Si::error_or<std::pair<std::unique_ptr<Si::http::response_header>, std::size_t>> receive_response_header(
				Si::yield_context yield,
				boost::asio::ip::tcp::socket &socket,
				std::array<char, 8192> &buffer)
			{
				Si::socket_observable receiving(socket, boost::make_iterator_range(buffer.data(), buffer.data() + buffer.size()));
				auto receiving_source = Si::virtualize_source(Si::make_observable_source(std::move(receiving), yield));
				Si::received_from_socket_source response_source(receiving_source);
				boost::optional<Si::http::response_header> const response_header = Si::http::parse_response_header(response_source);
				if (!response_header)
				{
					throw std::logic_error("todo 1");
				}
				return std::make_pair(Si::to_unique(std::move(*response_header)), response_source.buffered().size());
			}

			void size_impl(
				Si::push_context<Si::error_or<file_offset>> yield,
				unknown_digest const &requested_name)
			{
				auto const maybe_socket = connect(yield);
				if (maybe_socket.is_error())
				{
					return yield(*maybe_socket.error());
				}
				auto const socket = maybe_socket.get();
				{
					auto const request_buffer = serialize_request("GET", requested_name);
					auto const sent = send_all(yield, *socket, request_buffer);
					if (sent.is_error())
					{
						return yield(*sent.error());
					}
				}
				std::array<char, 8192> buffer;
				auto const received_header = receive_response_header(yield, *socket, buffer);
				if (received_header.is_error())
				{
					return yield(*received_header.error());
				}
				auto const &response_header = *received_header.get().first;

				if (response_header.status != 200)
				{
					return yield(boost::system::error_code(service_error::file_not_found));
				}

				auto content_length_header = response_header.arguments.find("Content-Length");
				if (content_length_header == response_header.arguments.end())
				{
					throw std::logic_error("todo 2");
				}

				yield(boost::lexical_cast<file_offset>(content_length_header->second));
			}

			void open_impl(
				Si::push_context<Si::error_or<linear_file>> yield,
				unknown_digest const &requested_name)
			{
				auto const maybe_socket = connect(yield);
				if (maybe_socket.is_error())
				{
					return yield(*maybe_socket.error());
				}
				auto const socket = maybe_socket.get();
				{
					auto const request_buffer = serialize_request("GET", requested_name);
					auto const sent = send_all(yield, *socket, request_buffer);
					if (sent.is_error())
					{
						return yield(*sent.error());
					}
				}
				std::array<char, 8192> buffer;
				auto const received_header = receive_response_header(yield, *socket, buffer);
				if (received_header.is_error())
				{
					return yield(*received_header.error());
				}
				auto const &response_header = *received_header.get().first;
				std::size_t const buffered_content = received_header.get().second;

				if (response_header.status != 200)
				{
					return yield(boost::system::error_code(service_error::file_not_found));
				}

				auto content_length_header = response_header.arguments.find("Content-Length");
				if (content_length_header == response_header.arguments.end())
				{
					throw std::logic_error("todo 2");
				}

				std::vector<byte> first_part(buffer.begin(), buffer.begin() + buffered_content);
				file_offset const file_size = boost::lexical_cast<file_offset>(content_length_header->second);
				linear_file file{file_size, Si::erase_unique(Si::make_coroutine<Si::error_or<Si::incoming_bytes>>(
					[first_part, socket, file_size]
						(Si::push_context<Si::error_or<Si::incoming_bytes>> yield)
					{
						if (!first_part.empty())
						{
							yield(Si::incoming_bytes(
								reinterpret_cast<char const *>(first_part.data()),
								reinterpret_cast<char const *>(first_part.data() + first_part.size())));
						}
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

		struct configuration
		{
			unknown_digest root;
			boost::asio::ip::tcp::endpoint server;
		}
		g_config;

#ifdef __linux__
		void *init(struct fuse_conn_info *conn)
		{
			assert(!g_config.root.empty());
			auto fs = Si::make_unique<file_system>();
			fs->backend = Si::make_unique<http_file_service>(fs->io, g_config.server);
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
			fs->root = g_config.root;
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

		struct local_push_context : Si::detail::push_context_impl<Si::nothing>
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
			local_push_context yield_impl;
			Si::push_context<Si::nothing> yield(yield_impl);
			auto receiving_source = Si::virtualize_source(Si::make_observable_source(Si::ref(file.content), yield));
			Si::received_from_socket_source content_source(receiving_source);
			return deserialize_json(std::move(content_source));
		}

		boost::optional<typed_reference> resolve_path(std::vector<std::string> const &path_components, digest const &root, file_service &service)
		{
			content_type last_type = json_listing_content_type;
			digest last_digest = root;
			for (auto component = path_components.begin(); component != path_components.end(); ++component)
			{
				auto file = read_file(service, to_unknown_digest(last_digest));
				if (file.is_error())
				{
					return boost::none;
				}
				auto parsed = parse_directory_listing(std::move(file).get());
				if (!Si::visit<bool>(
					parsed,
					[&last_digest, &last_type, component](std::unique_ptr<directory_listing> const &listing)
				{
					auto found = listing->entries.find(*component);
					if (found == listing->entries.end())
					{
						return false;
					}
					last_type = found->second.type;
					last_digest = found->second.referenced;
					return true;
				},
					[](std::size_t)
				{
					return false;
				}))
				{
					return boost::none;
				}
			}
			return typed_reference(std::move(last_type), last_digest);
		}

		std::vector<std::string> split_path(char const *path)
		{
			std::vector<std::string> path_components;
			boost::algorithm::split(path_components, path, [](char c) { return c == '/'; });
			path_components.erase(std::remove(path_components.begin(), path_components.end(), std::string()), path_components.end());
			return path_components;
		}

		bool fill_stat(typed_reference const &file, struct stat &destination, file_service &service)
		{
			if (file.type == blob_content_type)
			{
				destination.st_mode = S_IFREG | 0444;
				destination.st_nlink = 1;

				local_push_context yield_impl;
				Si::push_context<Si::nothing> yield(yield_impl);
				auto future_size = service.size(to_unknown_digest(file.referenced));
				boost::optional<Si::error_or<file_offset>> const size = yield.get_one(future_size);
				assert(size);
				if (size->is_error())
				{
					return false;
				}
				destination.st_size = size->get();
				return true;
			}
			else if (file.type == json_listing_content_type)
			{
				destination.st_mode = S_IFDIR | 0555;
				destination.st_nlink = 2;
				return true;
			}
			return false;
		}

		int hello_getattr(const char *path, struct stat *stbuf)
		{
			memset(stbuf, 0, sizeof(*stbuf));
			file_system * const fs = static_cast<file_system *>(fuse_get_context()->private_data);
			auto const file_info = resolve_path(split_path(path), *to_sha256_digest(fs->root), *fs->backend);
			if (!file_info)
			{
				return -ENOENT;
			}
			if (fill_stat(*file_info, *stbuf, *fs->backend))
			{
				return 0;
			}
			return -ENOENT;
		}

		int hello_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
					 off_t offset, struct fuse_file_info *fi)
		{
			try
			{
				file_system * const fs = static_cast<file_system *>(fuse_get_context()->private_data);
				auto const resolved = resolve_path(split_path(path), *to_sha256_digest(fs->root), *fs->backend);
				if (!resolved)
				{
					return -ENOENT;
				}

				auto file = read_file(*fs->backend, to_unknown_digest(resolved->referenced));
				if (file.is_error())
				{
					return -ENOENT;
				}

				auto parsed = parse_directory_listing(std::move(file).get());
				return Si::visit<int>(
					parsed,
					[buf, filler, fs](std::unique_ptr<directory_listing> &listing)
				{
					filler(buf, ".", NULL, 0);
					filler(buf, "..", NULL, 0);
					for (auto const &entry : listing->entries)
					{
						struct stat s{};
						if (fill_stat(entry.second, s, *fs->backend))
						{
							filler(buf, entry.first.c_str(), &s, 0);
						}
					}
					return 0;
				},
					[](std::size_t)
				{
					return -ENOENT;
				});
			}
			catch (std::exception const &e)
			{
				std::cerr << e.what() << '\n';
				return -EIO;
			}
		}

		struct open_file
		{
			linear_file source;
			std::vector<char> buffer;
			file_offset already_read = 0;

			explicit open_file(linear_file source)
				: source(std::move(source))
			{
			}
		};

		int hello_open(const char *path, struct fuse_file_info *fi)
		{
			if ((fi->flags & 3) != O_RDONLY) //the files are currently read-only
			{
				return -EACCES;
			}

			try
			{
				file_system * const fs = static_cast<file_system *>(fuse_get_context()->private_data);
				auto const resolved = resolve_path(split_path(path), *to_sha256_digest(fs->root), *fs->backend);
				if (!resolved)
				{
					return -ENOENT;
				}
				if (resolved->type == "blob")
				{
					auto file = read_file(*fs->backend, to_unknown_digest(resolved->referenced));
					if (file.is_error())
					{
						return -EIO;
					}

					auto file_ptr = Si::make_unique<open_file>(std::move(file.get()));
					fi->fh = reinterpret_cast<std::uintptr_t>(file_ptr.release());
					return 0;
				}
				else
				{
					return -ENOTSUP;
				}
			}
			catch (...)
			{
				return -EIO;
			}
		}

		int release(const char *path, struct fuse_file_info *fi)
		{
			std::unique_ptr<open_file>(reinterpret_cast<open_file *>(fi->fh));
			return 0; //ignored by FUSE
		}

		int hello_read(const char *path, char *buf, size_t size, off_t offset,
					  struct fuse_file_info *fi)
		{
			open_file &file = *reinterpret_cast<open_file *>(fi->fh);
			if (file.already_read != offset)
			{
				return -EIO;
			}
			if (file.buffer.empty())
			{
				local_push_context yield_impl;
				Si::push_context<Si::nothing> yield(yield_impl);
				boost::optional<Si::error_or<Si::incoming_bytes>> const piece = yield.get_one(file.source.content);
				assert(piece);
				if (piece->is_error())
				{
					return -EIO;
				}
				std::size_t const reading = std::min(size, (*piece)->size());
				std::copy((*piece)->begin, (*piece)->begin + reading, buf);
				file.already_read += reading;
				file.buffer.assign((*piece)->begin + reading, (*piece)->end);
				return reading; //TODO fix warning
			}
			else
			{
				std::size_t const copied = std::min(size, file.buffer.size());
				std::copy(file.buffer.begin(), file.buffer.begin() + copied, buf);
				file.already_read += copied;
				file.buffer.erase(file.buffer.begin(), file.buffer.begin() + copied);
				return copied;
			}
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
#endif
	}

	void mount_directory(unknown_digest const &root_digest, boost::filesystem::path const &mount_point)
	{
#ifdef __linux__
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
		operations.release = release;
		operations.read = hello_read;
		g_config.root = root_digest;
		g_config.server = boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 8080);
		user_data_for_fuse user_data;
		std::unique_ptr<fuse, fuse_deleter> const f(fuse_new(chan.get(), &args, &operations, sizeof(operations), &user_data));
		if (!f)
		{
			throw std::runtime_error("fuse_new failure");
		}

		//fuse_new seems to take ownership of the fuse_chan
		chan.release();

		fuse_loop(f.get());
#endif
	}
}
