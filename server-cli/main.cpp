#include <silicium/tcp_acceptor.hpp>
#include <silicium/coroutine.hpp>
#include <silicium/total_consumer.hpp>
#include <silicium/flatten.hpp>
#include <silicium/socket_observable.hpp>
#include <silicium/sending_observable.hpp>
#include <silicium/received_from_socket_source.hpp>
#include <silicium/transform_if_initialized.hpp>
#include <silicium/observable_source.hpp>
#include <silicium/for_each.hpp>
#include <silicium/optional.hpp>
#include <silicium/file_source.hpp>
#include <silicium/http/http.hpp>
#include <silicium/to_unique.hpp>
#include <silicium/thread.hpp>
#include <silicium/linux/open.hpp>
#include <silicium/read_file.hpp>
#include <silicium/thread.hpp>
#include <server/sha256.hpp>
#include <server/hexadecimal.hpp>
#include <server/digest.hpp>
#include <boost/interprocess/sync/null_mutex.hpp>
#include <boost/unordered_map.hpp>
#include <boost/filesystem/operations.hpp>
#include <sys/stat.h>

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

	Si::optional<content_request> parse_request_path(std::string const &path)
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
		auto const rest = decode_ascii_hex_bytes(digest_begin, path.end(), std::back_inserter(request.requested_file));
		if (rest.first != path.end())
		{
			return Si::none;
		}
		return std::move(request);
	}

	struct file_system_location
	{
		//shared_ptr for noexcept-movability and copyability
		std::shared_ptr<boost::filesystem::path> where;
		boost::uint64_t size;
	};

	using location = Si::fast_variant<file_system_location>;

	boost::uint64_t location_file_size(location const &location)
	{
		return Si::visit<boost::uint64_t>(location, [](file_system_location const &file)
		{
			return file.size;
		});
	}

	struct file_repository
	{
		boost::unordered_map<unknown_digest, location> available;

		location const *find_location(unknown_digest const &key) const
		{
			auto i = available.find(key);
			return (i == end(available)) ? nullptr : &i->second;
		}
	};

	Si::http::response_header make_not_found_response()
	{
		Si::http::response_header header;
		header.http_version = "HTTP/1.0";
		header.status = 404;
		header.status_text = "Not Found";
		header.arguments["Connection"] = "close";
		return header;
	}

	std::vector<char> serialize_response(Si::http::response_header const &header)
	{
		std::vector<char> serialized;
		auto sink = Si::make_container_sink(serialized);
		Si::http::write_header(sink, header);
		return serialized;
	}

	template <class MakeSender>
	void respond(
		Si::yield_context<Si::nothing> &yield,
		MakeSender const &make_sender,
		Si::http::request_header const &header,
		file_repository const &repository)
	{
		auto const try_send = [&yield, &make_sender](std::vector<char> const &data)
		{
			auto sender = make_sender(Si::incoming_bytes(data.data(), data.data() + data.size()));
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

		location const * const found_file = repository.find_location(request->requested_file);
		if (!found_file)
		{
			try_send(serialize_response(make_not_found_response()));
			return;
		}

		{
			Si::http::response_header response;
			response.http_version = "HTTP/1.0";
			response.status_text = "OK";
			response.status = 200;
			response.arguments["Content-Length"] = boost::lexical_cast<std::string>(location_file_size(*found_file));
			response.arguments["Connection"] = "close";

			std::vector<char> response_header = serialize_response(response);
			if (!try_send(response_header))
			{
				return;
			}
		}

		auto reading = Si::make_thread<std::vector<char>, Si::std_threading>([&](Si::yield_context<std::vector<char>> &yield)
		{
			yield(Si::read_file(Si::visit<boost::filesystem::path>(*found_file, [](file_system_location const &location)
			{
				return *location.where;
			})));
		});
		boost::optional<std::vector<char>> const body = yield.get_one(reading);
		if (!body)
		{
			return;
		}

		if (body->size() != location_file_size(*found_file))
		{
			return;
		}

		if (!try_send(*body))
		{
			return;
		}
	}

	template <class ReceiveObservable, class MakeSender, class Shutdown>
	void serve_client(
		Si::yield_context<Si::nothing> &yield,
		ReceiveObservable &receive,
		MakeSender const &make_sender,
		Shutdown const &shutdown,
		file_repository const &repository)
	{
		auto receive_sync = Si::make_observable_source(Si::ref(receive), yield);
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

	namespace detail
	{
		void list_files_recursively(Si::yield_context<boost::filesystem::path> &yield, boost::filesystem::path const &root)
		{
			for (boost::filesystem::directory_iterator i(root); i != boost::filesystem::directory_iterator(); ++i)
			{
				switch (i->status().type())
				{
				case boost::filesystem::file_type::regular_file:
					yield(i->path());
					break;

				case boost::filesystem::file_type::directory_file:
					list_files_recursively(yield, i->path());
					break;

				default:
					//ignore
					break;
				}
			}
		}
	}

	Si::unique_observable<boost::filesystem::path> list_files_recursively(boost::filesystem::path const &root)
	{
		return Si::erase_unique(Si::make_thread<boost::filesystem::path, Si::boost_threading>([root](Si::yield_context<boost::filesystem::path> &yield)
		{
			return detail::list_files_recursively(yield, root);
		}));
	}

	namespace detail
	{
		Si::error_or<boost::uint64_t> file_size(int file)
		{
			struct stat buffer;
			if (fstat(file, &buffer) < 0)
			{
				return boost::system::error_code(errno, boost::system::system_category());
			}
			return static_cast<boost::uint64_t>(buffer.st_size);
		}

		boost::optional<std::pair<digest, location>> hash_file(boost::filesystem::path const &file)
		{
			auto opening = Si::open_reading(file);
			if (opening.is_error())
			{
				return boost::none;
			}
			auto opened = std::move(opening).get();
			auto const size = file_size(opened.handle).get();
			std::array<char, 8192> buffer;
			auto content = Si::make_file_source(opened.handle, boost::make_iterator_range(buffer.data(), buffer.data() + buffer.size()));
			auto hashable_content = Si::make_transforming_source<boost::iterator_range<char const *>>(
				content,
				[&buffer](Si::file_read_result piece)
			{
				return Si::visit<boost::iterator_range<char const *>>(
					piece,
					[&buffer](std::size_t length) -> boost::iterator_range<char const *>
					{
						assert(length <= buffer.size());
						return boost::make_iterator_range(buffer.data(), buffer.data() + length);
					},
					[](boost::system::error_code error) -> boost::iterator_range<char const *>
					{
						boost::throw_exception(boost::system::system_error(error));
						SILICIUM_UNREACHABLE();
					});
			});
			auto sha256_digest = fileserver::sha256(hashable_content);
			return std::make_pair(digest{sha256_digest}, location{file_system_location{std::make_shared<boost::filesystem::path>(file), size}});
		}
	}

	template <class PathObservable>
	Si::unique_observable<std::pair<digest, location>> get_locations_by_hash(PathObservable &&paths)
	{
		return Si::erase_unique(Si::transform_if_initialized<std::pair<digest, location>>(std::forward<PathObservable>(paths), detail::hash_file));
	}
}

int main()
{
	using namespace fileserver;

	boost::asio::io_service io;
	boost::asio::ip::tcp::acceptor acceptor(io, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4(), 8080));
	Si::tcp_acceptor clients(acceptor);

	file_repository files;

	auto accept_all = Si::make_coroutine<session_handle>([&clients, &files](Si::yield_context<session_handle> &yield)
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
					auto prepare_socket = [socket, &files](Si::yield_context<Si::nothing> &yield)
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

	auto listed = Si::for_each(get_locations_by_hash(list_files_recursively(boost::filesystem::current_path())), [&files, &io](std::pair<digest, location> const &entry)
	{
		auto &out = std::cerr;
		auto const digest_bytes = fileserver::get_digest_digits(entry.first);
		encode_ascii_hex_digits(digest_bytes.begin(), digest_bytes.end(), std::ostreambuf_iterator<char>(out));
		out
			<< " "
			<< Si::visit<std::string>(entry.second, [](file_system_location const &location)
			{
				return location.where->string();
			})
			<< '\n';

		fileserver::unknown_digest key(digest_bytes.begin(), digest_bytes.end());
		io.post([key, entry, &files]() mutable
		{
			files.available.insert(std::make_pair(std::move(key), std::move(entry.second)));
		});
	});
	listed.start();

	io.run();
}
