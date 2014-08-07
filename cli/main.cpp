#include <reactive/tcp_acceptor.hpp>
#include <reactive/coroutine.hpp>
#include <reactive/total_consumer.hpp>
#include <reactive/flatten.hpp>
#include <reactive/socket_observable.hpp>
#include <reactive/sending_observable.hpp>
#include <reactive/received_from_socket_source.hpp>
#include <reactive/observable_source.hpp>
#include <silicium/http/http.hpp>
#include <boost/interprocess/sync/null_mutex.hpp>

namespace
{
	using response_part = rx::incoming_bytes;

	boost::iterator_range<char const *> to_range(response_part part)
	{
		return boost::make_iterator_range(part.begin, part.end);
	}

	template <class MakeSender>
	void serve_client(
		rx::yield_context<rx::nothing> &yield,
		rx::observable<rx::received_from_socket> &receive,
		MakeSender const &make_sender)
	{
		auto receive_sync = rx::make_observable_source(rx::ref(receive), yield);
		rx::received_from_socket_source receive_bytes(receive_sync);
		auto header = Si::http::parse_header(receive_bytes);
		if (!header)
		{
			return;
		}

		std::string body = "Hello";

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

			auto send_header = make_sender(rx::incoming_bytes(response_header.data(), response_header.data() + response_header.size()));
			auto error = yield.get_one(send_header);
			assert(error);
			if (*error)
			{
				return;
			}
		}

		{
			auto send_body = make_sender(rx::incoming_bytes(body.data(), body.data() + body.size()));
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
	using session_handle = rx::shared_observable<rx::nothing>;

	struct accept_handler : boost::static_visitor<session_handle>
	{
		session_handle operator()(boost::system::error_code) const
		{
			throw std::logic_error("to do");
		}

		session_handle operator()(std::shared_ptr<boost::asio::ip::tcp::socket> socket) const
		{
			auto prepare_socket = [socket](rx::yield_context<rx::nothing> &yield)
			{
				std::array<char, 1024> receive_buffer;
				rx::socket_observable received(*socket, boost::make_iterator_range(receive_buffer.data(), receive_buffer.data() + receive_buffer.size()));
				auto make_sender = [socket](rx::incoming_bytes sent)
				{
					return rx::sending_observable(*socket, to_range(sent));
				};
				serve_client(yield, received, make_sender);
			};
			return rx::wrap<rx::nothing>(rx::make_coroutine<rx::nothing>(prepare_socket));
		}
	};
}

int main()
{
	boost::asio::io_service io;
	boost::asio::ip::tcp::acceptor acceptor(io, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4(), 8080));
	rx::tcp_acceptor clients(acceptor);

	auto accept_all = rx::make_coroutine<session_handle>([&clients](rx::yield_context<session_handle> &yield)
	{
		for (;;)
		{
			auto accepted = yield.get_one(clients);
			if (!accepted)
			{
				return;
			}
			auto session = Si::apply_visitor(accept_handler{}, *accepted);
			assert(!session.empty());
			yield(std::move(session));
		}
	});
	auto all_sessions_finished = rx::flatten<boost::interprocess::null_mutex>(std::move(accept_all));
	auto done = rx::make_total_consumer(std::move(all_sessions_finished));
	done.start();
	io.run();
}
