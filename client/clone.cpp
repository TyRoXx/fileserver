#include "clone.hpp"
#include "storage_reader/http_storage_reader.hpp"
#include <server/directory_listing.hpp>
#include <silicium/source/virtualized_source.hpp>
#include <silicium/source/observable_source.hpp>
#include <silicium/source/received_from_socket_source.hpp>
#include <silicium/observable/ref.hpp>
#include <silicium/observable/coroutine_generator.hpp>
#include <silicium/observable/total_consumer.hpp>
#include <silicium/open.hpp>
#include <silicium/source/observable_source.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/write.hpp>

namespace fileserver
{
	namespace
	{
		template <class IncomingBytesSource>
		boost::system::error_code copy_bytes(IncomingBytesSource &&from, file_offset copied_size, writeable_file &to)
		{
			file_offset total_written = 0;
			while (total_written < copied_size)
			{
				Si::error_or<Si::memory_range> const received = *Si::get(from);
				if (received.is_error())
				{
					return received.error();
				}
				if (received->size() == 0)
				{
					break;
				}
				if (static_cast<file_offset>(received->size()) > (copied_size - total_written))
				{
					throw std::logic_error("todo received too much");
				}
				boost::system::error_code const written = to.write(received.get());
				if (written)
				{
					return written;
				}
				total_written += received->size();
			}
			return boost::system::error_code();
		}

		boost::system::error_code clone_regular_file(storage_reader &service, std::string const &file_name, unknown_digest const &blob_digest, directory_manipulator &destination, Si::yield_context yield)
		{
			Si::error_or<linear_file> maybe_remote_file;
			yield.get_one(service.open(blob_digest), maybe_remote_file);
			if (maybe_remote_file.is_error())
			{
				return maybe_remote_file.error();
			}
			linear_file remote_file = std::move(maybe_remote_file.get());
			Si::error_or<std::unique_ptr<writeable_file>> maybe_local_file = destination.create_regular_file(file_name);
			if (maybe_local_file.is_error())
			{
				return maybe_local_file.error();
			}
			auto content_source = Si::make_observable_source(Si::ref(remote_file.content), yield);
			std::unique_ptr<writeable_file> const local_file = std::move(maybe_local_file.get());
			return copy_bytes(content_source, remote_file.size, *local_file);
		}

		boost::system::error_code clone_recursively(storage_reader &service, unknown_digest const &tree_digest, directory_manipulator &destination, Si::yield_context yield, boost::asio::io_service &io)
		{
			{
				boost::system::error_code const ec = destination.require_exists();
				if (ec)
				{
					return ec;
				}
			}
			Si::error_or<linear_file> maybe_tree_file;
			yield.get_one(service.open(tree_digest), maybe_tree_file);
			if (maybe_tree_file.is_error())
			{
				return maybe_tree_file.error();
			}
			linear_file tree_file = std::move(maybe_tree_file.get());
			auto receiving_source = Si::virtualize_source(Si::make_observable_source(Si::ref(tree_file.content), yield));
			Si::received_from_socket_source content_source(receiving_source);
			Si::fast_variant<std::unique_ptr<fileserver::directory_listing>, std::size_t> parsed = deserialize_json(std::move(content_source));
			return Si::visit<boost::system::error_code>(
				parsed,
				[&](std::unique_ptr<directory_listing> const &listing) -> boost::system::error_code
			{
				for (auto const &entry : listing->entries)
				{
					if (entry.second.type == "blob")
					{
						boost::system::error_code const ec = clone_regular_file(service, entry.first, to_unknown_digest(entry.second.referenced), destination, yield);
						if (ec)
						{
							return ec;
						}
					}
					else if (entry.second.type == "json_v1")
					{
						boost::system::error_code const ec = clone_recursively(service, to_unknown_digest(entry.second.referenced), *destination.edit_subdirectory(entry.first), yield, io);
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

	Si::unique_observable<boost::system::error_code>
	clone_directory(unknown_digest const &root_digest, directory_manipulator &destination, storage_reader &server, boost::asio::io_service &io)
	{
		return Si::erase_unique(Si::make_coroutine_generator<boost::system::error_code>([&root_digest, &destination, &server, &io](Si::push_context<boost::system::error_code> yield)
		{
			boost::system::error_code const ec = clone_recursively(server, root_digest, destination, yield, io);
			yield(ec);
		}));
	}
}
