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
#include <silicium/buffering_source.hpp>
#include <silicium/linux/open.hpp>
#include <silicium/read_file.hpp>
#include <silicium/memory_source.hpp>
#include <silicium/std_threading.hpp>
#include <silicium/virtualized_source.hpp>
#include <silicium/transforming_source.hpp>
#include <silicium/end_observable.hpp>
#include <silicium/single_source.hpp>
#include <server/sha256.hpp>
#include <server/hexadecimal.hpp>
#include <server/typed_reference.hpp>
#include <server/path.hpp>
#include <boost/interprocess/sync/null_mutex.hpp>
#include <boost/unordered_map.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <boost/container/vector.hpp>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
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
		path where;
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
		boost::unordered_map<unknown_digest, std::vector<location>> available;

		std::vector<location> const *find_location(unknown_digest const &key) const
		{
			auto i = available.find(key);
			return (i == end(available)) ? nullptr : &i->second;
		}

		void merge(file_repository merged)
		{
			for (auto /*non-const*/ &entry : merged.available)
			{
				auto &locations = available[entry.first];
				for (auto &location : entry.second)
				{
					locations.emplace_back(std::move(location));
				}
			}
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
			response.http_version = "HTTP/1.0";
			response.status_text = "OK";
			response.status = 200;
			response.arguments["Content-Length"] = boost::lexical_cast<std::string>(location_file_size(found_file));
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

		Si::error_or<std::pair<typed_reference, location>> hash_file(boost::filesystem::path const &file)
		{
			auto opening = Si::open_reading(file);
			if (opening.is_error())
			{
				return *opening.error();
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
			return std::make_pair(typed_reference{blob_content_type, digest{sha256_digest}}, location{file_system_location{path(file), size}});
		}
	}

	struct directory_listing
	{
		std::map<std::string, typed_reference> entries;
	};

	content_type const txt_listing_content_type = "txt_v1";

	std::pair<std::vector<char>, content_type> serialize_txt(directory_listing const &listing)
	{
		std::vector<char> serialized;
		auto sink = Si::make_container_sink(serialized);
		for (auto const &entry : listing.entries)
		{
			Si::append(sink, entry.first);
			Si::append(sink, ":");
			typed_reference const &ref = entry.second;
			Si::append(sink, ref.type.c_str());
			Si::append(sink, ":");
			auto &&digest_digits = get_digest_digits(ref.referenced);
			encode_ascii_hex_digits(digest_digits.begin(), digest_digits.end(), std::back_inserter(serialized));
			Si::append(sink, "\n");
		}
		return std::make_pair(std::move(serialized), txt_listing_content_type);
	}

	namespace detail
	{
		template <class Sink>
		struct sink_stream
		{
			using Ch = char;

			explicit sink_stream(Sink sink)
				: sink(std::move(sink))
			{
			}

			//! Read the current character from stream without moving the read cursor.
			Ch Peek() const
			{
				SILICIUM_UNREACHABLE();
			}

			//! Read the current character from stream and moving the read cursor to next character.
			Ch Take()
			{
				SILICIUM_UNREACHABLE();
			}

			//! Get the current read cursor.
			//! \return Number of characters read from start.
			size_t Tell()
			{
				SILICIUM_UNREACHABLE();
			}

			//! Begin writing operation at the current read pointer.
			//! \return The begin writer pointer.
			Ch* PutBegin()
			{
				SILICIUM_UNREACHABLE();
			}

			//! Write a character.
			void Put(Ch c)
			{
				Si::append(sink, c);
			}

			//! Flush the buffer.
			void Flush()
			{
			}

			//! End the writing operation.
			//! \param begin The begin write pointer returned by PutBegin().
			//! \return Number of characters written.
			size_t PutEnd(Ch* begin)
			{
				SILICIUM_UNREACHABLE();
			}

		private:

			Sink sink;
		};

		template <class Sink>
		auto make_sink_stream(Sink &&sink)
		{
			return sink_stream<typename std::decay<Sink>::type>(std::forward<Sink>(sink));
		}

		std::string const &get_digest_type_name(digest const &instance)
		{
			return Si::visit<std::string const &>(
				instance,
				[](sha256_digest const &) -> std::string const &
			{
				static std::string const name = "SHA256";
				return name;
			});
		}
	}

	content_type const json_listing_content_type = "json_v1";

	std::pair<std::vector<char>, content_type> serialize_json(directory_listing const &listing)
	{
		std::vector<char> serialized;
		auto stream = detail::make_sink_stream(Si::make_container_sink(serialized));
		rapidjson::PrettyWriter<decltype(stream)> writer(stream);
		writer.StartObject();
		for (auto const &entry : listing.entries)
		{
			writer.Key(entry.first.data(), entry.first.size());
			writer.StartObject();
			{
				typed_reference const &ref = entry.second;
				writer.Key("type");
				writer.String(ref.type.data(), ref.type.size());

				writer.Key("content");
				std::string content;
				auto const &content_digits = get_digest_digits(ref.referenced);
				encode_ascii_hex_digits(content_digits.begin(), content_digits.end(), std::back_inserter(content));
				writer.String(content.data(), content.size());

				writer.Key("hash");
				auto const &hash = detail::get_digest_type_name(ref.referenced);
				writer.String(hash.data(), hash.size());
			}
			writer.EndObject();
		}
		writer.EndObject();
		return std::make_pair(std::move(serialized), json_listing_content_type);
	}

	std::pair<file_repository, typed_reference> scan_directory(
		boost::filesystem::path const &root,
		std::function<std::pair<std::vector<char>, content_type> (directory_listing const &)> const &serialize_listing,
		std::function<Si::error_or<std::pair<typed_reference, location>> (boost::filesystem::path const &)> const &hash_file)
	{
		file_repository repository;
		directory_listing listing;
		for (boost::filesystem::directory_iterator i(root); i != boost::filesystem::directory_iterator(); ++i)
		{
			auto const add_to_listing = [&listing, &i](typed_reference entry)
			{
				listing.entries.emplace(std::make_pair(i->path().leaf().string(), std::move(entry)));
			};
			switch (i->status().type())
			{
			case boost::filesystem::regular_file:
				{
					auto /*non-const*/ hashed = hash_file(i->path());
					if (hashed.is_error())
					{
						//ignore error for now
						break;
					}
					repository.available[to_unknown_digest(hashed->first.referenced)].emplace_back(std::move(hashed->second));
					add_to_listing(hashed->first);
					break;
				}

			case boost::filesystem::directory_file:
				{
					auto /*non-const*/ sub_dir = scan_directory(i->path(), serialize_listing, hash_file);
					repository.merge(std::move(sub_dir.first));
					add_to_listing(sub_dir.second);
					break;
				}

			default:
				break;
			}
		}
		auto /*non-const*/ typed_serialized_listing = serialize_listing(listing);
		auto /*non-const*/ &serialized_listing = typed_serialized_listing.first;
		auto const listing_digest = sha256(Si::make_single_source(boost::make_iterator_range(serialized_listing.data(), serialized_listing.data() + serialized_listing.size())));
		repository.available[to_unknown_digest(listing_digest)].emplace_back(location{in_memory_location{std::move(serialized_listing)}});
		return std::make_pair(std::move(repository), typed_reference(typed_serialized_listing.second, listing_digest));
	}

	void serve_directory(boost::filesystem::path const &served_dir)
	{
		boost::asio::io_service io;
		boost::asio::ip::tcp::acceptor acceptor(io, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4(), 8080));
		Si::tcp_acceptor clients(acceptor);

		std::pair<file_repository, typed_reference> const scanned = scan_directory(served_dir, serialize_json, detail::hash_file);
		std::cerr << "Scan complete. Tree hash value ";
		print(std::cerr, scanned.second);
		std::cerr << "\n";
		file_repository const &files = scanned.first;

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
