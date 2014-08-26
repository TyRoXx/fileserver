#include <boost/asio/io_service.hpp>
#include <silicium/connecting_observable.hpp>
#include <silicium/total_consumer.hpp>
#include <silicium/http/http.hpp>
#include <silicium/coroutine.hpp>
#include <silicium/sending_observable.hpp>
#include <silicium/socket_observable.hpp>
#include <silicium/received_from_socket_source.hpp>
#include <silicium/observable_source.hpp>
#include <iostream>

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
}

int main()
{
	boost::asio::io_service io;
	boost::asio::ip::tcp::socket socket(io);
	Si::connecting_observable connector(socket, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 8080));
	auto connecting = Si::make_total_consumer(Si::make_coroutine<Si::nothing>([&connector, &socket](Si::yield_context<Si::nothing> &yield) -> void
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
			Si::http::write_header(send_sink, make_get_request("localhost", "/0cf58519be5d78070b76de0549279ef7d7d29a29bd46fd3b530baf962b40430a"));
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
			auto socket_source = Si::make_observable_source(Si::socket_observable(socket, boost::make_iterator_range(receive_buffer.data(), receive_buffer.data() + receive_buffer.size())), yield);
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
