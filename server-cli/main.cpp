#ifdef _MSC_VER
//fixes "fatal error C1189: #error :  WinSock.h has already been included"
#	include <boost/asio.hpp>
#endif
#include <server/scan_directory.hpp>
#include <server/directory_listing.hpp>
#include <server/sha256.hpp>
#include <server/hexadecimal.hpp>
#include <server/path.hpp>
#include <silicium/asio/tcp_acceptor.hpp>
#include <silicium/observable/coroutine_generator.hpp>
#include <silicium/observable/total_consumer.hpp>
#include <silicium/observable/flatten.hpp>
#include <silicium/asio/writing_observable.hpp>
#include <silicium/asio/reading_observable.hpp>
#include <silicium/source/received_from_socket_source.hpp>
#include <silicium/observable/transform_if_initialized.hpp>
#include <silicium/source/observable_source.hpp>
#include <silicium/observable/for_each.hpp>
#include <silicium/optional.hpp>
#include <silicium/observable/thread.hpp>
#include <silicium/source/file_source.hpp>
#include <silicium/http/http.hpp>
#include <silicium/to_unique.hpp>
#include <silicium/observable/thread_generator.hpp>
#include <silicium/source/buffering_source.hpp>
#include <silicium/open.hpp>
#include <silicium/read_file.hpp>
#include <silicium/source/memory_source.hpp>
#include <silicium/std_threading.hpp>
#include <silicium/source/virtualized_source.hpp>
#include <silicium/source/transforming_source.hpp>
#include <silicium/observable/end.hpp>
#include <silicium/source/single_source.hpp>
#include <silicium/sink/iterator_sink.hpp>
#include <boost/interprocess/sync/null_mutex.hpp>
#include <boost/unordered_map.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <boost/container/vector.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/thread/mutex.hpp>

namespace fileserver
{
	using response_part = Si::memory_range;

	struct content_request
	{
		unknown_digest requested_file;
	};

	struct root_request
	{
	};

	typedef Si::fast_variant<root_request, content_request> parsed_request;

	template <class String>
	Si::optional<parsed_request> parse_request_path(String const &path)
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
		if (digest_begin == path.end())
		{
			return root_request();
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

	Si::http::response make_not_found_response()
	{
		Si::http::response header;
		header.http_version = "HTTP/1.0";
		header.status = 404;
		header.status_text = "Not Found";
		header.arguments = Si::make_unique<Si::http::response::arguments_table>();
		(*header.arguments)["Connection"] = "close";
		return header;
	}

	std::vector<char> serialize_response(Si::http::response const &header)
	{
		std::vector<char> serialized;
		auto sink = Si::make_container_sink(serialized);
		Si::http::generate_response(sink, header);
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
		Si::http::request const &header,
		file_repository const &repository,
		digest const &root)
	{
		auto const try_send = [&yield, &make_sender](std::vector<char> const &data)
		{
			char const * const begin = data.data();
			auto sender = make_sender(Si::make_memory_range(begin, begin + data.size()));
			auto result = yield.get_one(sender);
			assert(result);
			return !*result;
		};

		auto const request = parse_request_path(header.path);
		if (!request)
		{
			try_send(serialize_response(make_not_found_response()));
			return;
		}

		std::vector<location> const * const found_file_locations = repository.find_location(
			Si::visit<unknown_digest>(
				*request,
				[&root](root_request) -> unknown_digest { return to_unknown_digest(root); },
				[](content_request const &content) -> unknown_digest
				{
					return content.requested_file;
				}
			)
		);
		if (!found_file_locations)
		{
			try_send(serialize_response(make_not_found_response()));
			return;
		}
		assert(!found_file_locations->empty());

		//just try the first entry for now
		auto &found_file = (*found_file_locations)[0];

		{
			Si::http::response response;
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
				auto reading = Si::make_thread_generator<std::vector<char>, Si::std_threading>([&](Si::push_context<std::vector<char>> &yield) -> Si::nothing
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
					return {};
				});
				auto const &body = yield.get_one(reading);
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
		file_repository const &repository,
		digest const &root)
	{
		auto receive_sync = Si::virtualize_source(Si::make_observable_source(Si::ref(receive), yield));
		Si::received_from_socket_source receive_bytes(receive_sync);
		auto header = Si::http::parse_request(receive_bytes);
		if (!header)
		{
			return;
		}

		respond(yield, make_sender, *header, repository, root);
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
		auto clients = Si::asio::make_tcp_acceptor(&acceptor);

		std::pair<file_repository, typed_reference> const scanned = scan_directory(served_dir, directory_listing_to_json_bytes, detail::hash_file);
		std::cerr << "Scan complete. Tree hash value ";
		typed_reference const &root = scanned.second;
		print(std::cerr, root);
		std::cerr << "\n";
		file_repository const &files = scanned.first;
		digest const &root_digest = root.referenced;

		auto accept_all = Si::make_coroutine_generator<session_handle>([&clients, &files, &root_digest](Si::push_context<session_handle> &yield)
		{
			for (;;)
			{
				auto accepted = yield.get_one(clients);
				if (!accepted)
				{
					return;
				}
				std::shared_ptr<boost::asio::ip::tcp::socket> socket = accepted->get(); //TODO handle error
				auto prepare_socket = [socket, &files, &root_digest](Si::push_context<Si::nothing> &yield)
				{
					std::array<char, 1024> receive_buffer;
					auto received = Si::asio::make_reading_observable(*socket, Si::make_iterator_range(receive_buffer.data(), receive_buffer.data() + receive_buffer.size()));
					auto make_sender = [socket](Si::memory_range sent)
					{
						auto sender = Si::asio::make_writing_observable(*socket);
						sender.set_buffer(sent);
						return sender;
					};
					auto shutdown = [socket]()
					{
						boost::system::error_code ec; //ignored
						socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
					};
					serve_client(yield, received, make_sender, shutdown, files, root_digest);
				};
				yield(Si::erase_shared(Si::make_coroutine_generator<Si::nothing>(prepare_socket)));
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
