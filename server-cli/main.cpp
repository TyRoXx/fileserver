#include <silicium/yield_context_sink.hpp>
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
#include <silicium/std_threading.hpp>
#include <silicium/virtualized_source.hpp>
#include <silicium/transforming_source.hpp>
#include <server/sha256.hpp>
#include <server/hexadecimal.hpp>
#include <server/digest.hpp>
#include <server/path.hpp>
#include <boost/interprocess/sync/null_mutex.hpp>
#include <boost/unordered_map.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <boost/container/vector.hpp>
#include <sys/stat.h>

namespace Si
{
	struct ended
	{
	};

	template <class Input>
	struct end_observable : private observer<typename Input::element_type>
	{
		using element_type = Si::ended;

		end_observable()
		{
		}

		explicit end_observable(Input input)
			: input(std::move(input))
		{
		}

		void async_get_one(observer<element_type> &receiver)
		{
			assert(!receiver_);
			if (has_ended)
			{
				return receiver.ended();
			}
			receiver_ = &receiver;
			next();
		}

	private:

		Input input;
		observer<element_type> *receiver_ = nullptr;
		bool has_ended = false;

		void next()
		{
			assert(receiver_);
			input.async_get_one(*this);
		}

		virtual void got_element(typename Input::element_type value) SILICIUM_OVERRIDE
		{
			boost::ignore_unused_variable_warning(value);
			next();
		}

		virtual void ended() SILICIUM_OVERRIDE
		{
			has_ended = true;
			Si::exchange(receiver_, nullptr)->got_element(element_type());
		}
	};

	template <class Input>
	auto make_end_observable(Input &&input)
	{
		return end_observable<typename std::decay<Input>::type>(std::forward<Input>(input));
	}
}

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

	struct in_memory_location
	{
		std::vector<char> content;
	};

	using location = Si::fast_variant<file_system_location, in_memory_location>;

	boost::uint64_t location_file_size(location const &location)
	{
		return Si::visit<boost::uint64_t>(
			location,
			[](file_system_location const &file)
		{
			return file.size;
		},
			[](in_memory_location const &memory)
		{
			return memory.content.size();
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
			yield(Si::visit<std::vector<char>>(
				*found_file,
				[](file_system_location const &location)
				{
					return Si::read_file(*location.where);
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

	struct found_file
	{
		path name;
	};

	struct finished_directory
	{
		path name;
	};

	using file_iteration_element = Si::fast_variant<found_file, finished_directory>;

	namespace detail
	{
		template <class FileIterationElementSink>
		void list_files_recursively(FileIterationElementSink &files, boost::filesystem::path const &root)
		{
			for (boost::filesystem::directory_iterator i(root); i != boost::filesystem::directory_iterator(); ++i)
			{
				switch (i->status().type())
				{
				case boost::filesystem::file_type::regular_file:
					Si::append(files, file_iteration_element{found_file{path{i->path()}}});
					break;

				case boost::filesystem::file_type::directory_file:
					list_files_recursively(files, i->path());
					break;

				default:
					//ignore
					break;
				}
			}
			Si::append(files, file_iteration_element{finished_directory{path{root}}});
		}
	}

	Si::unique_observable<file_iteration_element> list_files_recursively(boost::filesystem::path const &root)
	{
		return Si::erase_unique(Si::make_thread<file_iteration_element, Si::std_threading>([root](Si::yield_context<file_iteration_element> &yield)
		{
			Si::yield_context_sink<file_iteration_element> sink(yield);
			return detail::list_files_recursively(sink, root);
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
			auto content = Si::virtualize_source(Si::make_file_source(opened.handle, boost::make_iterator_range(buffer.data(), buffer.data() + buffer.size())));
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

		boost::optional<std::pair<digest, location>> hash_file_iteration_element(file_iteration_element const &found)
		{
			return Si::visit<boost::optional<std::pair<digest, location>>>(
				found,
				[](found_file const &file) { return hash_file(file.name.to_boost_path()); },
				[](finished_directory const &) { return boost::none; });
		}
	}

	template <class PathObservable>
	Si::unique_observable<std::pair<digest, location>> get_locations_by_hash(PathObservable &&paths)
	{
		return Si::erase_unique(Si::transform_if_initialized<std::pair<digest, location>>(std::forward<PathObservable>(paths), detail::hash_file_iteration_element));
	}

	void serve_directory(boost::filesystem::path const &served_dir)
	{
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

		auto listed = Si::for_each(
			Si::make_end_observable(
				Si::transform(
					get_locations_by_hash(
						list_files_recursively(served_dir)),
						[&files, &io](std::pair<digest, location> const &entry)
		{
			auto &out = std::cerr;
			auto const digest_bytes = fileserver::get_digest_digits(entry.first);
			encode_ascii_hex_digits(digest_bytes.begin(), digest_bytes.end(), std::ostreambuf_iterator<char>(out));
			out
				<< " "
				<< Si::visit<std::string>(
					entry.second,
					[](file_system_location const &location)
					{
						return location.where->string();
					},
					[](in_memory_location const &)
					{
						return ":memory:";
					})
				<< '\n';

			fileserver::unknown_digest key(digest_bytes.begin(), digest_bytes.end());
			io.post([key, entry, &files]() mutable
			{
				files.available.insert(std::make_pair(std::move(key), std::move(entry.second)));
			});
			return Si::nothing();
		})),
			[](Si::ended)
		{
			std::cerr << "Scan complete\n";
		});
		listed.start();

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
