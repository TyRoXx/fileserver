#include <silicium/tcp_acceptor.hpp>
#include <silicium/coroutine.hpp>
#include <silicium/total_consumer.hpp>
#include <silicium/flatten.hpp>
#include <silicium/socket_observable.hpp>
#include <silicium/sending_observable.hpp>
#include <silicium/received_from_socket_source.hpp>
#include <silicium/observable_source.hpp>
#include <silicium/http/http.hpp>
#include <boost/interprocess/sync/null_mutex.hpp>

namespace
{
	using response_part = Si::incoming_bytes;

	boost::iterator_range<char const *> to_range(response_part part)
	{
		return boost::make_iterator_range(part.begin, part.end);
	}

	template <class MakeSender>
	void serve_client(
		Si::yield_context<Si::nothing> &yield,
		Si::observable<Si::received_from_socket> &receive,
		MakeSender const &make_sender)
	{
		auto receive_sync = Si::make_observable_source(Si::ref(receive), yield);
		Si::received_from_socket_source receive_bytes(receive_sync);
		auto header = Si::http::parse_header(receive_bytes);
		if (!header)
		{
			return;
		}

		std::string body = "Hello at " + header->path;

		{
			Si::http::response_header response;
			response.http_version = "HTTP/1.0";
			response.status_text = "OK";
			response.status = 200;
			response.arguments["Content-Length"] = boost::lexical_cast<std::string>(body.size());
			response.arguments["Connection"] = "close";

			std::string response_header;
			auto header_sink = Si::make_container_sink(response_header);
			Si::http::write_header(header_sink, response);

			auto send_header = make_sender(Si::incoming_bytes(response_header.data(), response_header.data() + response_header.size()));
			auto error = yield.get_one(send_header);
			assert(error);
			if (*error)
			{
				return;
			}
		}

		{
			auto send_body = make_sender(Si::incoming_bytes(body.data(), body.data() + body.size()));
			auto error = yield.get_one(send_body);
			assert(error);
			if (*error)
			{
				return;
			}
		}

		while (Si::get(receive_bytes))
		{
		}
	}

	//TODO: use unique_observable
	using session_handle = Si::shared_observable<Si::nothing>;
}

int main()
{
	boost::asio::io_service io;
	boost::asio::ip::tcp::acceptor acceptor(io, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4(), 8080));
	Si::tcp_acceptor clients(acceptor);

	auto accept_all = Si::make_coroutine<session_handle>([&clients](Si::yield_context<session_handle> &yield)
	{
		for (;;)
		{
			auto accepted = yield.get_one(clients);
			if (!accepted)
			{
				return;
			}
			auto session = Si::visit<session_handle>(
				*accepted,
				[](std::shared_ptr<boost::asio::ip::tcp::socket> socket)
				{
					auto prepare_socket = [socket](Si::yield_context<Si::nothing> &yield)
					{
						std::array<char, 1024> receive_buffer;
						Si::socket_observable received(*socket, boost::make_iterator_range(receive_buffer.data(), receive_buffer.data() + receive_buffer.size()));
						auto make_sender = [socket](Si::incoming_bytes sent)
						{
							return Si::sending_observable(*socket, to_range(sent));
						};
						serve_client(yield, received, make_sender);
					};
					return Si::wrap<Si::nothing>(Si::make_coroutine<Si::nothing>(prepare_socket));
				},
				[](boost::system::error_code) -> session_handle
				{
					throw std::logic_error("to do");
				}
			);
			assert(!session.empty());
			yield(std::move(session));
		}
	});
	auto all_sessions_finished = Si::flatten<boost::interprocess::null_mutex>(std::move(accept_all));
	auto done = Si::make_total_consumer(std::move(all_sessions_finished));
	done.start();
	io.run();
}
