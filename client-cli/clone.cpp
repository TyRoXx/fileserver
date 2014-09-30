#include "clone.hpp"
#include "http_file_service.hpp"
#include <server/directory_listing.hpp>
#include <silicium/virtualized_source.hpp>
#include <silicium/observable_source.hpp>
#include <silicium/received_from_socket_source.hpp>
#include <silicium/ref.hpp>
#include <silicium/coroutine.hpp>
#include <silicium/total_consumer.hpp>
#include <silicium/open.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/write.hpp>

namespace fileserver
{
	namespace
	{
		boost::system::error_code write_all(Si::native_file_handle destination, boost::iterator_range<char const *> buffer)
		{
			std::size_t total_written = 0;
			while (total_written < buffer.size())
			{
				ssize_t rc = write(destination, buffer.begin() + total_written, buffer.size() - total_written);
				if (rc < 0)
				{
					return boost::system::error_code(errno, boost::system::native_ecat);
				}
				total_written += static_cast<size_t>(rc);
			}
			return boost::system::error_code();
		}

		boost::system::error_code clone_recursively(file_service &service, unknown_digest const &tree_digest, boost::filesystem::path const &destination, Si::yield_context yield, boost::asio::io_service &io)
		{
			{
				boost::system::error_code ec;
				boost::filesystem::create_directories(destination, ec);
				if (ec)
				{
					return ec;
				}
			}
			auto tree_file_opening = service.open(tree_digest);
			boost::optional<Si::error_or<linear_file>> maybe_tree_file = yield.get_one(tree_file_opening);
			if (maybe_tree_file->is_error())
			{
				return *maybe_tree_file->error();
			}
			linear_file tree_file = std::move(*maybe_tree_file).get();
			auto receiving_source = Si::virtualize_source(Si::make_observable_source(Si::ref(tree_file.content), yield));
			Si::received_from_socket_source content_source(receiving_source);
			auto parsed = deserialize_json(std::move(content_source));
			return Si::visit<boost::system::error_code>(
				parsed,
				[&](std::unique_ptr<directory_listing> const &listing) -> boost::system::error_code
			{
				for (auto const &entry : listing->entries)
				{
					if (entry.second.type == "blob")
					{
						auto opening_remote = service.open(to_unknown_digest(entry.second.referenced));
						auto maybe_remote_file = *yield.get_one(opening_remote);
						if (maybe_remote_file.error())
						{
							return *maybe_remote_file.error();
						}
						linear_file remote_file = std::move(maybe_remote_file).get();
						auto maybe_local_file = Si::create_file(destination / entry.first);
						if (maybe_local_file.error())
						{
							return *maybe_local_file.error();
						}
						auto local_file = std::move(maybe_local_file).get();
						file_offset total_written = 0;
						while (total_written < remote_file.size)
						{
							Si::error_or<Si::incoming_bytes> const received = *yield.get_one(remote_file.content);
							if (received.error())
							{
								return *received.error();
							}
							if (received->size() == 0)
							{
								break;
							}
							if (static_cast<file_offset>(received->size()) > (remote_file.size - total_written))
							{
								throw std::logic_error("todo received too much");
							}
							boost::system::error_code const written = write_all(local_file.handle, boost::make_iterator_range(received->begin, received->end));
							if (written)
							{
								return written;
							}
							total_written += received->size();
						}
					}
					else if (entry.second.type == "json_v1")
					{
						auto ec = clone_recursively(service, to_unknown_digest(entry.second.referenced), destination / entry.first, yield, io);
						if (ec)
						{
							return ec;
						}
					}
					else
					{
						throw std::logic_error("unknown directory entry type"); //TODO
					}
				}
				return boost::system::error_code();
			},
				[](std::size_t) -> boost::system::error_code
			{
				throw std::logic_error("todo");
			});
		}
	}

	void clone_directory(unknown_digest const &root_digest, boost::filesystem::path const &destination, boost::asio::ip::tcp::endpoint const &server)
	{
		boost::asio::io_service io;
		http_file_service service(io, server);
		auto all = Si::make_total_consumer(Si::make_coroutine<Si::nothing>([&](Si::push_context<Si::nothing> yield)
		{
			auto ec = clone_recursively(service, root_digest, destination, yield, io);
			if (ec)
			{
				Si::detail::throw_system_error(ec);
			}
		}));
		all.start();
		io.run();
	}
}
