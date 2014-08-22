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
#include <fileserver/sha256.hpp>
#include <fileserver/hexadecimal.hpp>
#include <boost/interprocess/sync/null_mutex.hpp>
#include <boost/container/string.hpp>
#include <boost/unordered_map.hpp>
#include <boost/filesystem/operations.hpp>

namespace fileserver
{
	using response_part = Si::incoming_bytes;

	boost::iterator_range<char const *> to_range(response_part part)
	{
		return boost::make_iterator_range(part.begin, part.end);
	}

	using digest = boost::container::basic_string<byte>;

	struct content_request
	{
		digest requested_file;
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
	};

	using location = Si::fast_variant<file_system_location>;

	struct file_repository
	{
		boost::unordered_map<digest, location> available;

		location const *find_location(digest const &key) const
		{
			auto i = available.find(key);
			return (i == end(available)) ? nullptr : &i->second;
		}
	};

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

		auto const request = parse_request_path(header->path);
		if (!request)
		{
			//TODO: 404 or sth
			return;
		}

		location const * const found_file = repository.find_location(request->requested_file);
		if (!found_file)
		{
			//TODO: 404
			return;
		}

		std::vector<char> const body = Si::read_file(Si::visit<boost::filesystem::path>(*found_file, [](file_system_location const &location)
		{
			return *location.where;
		}));

		auto const try_send = [&yield, &make_sender](std::vector<char> const &data)
		{
			auto sender = make_sender(Si::incoming_bytes(data.data(), data.data() + data.size()));
			auto error = yield.get_one(sender);
			assert(error);
			return !*error;
		};

		{
			Si::http::response_header response;
			response.http_version = "HTTP/1.0";
			response.status_text = "OK";
			response.status = 200;
			response.arguments["Content-Length"] = boost::lexical_cast<std::string>(body.size());
			response.arguments["Connection"] = "close";

			std::vector<char> response_header;
			auto header_sink = Si::make_container_sink(response_header);
			Si::http::write_header(header_sink, response);

			if (!try_send(response_header))
			{
				return;
			}
		}

		if (!try_send(body))
		{
			return;
		}

		shutdown();

		while (Si::get(receive_bytes))
		{
		}
	}

	//TODO: use unique_observable
	using session_handle = Si::shared_observable<Si::nothing>;

	namespace detail
	{
		void list_files_recursively(Si::yield_context_2<boost::filesystem::path> &yield, boost::filesystem::path const &root)
		{
			for (boost::filesystem::directory_iterator i(root); i != boost::filesystem::directory_iterator(); ++i)
			{
				switch (i->status().type())
				{
				case boost::filesystem::file_type::regular_file:
					yield.push_result(i->path());
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
		return Si::erase_unique(Si::make_thread<boost::filesystem::path, Si::boost_threading>([root](Si::yield_context_2<boost::filesystem::path> &yield)
		{
			return detail::list_files_recursively(yield, root);
		}));
	}

	namespace detail
	{
		boost::optional<std::pair<digest, location>> hash_file(boost::filesystem::path const &file)
		{
			auto opened = Si::linux::open_reading(file);
			std::array<char, 8192> buffer;
			auto content = Si::make_file_source(opened.value().handle, boost::make_iterator_range(buffer.data(), buffer.data() + buffer.size()));
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
			digest resulting_digest;
			resulting_digest.assign(sha256_digest.bytes.begin(), sha256_digest.bytes.end());
			return std::make_pair(resulting_digest, location{file_system_location{std::make_shared<boost::filesystem::path>(file)}});
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
							socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both);
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
	auto all_sessions_finished = Si::flatten<boost::interprocess::null_mutex>(std::move(accept_all));
	auto done = Si::make_total_consumer(std::move(all_sessions_finished));
	done.start();

	auto listed = Si::for_each(get_locations_by_hash(list_files_recursively(boost::filesystem::current_path())), [&files, &io](std::pair<digest, location> const &entry)
	{
		auto &out = std::cerr;
		encode_ascii_hex_digits(entry.first.begin(), entry.first.end(), std::ostreambuf_iterator<char>(out));
		out
			<< " "
			<< Si::visit<std::string>(entry.second, [](file_system_location const &location)
			{
				return location.where->string();
			})
			<< '\n';

		io.post([entry, &files]()
		{
			files.available.insert(entry);
		});
	});
	listed.start();

	io.run();
}
