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

namespace fileserver
{
	namespace
	{
		boost::system::error_code clone_recursively(file_service &service, unknown_digest const &tree_digest, boost::filesystem::path const &destination, Si::yield_context yield)
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
						auto maybe_file = Si::create_file(destination / entry.first);
						if (maybe_file.error())
						{
							return *maybe_file.error();
						}
						//TODO write file
					}
					else if (entry.second.type == "json_v1")
					{
						auto ec = clone_recursively(service, to_unknown_digest(entry.second.referenced), destination / entry.first, yield);
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
			auto ec = clone_recursively(service, root_digest, destination, yield);
			if (ec)
			{
				Si::detail::throw_system_error(ec);
			}
		}));
		all.start();
		io.run();
	}
}
