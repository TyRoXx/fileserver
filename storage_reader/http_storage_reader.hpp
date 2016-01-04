#ifndef FILESERVER_HTTP_STORAGE_READER_HPP
#define FILESERVER_HTTP_STORAGE_READER_HPP

#include "storage_reader.hpp"
#include <silicium/observable/yield_context.hpp>
#include <silicium/http/http.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace fileserver
{
	struct http_storage_reader : storage_reader
	{
		explicit http_storage_reader(boost::asio::io_service &io, boost::asio::ip::tcp::endpoint server,
		                             Si::noexcept_string relative_path);
		virtual Si::unique_observable<Si::error_or<linear_file>> open(unknown_digest const &name) SILICIUM_OVERRIDE;
		virtual Si::unique_observable<Si::error_or<file_offset>> size(unknown_digest const &name) SILICIUM_OVERRIDE;

	private:
		boost::asio::io_service *io = nullptr;
		boost::asio::ip::tcp::endpoint server;
		Si::noexcept_string relative_path;

		Si::error_or<std::shared_ptr<boost::asio::ip::tcp::socket>> connect(Si::yield_context yield);
		std::vector<char> serialize_request(Si::noexcept_string method, unknown_digest const &requested);
		Si::error_or<Si::nothing> send_all(Si::yield_context yield, boost::asio::ip::tcp::socket &socket,
		                                   std::vector<char> const &buffer);
		Si::error_or<std::pair<Si::http::response, std::size_t>>
		receive_response_header(Si::yield_context yield, boost::asio::ip::tcp::socket &socket,
		                        std::array<char, 8192> &buffer);
		void size_impl(Si::push_context<Si::error_or<file_offset>> yield, unknown_digest const &requested_name);
		void open_impl(Si::push_context<Si::error_or<linear_file>> yield, unknown_digest const &requested_name);
	};
}

#endif
