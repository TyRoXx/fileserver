#include <server/digest.hpp>
#include <silicium/connecting_observable.hpp>
#include <silicium/total_consumer.hpp>
#include <silicium/http/http.hpp>
#include <silicium/coroutine.hpp>
#include <silicium/sending_observable.hpp>
#include <silicium/socket_observable.hpp>
#include <silicium/received_from_socket_source.hpp>
#include <silicium/observable_source.hpp>
#include <silicium/virtualized_source.hpp>
#include <boost/program_options.hpp>
#include <boost/asio/io_service.hpp>
#include <iostream>
#include <fuse.h>

namespace
{
	Si::http::request_header make_get_request(std::string host, std::string path)
	{
		Si::http::request_header header;
		header.http_version = "HTTP/1.0";
		header.method = "GET";
		header.path = std::move(path);
		header.arguments["Host"] = std::move(host);
		return header;
	}

	void get_file(fileserver::unknown_digest const &requested_digest)
	{
		boost::asio::io_service io;
		boost::asio::ip::tcp::socket socket(io);
		Si::connecting_observable connector(socket, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 8080));
		auto connecting = Si::make_total_consumer(Si::make_coroutine<Si::nothing>([&connector, &socket, &requested_digest](Si::yield_context<Si::nothing> &yield) -> void
		{
			{
				boost::optional<boost::system::error_code> const error = yield.get_one(connector);
				assert(error);
				if (*error)
				{
					return;
				}
			}

			{
				std::vector<char> send_buffer;
				auto send_sink = Si::make_container_sink(send_buffer);
				Si::http::write_header(send_sink, make_get_request("localhost", "/" + fileserver::format_digest(requested_digest)));
				Si::sending_observable sending(socket, boost::make_iterator_range(send_buffer.data(), send_buffer.data() + send_buffer.size()));
				boost::optional<Si::error_or<std::size_t>> const error = yield.get_one(sending);
				assert(error);
				if (error->is_error())
				{
					return;
				}
			}

			{
				std::array<char, 4096> receive_buffer;
				auto socket_source = Si::virtualize_source(Si::make_observable_source(Si::socket_observable(socket, boost::make_iterator_range(receive_buffer.data(), receive_buffer.data() + receive_buffer.size())), yield));
				{
					Si::received_from_socket_source response_source(socket_source);
					boost::optional<Si::http::response_header> const response = Si::http::parse_response_header(response_source);
					if (!response)
					{
						return;
					}
				}
				for (;;)
				{
					auto piece = Si::get(socket_source);
					if (!piece || piece->is_error())
					{
						break;
					}
					auto const &bytes = piece->get();
					std::cout.write(bytes.begin, std::distance(bytes.begin, bytes.end));
				}
			}
		}));
		connecting.start();
		io.run();
	}

	char const * const hello_path = "/hello";
	char const * const hello_str = "Hello, fuse!\n";

	int hello_getattr(const char *path, struct stat *stbuf)
	{
		int res = 0;

		memset(stbuf, 0, sizeof(struct stat));
		if (strcmp(path, "/") == 0) {
			stbuf->st_mode = S_IFDIR | 0755;
			stbuf->st_nlink = 2;
		} else if (strcmp(path, hello_path) == 0) {
			stbuf->st_mode = S_IFREG | 0444;
			stbuf->st_nlink = 1;
			stbuf->st_size = strlen(hello_str);
		} else
			res = -ENOENT;

		return res;
	}

	static int hello_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
				 off_t offset, struct fuse_file_info *fi)
	{
		(void) offset;
		(void) fi;

		if (strcmp(path, "/") != 0)
			return -ENOENT;

		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);
		filler(buf, hello_path + 1, NULL, 0);

		return 0;
	}

	static int hello_open(const char *path, struct fuse_file_info *fi)
	{
		if (strcmp(path, hello_path) != 0)
			return -ENOENT;

		if ((fi->flags & 3) != O_RDONLY)
			return -EACCES;

		return 0;
	}

	static int hello_read(const char *path, char *buf, size_t size, off_t offset,
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

	void mount_directory(fileserver::unknown_digest const &root_digest, boost::filesystem::path const &mount_point)
	{
		fuse_args args{};
		fuse_chan * const chan = fuse_mount(mount_point.c_str(), &args);
		if (!chan)
		{
			throw std::runtime_error("fuse_mount failure");
		}
		fuse_operations operations{};
		operations.getattr = hello_getattr;
		operations.readdir = hello_readdir;
		operations.open = hello_open;
		operations.read = hello_read;
		user_data_for_fuse user_data;
		fuse * const f = fuse_new(chan, &args, &operations, sizeof(operations), &user_data);
		if (!f)
		{
			throw std::runtime_error("fuse_new failure");
		}
		fuse_loop(f);
		fuse_destroy(f);
	}
}

int main(int argc, char **argv)
{
	std::string verb;
	std::string digest;
	boost::filesystem::path mount_point;

	boost::program_options::options_description desc("Allowed options");
	desc.add_options()
	    ("help", "produce help message")
		("verb", boost::program_options::value(&verb), "what to do (get)")
		("digest,d", boost::program_options::value(&digest), "the hash of the file to get/mount")
		("mountpoint", boost::program_options::value(&mount_point), "a directory to mount at")
	;

	boost::program_options::positional_options_description positional;
	positional.add("verb", 1);
	positional.add("digest", 1);
	boost::program_options::variables_map vm;
	try
	{
		boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(desc).positional(positional).run(), vm);
	}
	catch (boost::program_options::error const &ex)
	{
		std::cerr
			<< ex.what() << '\n'
			<< desc << "\n";
		return 1;
	}

	boost::program_options::notify(vm);

	if (vm.count("help"))
	{
	    std::cerr << desc << "\n";
	    return 1;
	}

	auto const parse_digest = [&digest]() -> boost::optional<fileserver::unknown_digest>
	{
		auto parsed = fileserver::parse_digest(digest.begin(), digest.end());
		if (!parsed)
		{
			std::cerr << "The digest must be an even number of hexidecimal digits.\n";
			return boost::none;
		}
		if (parsed->empty())
		{
			std::cerr << "The digest must be empty\n";
			return boost::none;
		}
		return parsed;
	};

	if (verb == "get")
	{
		auto requested = parse_digest();
		if (!requested)
		{
			return 1;
		}
		get_file(*requested);
	}
	else if (verb == "mount")
	{
		auto requested = parse_digest();
		if (!requested)
		{
			return 1;
		}
		mount_directory(*requested, mount_point);
	}
	else
	{
		std::cerr
			<< "Unknown verb\n"
			<< desc << "\n";
	    return 1;
	}
}
