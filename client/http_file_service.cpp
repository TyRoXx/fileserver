#include "http_file_service.hpp"
#include <silicium/observable/coroutine_generator.hpp>
#include <silicium/asio/connecting_observable.hpp>
#include <silicium/asio/writing_observable.hpp>
#include <silicium/source/received_from_socket_source.hpp>
#include <silicium/observable/virtualized.hpp>
#include <silicium/source/virtualized_source.hpp>
#include <silicium/to_unique.hpp>
#include <silicium/source/observable_source.hpp>
#include <silicium/sink/iterator_sink.hpp>

namespace fileserver
{
	http_file_service::http_file_service(boost::asio::io_service &io, boost::asio::ip::tcp::endpoint server)
		: io(&io)
		, server(server)
	{
	}

	Si::unique_observable<Si::error_or<linear_file>> http_file_service::open(unknown_digest const &name)
	{
		return Si::erase_unique(Si::make_coroutine_generator<Si::error_or<linear_file>>(std::bind(&http_file_service::open_impl, this, std::placeholders::_1, name)));
	}

	Si::unique_observable<Si::error_or<file_offset>> http_file_service::size(unknown_digest const &name)
	{
		return Si::erase_unique(Si::make_coroutine_generator<Si::error_or<file_offset>>(std::bind(&http_file_service::size_impl, this, std::placeholders::_1, name)));
	}

	Si::error_or<std::shared_ptr<boost::asio::ip::tcp::socket>> http_file_service::connect(
		Si::yield_context yield)
	{
		auto socket = std::make_shared<boost::asio::ip::tcp::socket>(*io);
		Si::asio::connecting_observable connector(*socket, server);
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

	std::vector<char> http_file_service::serialize_request(Si::noexcept_string method, unknown_digest const &requested)
	{
		std::vector<char> request_buffer;
		Si::http::request request;
		request.http_version = "HTTP/1.0";
		request.method = std::move(method);
		request.path = "/";
		encode_ascii_hex_digits(requested.begin(), requested.end(), std::back_inserter(request.path));
		request.arguments["Host"] = server.address().to_string().c_str();
		auto request_sink = Si::make_container_sink(request_buffer);
		Si::http::generate_request(request_sink, request);
		return request_buffer;
	}

	Si::error_or<Si::nothing> http_file_service::send_all(
		Si::yield_context yield,
		boost::asio::ip::tcp::socket &socket,
		std::vector<char> const &buffer)
	{
		auto sending = Si::asio::make_writing_observable(socket);
		sending.set_buffer(Si::make_memory_range(buffer.data(), buffer.data() + buffer.size()));
		boost::optional<boost::system::error_code> const ec = yield.get_one(sending);
		assert(ec);
		if (*ec)
		{
			return *ec;
		}
		return Si::nothing();
	}

	Si::error_or<std::pair<Si::http::response, std::size_t>> http_file_service::receive_response_header(
		Si::yield_context yield,
		boost::asio::ip::tcp::socket &socket,
		std::array<char, 8192> &buffer)
	{
		auto receiving = Si::asio::make_reading_observable(socket, Si::make_iterator_range(buffer.data(), buffer.data() + buffer.size()));
		auto receiving_source = Si::virtualize_source(Si::make_observable_source(std::move(receiving), yield));
		Si::received_from_socket_source response_source(receiving_source);
		boost::optional<Si::http::response> response_header = Si::http::parse_response(response_source);
		if (!response_header)
		{
			throw std::logic_error("todo 1");
		}
		return std::make_pair(std::move(*response_header), response_source.buffered().size());
	}

	void http_file_service::size_impl(
		Si::push_context<Si::error_or<file_offset>> yield,
		unknown_digest const &requested_name)
	{
		auto const maybe_socket = connect(yield);
		if (maybe_socket.is_error())
		{
			return yield(maybe_socket.error());
		}
		auto const socket = maybe_socket.get();
		{
			auto const request_buffer = serialize_request("HEAD", requested_name);
			auto const sent = send_all(yield, *socket, request_buffer);
			if (sent.is_error())
			{
				return yield(sent.error());
			}
		}
		std::array<char, 8192> buffer;
		auto const received_header = receive_response_header(yield, *socket, buffer);
		if (received_header.is_error())
		{
			return yield(received_header.error());
		}
		auto const &response_header = received_header.get().first;

		if (response_header.status != 200)
		{
			return yield(boost::system::error_code(service_error::file_not_found));
		}

		auto content_length_header = response_header.arguments->find("Content-Length");
		if (content_length_header == response_header.arguments->end())
		{
			throw std::logic_error("todo 2");
		}

		yield(boost::lexical_cast<file_offset>(content_length_header->second));
	}

	void http_file_service::open_impl(
		Si::push_context<Si::error_or<linear_file>> yield,
		unknown_digest const &requested_name)
	{
		auto const maybe_socket = connect(yield);
		if (maybe_socket.is_error())
		{
			return yield(maybe_socket.error());
		}
		auto const socket = maybe_socket.get();
		{
			auto const request_buffer = serialize_request("GET", requested_name);
			auto const sent = send_all(yield, *socket, request_buffer);
			if (sent.is_error())
			{
				return yield(sent.error());
			}
		}
		std::array<char, 8192> buffer;
		auto const received_header = receive_response_header(yield, *socket, buffer);
		if (received_header.is_error())
		{
			return yield(received_header.error());
		}
		auto const &response_header = received_header.get().first;
		std::size_t const buffered_content = received_header.get().second;

		if (response_header.status != 200)
		{
			return yield(boost::system::error_code(service_error::file_not_found));
		}

		auto content_length_header = response_header.arguments->find("Content-Length");
		if (content_length_header == response_header.arguments->end())
		{
			throw std::logic_error("todo 2");
		}

		std::vector<byte> first_part(buffer.begin(), buffer.begin() + buffered_content);
		file_offset const file_size = boost::lexical_cast<file_offset>(content_length_header->second);
		linear_file file{file_size, Si::erase_unique(Si::make_coroutine_generator<Si::error_or<Si::memory_range>>(
			[first_part, socket, file_size]
		(Si::push_context<Si::error_or<Si::memory_range>> yield)
		{
			if (!first_part.empty())
			{
				yield(Si::make_memory_range(
					reinterpret_cast<char const *>(first_part.data()),
					reinterpret_cast<char const *>(first_part.data() + first_part.size())));
			}
			file_offset receive_counter = first_part.size();
			std::array<char, 8192> buffer;
			auto receiving = Si::asio::make_reading_observable(*socket, Si::make_iterator_range(buffer.data(), buffer.data() + buffer.size()));
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
}
