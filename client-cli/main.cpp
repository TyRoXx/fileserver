#include "client/mount.hpp"
#include "client/clone.hpp"
#include "client/http_file_service.hpp"
#include <server/path.hpp>
#include <silicium/asio/connecting_observable.hpp>
#include <silicium/observable/total_consumer.hpp>
#include <silicium/http/http.hpp>
#include <silicium/sink/iterator_sink.hpp>
#include <silicium/observable/coroutine_generator.hpp>
#include <silicium/asio/writing_observable.hpp>
#include <silicium/asio/reading_observable.hpp>
#include <silicium/source/received_from_socket_source.hpp>
#include <silicium/source/observable_source.hpp>
#include <silicium/observable/for_each.hpp>
#include <silicium/source/virtualized_source.hpp>
#include <boost/program_options.hpp>
#include <boost/asio/io_service.hpp>
#include <iostream>

namespace
{
	Si::http::request make_get_request(Si::noexcept_string host, Si::noexcept_string path)
	{
		Si::http::request header;
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
		Si::asio::connecting_observable connector(socket, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 8080));
		auto connecting = Si::make_total_consumer(Si::make_coroutine_generator<Si::nothing>([&connector, &socket, &requested_digest](Si::push_context<Si::nothing> &yield) -> void
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
				Si::http::generate_request(send_sink, make_get_request("localhost", "/" + fileserver::format_digest<Si::noexcept_string>(requested_digest)));
				auto sending = Si::asio::make_writing_observable(socket);
				sending.set_buffer(Si::make_memory_range(send_buffer.data(), send_buffer.data() + send_buffer.size()));
				boost::optional<boost::system::error_code> const error = yield.get_one(sending);
				assert(error);
				if (*error)
				{
					return;
				}
			}

			{
				std::array<char, 4096> receive_buffer;
				auto socket_source = Si::virtualize_source(Si::make_observable_source(Si::asio::make_reading_observable(socket, Si::make_iterator_range(receive_buffer.data(), receive_buffer.data() + receive_buffer.size())), yield));
				{
					Si::received_from_socket_source response_source(socket_source);
					boost::optional<Si::http::response> const response = Si::http::parse_response(response_source);
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
					std::cout.write(bytes.begin(), std::distance(bytes.begin(), bytes.end()));
				}
			}
		}));
		connecting.start();
		io.run();
	}
}

int main(int argc, char **argv)
{
	std::string verb;
	std::string digest;
	boost::filesystem::path mount_point;
	std::string host;

	boost::program_options::options_description desc("Allowed options");
	desc.add_options()
	    ("help", "produce help message")
		("verb", boost::program_options::value(&verb), "what to do (get)")
		("digest,d", boost::program_options::value(&digest), "the hash of the file to get/mount")
		("mountpoint", boost::program_options::value(&mount_point), "a directory to mount at")
		("host", boost::program_options::value(&host), "the IP address of the server")
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

	boost::asio::ip::tcp::endpoint server(boost::asio::ip::address_v4::loopback(), 8080);
	if (!host.empty())
	{
		boost::system::error_code ec;
		server.address(boost::asio::ip::address_v4::from_string(host, ec));
		if (ec)
		{
			std::cerr << ec.message() << '\n';
			return 1;
		}
	}

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
		fileserver::mount_directory(*requested, mount_point, server);
	}
	else if (verb == "clone")
	{
		auto requested = parse_digest();
		if (!requested)
		{
			return 1;
		}
		int rc = 1;
		fileserver::filesystem_directory_manipulator mount_point_manipulator(mount_point);
		boost::asio::io_service io;
		fileserver::http_file_service service(io, server);
		auto all = Si::for_each(fileserver::clone_directory(*requested, mount_point_manipulator, service, io), [&rc](boost::system::error_code ec)
		{
			if (ec)
			{
				std::cerr << ec << ": " << ec.message() << '\n';
				rc = 1;
			}
			else
			{
				rc = 0;
			}
		});
		all.start();
		io.run();
		return rc;
	}
	else
	{
		std::cerr
			<< "Unknown verb\n"
			<< desc << "\n";
	    return 1;
	}
}
