#include "mount.hpp"
#include "storage_reader/http_storage_reader.hpp"
#include <silicium/observable/ptr.hpp>
#include <silicium/error_or.hpp>
#include <silicium/asio/connecting_observable.hpp>
#include <silicium/observable/coroutine.hpp>
#include <silicium/observable/virtualized.hpp>
#include <silicium/source/received_from_socket_source.hpp>
#include <silicium/asio/writing_observable.hpp>
#include <silicium/source/virtualized_source.hpp>
#include <silicium/source/observable_source.hpp>
#include <silicium/http/http.hpp>
#include <silicium/observable/thread_generator.hpp>
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
	namespace
	{
		struct file_system
		{
			boost::asio::io_service io;
			std::unique_ptr<storage_reader> backend;
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
			boost::ignore_unused_variable_warning(conn);
			assert(!g_config.root.empty());
			auto fs = Si::make_unique<file_system>();
			fs->backend = Si::make_unique<http_storage_reader>(fs->io, g_config.server);
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

		Si::error_or<linear_file> read_file(storage_reader &service, unknown_digest const &name)
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

			virtual void get_one(Si::observable<Si::nothing, Si::ptr_observer<Si::observer<Si::nothing>>> &target) SILICIUM_OVERRIDE
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

		boost::optional<typed_reference> resolve_path(std::vector<std::string> const &path_components, digest const &root, storage_reader &service)
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

		bool fill_stat(typed_reference const &file, struct stat &destination, storage_reader &service)
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

		int getattr(const char *path, struct stat *stbuf)
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

		int readdir(
			const char *path,
			void *buf,
			fuse_fill_dir_t filler,
			off_t offset,
			struct fuse_file_info *fi)
		{
			boost::ignore_unused_variable_warning(offset);
			boost::ignore_unused_variable_warning(fi);
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

		int open(const char *path, struct fuse_file_info *fi)
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
			boost::ignore_unused_variable_warning(path);
			std::unique_ptr<open_file>(reinterpret_cast<open_file *>(fi->fh));
			return 0; //ignored by FUSE
		}

		int read(
			const char *path,
			char *buf,
			size_t size,
			off_t offset,
			struct fuse_file_info *fi)
		{
			boost::ignore_unused_variable_warning(path);
			open_file &file = *reinterpret_cast<open_file *>(fi->fh);
			if (file.already_read != offset)
			{
				return -EIO;
			}
			if (file.buffer.empty())
			{
				local_push_context yield_impl;
				Si::push_context<Si::nothing> yield(yield_impl);
				boost::optional<Si::error_or<Si::memory_range>> const piece = yield.get_one(file.source.content);
				assert(piece);
				if (piece->is_error())
				{
					return -EIO;
				}
				int const reading = static_cast<int>(std::min<std::ptrdiff_t>(std::numeric_limits<int>::max(), std::min<std::ptrdiff_t>(size, (*piece)->size())));
				std::copy((*piece)->begin(), (*piece)->begin() + reading, buf);
				file.already_read += reading;
				file.buffer.assign((*piece)->begin() + reading, (*piece)->end());
				return reading;
			}
			else
			{
				int const copied = static_cast<int>(std::min<std::ptrdiff_t>(std::numeric_limits<int>::max(), std::min<std::ptrdiff_t>(size, file.buffer.size())));
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

	void mount_directory(unknown_digest const &root_digest, boost::filesystem::path const &mount_point, boost::asio::ip::tcp::endpoint const &server)
	{
#ifdef __linux__
		chan_deleter deleter;
		deleter.mount_point = fileserver::path(mount_point);

		//remove an existing mount because FUSE did not do that if the previous process was killed
		fuse_unmount(mount_point.c_str(), nullptr);

		fuse_args args{};
		std::unique_ptr<fuse_chan, chan_deleter> chan(fuse_mount(mount_point.c_str(), &args), std::move(deleter));
		if (!chan)
		{
			throw std::runtime_error("fuse_mount failure");
		}
		fuse_operations operations{};
		operations.init = init;
		operations.destroy = destroy;
		operations.getattr = getattr;
		operations.readdir = readdir;
		operations.open = open;
		operations.release = release;
		operations.read = read;
		g_config.root = root_digest;
		g_config.server = server;
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
