#ifdef _MSC_VER
//fixes "fatal error C1189: #error :  WinSock.h has already been included"
#	include <boost/asio.hpp>
#endif
#include <server/scan_directory.hpp>
#include <server/directory_listing.hpp>
#include <server/sha256.hpp>
#include <server/hexadecimal.hpp>
#include <server/path.hpp>
#include <silicium/yield_context_sink.hpp>
#include <silicium/asio/tcp_acceptor.hpp>
#include <silicium/coroutine.hpp>
#include <silicium/total_consumer.hpp>
#include <silicium/flatten.hpp>
#include <silicium/asio/socket_observable.hpp>
#include <silicium/asio/sending_observable.hpp>
#include <silicium/received_from_socket_source.hpp>
#include <silicium/transform_if_initialized.hpp>
#include <silicium/observable_source.hpp>
#include <silicium/for_each.hpp>
#include <silicium/optional.hpp>
#include <silicium/file_source.hpp>
#include <silicium/http/http.hpp>
#include <silicium/to_unique.hpp>
#include <silicium/thread.hpp>
#include <silicium/buffering_source.hpp>
#include <silicium/open.hpp>
#include <silicium/read_file.hpp>
#include <silicium/memory_source.hpp>
#include <silicium/std_threading.hpp>
#include <silicium/virtualized_source.hpp>
#include <silicium/transforming_source.hpp>
#include <silicium/end_observable.hpp>
#include <silicium/single_source.hpp>
#include <boost/interprocess/sync/null_mutex.hpp>
#include <boost/unordered_map.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <boost/container/vector.hpp>
#include <boost/algorithm/string/predicate.hpp>

namespace fileserver
{
	using response_part = Si::incoming_bytes;

	boost::iterator_range<char const *> to_range(response_part part)
	{
		return boost::make_iterator_range(part.begin, part.end);
	}

	struct content_request
	{
		unknown_digest requested_file;
	};

	template <class String>
	Si::optional<content_request> parse_request_path(String const &path)
	{
		if (path.empty())
		{
			return Si::none;
		}
		auto digest_begin = path.begin();
		if (*digest_begin == '/')
		{
			++digest_begin;
		}
		content_request request;
		auto digest = parse_digest(digest_begin, path.end());
		if (!digest)
		{
			return Si::none;
		}
		request.requested_file = std::move(*digest);
		return std::move(request);
	}

	Si::http::response_header make_not_found_response()
	{
		Si::http::response_header header;
		header.http_version = "HTTP/1.0";
		header.status = 404;
		header.status_text = "Not Found";
		(*header.arguments)["Connection"] = "close";
		return header;
	}

	std::vector<char> serialize_response(Si::http::response_header const &header)
	{
		std::vector<char> serialized;
		auto sink = Si::make_container_sink(serialized);
		Si::http::write_header(sink, header);
		return serialized;
	}

	enum class request_type
	{
		get,
		head
	};

	template <class String>
	request_type determine_request_type(String const &method)
	{
		if (boost::algorithm::iequals("HEAD", method))
		{
			return request_type::head;
		}
		else
		{
			return request_type::get;
		}
	}

	template <class MakeSender>
	void respond(
		Si::push_context<Si::nothing> &yield,
		MakeSender const &make_sender,
		Si::http::request_header const &header,
		file_repository const &repository)
	{
		auto const try_send = [&yield, &make_sender](std::vector<char> const &data)
		{
			char const * const begin = data.data();
			auto sender = make_sender(Si::incoming_bytes(begin, begin + data.size()));
			auto result = yield.get_one(sender);
			assert(result);
			return !result->is_error();
		};

		auto const request = parse_request_path(header.path);
		if (!request)
		{
			try_send(serialize_response(make_not_found_response()));
			return;
		}

		std::vector<location> const * const found_file_locations = repository.find_location(request->requested_file);
		if (!found_file_locations)
		{
			try_send(serialize_response(make_not_found_response()));
			return;
		}
		assert(!found_file_locations->empty());

		//just try the first entry for now
		auto &found_file = (*found_file_locations)[0];

		{
			Si::http::response_header response;
			response.arguments = Si::make_unique<std::map<Si::noexcept_string, Si::noexcept_string>>();
			response.http_version = "HTTP/1.0";
			response.status_text = "OK";
			response.status = 200;
			(*response.arguments)["Content-Length"] = boost::lexical_cast<Si::noexcept_string>(location_file_size(found_file));
			(*response.arguments)["Connection"] = "close";

			std::vector<char> response_header = serialize_response(response);
			if (!try_send(response_header))
			{
				return;
			}
		}

		switch (determine_request_type(header.method))
		{
		case request_type::get:
			{
				auto reading = Si::make_thread<std::vector<char>, Si::std_threading>([&](Si::push_context<std::vector<char>> &yield)
				{
					yield(Si::visit<std::vector<char>>(
						found_file,
						[](file_system_location const &location)
						{
							return Si::read_file(location.where.to_boost_path());
						},
						[](in_memory_location const &location)
						{
							return location.content;
						}
					));
				});
				boost::optional<std::vector<char>> const body = yield.get_one(reading);
				if (!body)
				{
					return;
				}

				if (body->size() != location_file_size(found_file))
				{
					return;
				}

				if (!try_send(*body))
				{
					return;
				}
				break;
			}

		case request_type::head:
			{
				break;
			}
		}
	}

	template <class ReceiveObservable, class MakeSender, class Shutdown>
	void serve_client(
		Si::push_context<Si::nothing> &yield,
		ReceiveObservable &receive,
		MakeSender const &make_sender,
		Shutdown const &shutdown,
		file_repository const &repository)
	{
		auto receive_sync = Si::virtualize_source(Si::make_observable_source(Si::ref(receive), yield));
		Si::received_from_socket_source receive_bytes(receive_sync);
		auto header = Si::http::parse_header(receive_bytes);
		if (!header)
		{
			return;
		}

		respond(yield, make_sender, *header, repository);
		shutdown();

		while (Si::get(receive_bytes))
		{
		}
	}

	//TODO: use unique_observable
	using session_handle = Si::shared_observable<Si::nothing>;

	std::pair<std::vector<char>, content_type> directory_listing_to_json_bytes(directory_listing const &listing)
	{
		std::vector<char> bytes;
		serialize_json(Si::make_container_sink(bytes), listing);
		return std::make_pair(std::move(bytes), json_listing_content_type);
	}

	void serve_directory(boost::filesystem::path const &served_dir)
	{
		boost::asio::io_service io;
		boost::asio::ip::tcp::acceptor acceptor(io, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4(), 8080));
		Si::tcp_acceptor clients(acceptor);

		std::pair<file_repository, typed_reference> const scanned = scan_directory(served_dir, directory_listing_to_json_bytes, detail::hash_file);
		std::cerr << "Scan complete. Tree hash value ";
		print(std::cerr, scanned.second);
		std::cerr << "\n";
		file_repository const &files = scanned.first;

		auto accept_all = Si::make_coroutine<session_handle>([&clients, &files](Si::push_context<session_handle> &yield)
		{
			for (;;)
			{
				auto accepted = yield.get_one(clients);
				if (!accepted)
				{
					return;
				}
				Si::visit<void>(
					*accepted,
					[&yield, &files](std::shared_ptr<boost::asio::ip::tcp::socket> socket)
					{
						auto prepare_socket = [socket, &files](Si::push_context<Si::nothing> &yield)
						{
							std::array<char, 1024> receive_buffer;
							Si::socket_observable received(*socket, boost::make_iterator_range(receive_buffer.data(), receive_buffer.data() + receive_buffer.size()));
							auto make_sender = [socket](Si::incoming_bytes sent)
							{
								return Si::sending_observable(*socket, to_range(sent));
							};
							auto shutdown = [socket]()
							{
								boost::system::error_code ec; //ignored
								socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
							};
							serve_client(yield, received, make_sender, shutdown, files);
						};
						yield(Si::erase_shared(Si::make_coroutine<Si::nothing>(prepare_socket)));
					},
					[](boost::system::error_code)
					{
						throw std::logic_error("to do");
					}
				);
			}
		});
		auto all_sessions_finished = Si::flatten<boost::mutex>(std::move(accept_all));
		auto done = Si::make_total_consumer(std::move(all_sessions_finished));
		done.start();

		io.run();
	}
}

int main(int argc, char **argv)
{
	std::string verb;
	boost::filesystem::path where = boost::filesystem::current_path();

	boost::program_options::options_description desc("Allowed options");
	desc.add_options()
	    ("help", "produce help message")
		("verb", boost::program_options::value(&verb), "what to do (serve)")
		("where", boost::program_options::value(&where), "which filesystem directory to use")
	;

	boost::program_options::positional_options_description positional;
	positional.add("verb", 1);
	positional.add("where", 1);
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

	if (verb == "serve")
	{
		fileserver::serve_directory(where);
		return 0;
	}
	else
	{
		std::cerr
			<< "Unknown verb\n"
			<< desc << "\n";
	    return 1;
	}
}
