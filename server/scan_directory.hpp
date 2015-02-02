#ifndef FILESERVER_SCAN_DIRECTORY_HPP
#define FILESERVER_SCAN_DIRECTORY_HPP

#include <server/typed_reference.hpp>
#include <server/file_repository.hpp>
#include <server/directory_listing.hpp>
#include <silicium/error_or.hpp>
#include <silicium/source/virtualized_source.hpp>
#include <silicium/source/file_source.hpp>
#include <silicium/source/single_source.hpp>
#include <silicium/source/transforming_source.hpp>
#include <silicium/open.hpp>
#include <silicium/file_size.hpp>
#include <boost/filesystem/operations.hpp>

namespace fileserver
{
	namespace detail
	{
		inline Si::error_or<std::pair<typed_reference, location>> hash_file(boost::filesystem::path const &file)
		{
			auto opening = Si::open_reading(file);
			if (opening.is_error())
			{
				return opening.error();
			}
			auto &&opened = std::move(opening).get();
			Si::optional<boost::uintmax_t> const size = Si::file_size(opened.handle).get();
			if (!size)
			{
				//TODO: return a proper error_code for this problem
				throw std::runtime_error("hash_file works only for regular files");
			}
			std::array<char, 8192> buffer;
			auto content = Si::virtualize_source(Si::make_file_source(opened.handle, Si::make_memory_range(buffer.data(), buffer.data() + buffer.size())));
			auto hashable_content = Si::make_transforming_source(
				content,
				[&buffer](Si::file_read_result piece)
			{
				std::size_t length = piece.get(); //may throw
				assert(length <= buffer.size());
				return boost::make_iterator_range(buffer.data(), buffer.data() + length);
			});
			auto sha256_digest = fileserver::sha256(hashable_content);
			return std::make_pair(typed_reference{blob_content_type, digest{sha256_digest}}, location{file_system_location{path(file), *size}});
		}
	}

	inline std::pair<file_repository, typed_reference> scan_directory(
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
}

#endif
