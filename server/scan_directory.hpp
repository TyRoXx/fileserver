#ifndef FILESERVER_SCAN_DIRECTORY_HPP
#define FILESERVER_SCAN_DIRECTORY_HPP

#include <server/typed_reference.hpp>
#include <server/file_repository.hpp>
#include <server/directory_listing.hpp>
#include <silicium/error_or.hpp>
#include <silicium/source/virtualized_source.hpp>
#include <ventura/source/file_source.hpp>
#include <silicium/source/single_source.hpp>
#include <silicium/source/transforming_source.hpp>
#include <ventura/open.hpp>
#include <ventura/file_size.hpp>
#include <boost/filesystem/operations.hpp>

namespace fileserver
{
	namespace detail
	{
		inline Si::error_or<std::pair<typed_reference, location>> hash_file(ventura::absolute_path const &file)
		{
			Si::error_or<Si::file_handle> opening = ventura::open_reading(safe_c_str(to_native_range(file)));
			if (opening.is_error())
			{
				return opening.error();
			}
			Si::file_handle opened = opening.move_value();
			Si::optional<boost::uintmax_t> const size = ventura::file_size(opened.handle).get();
			if (!size)
			{
				// TODO: return a proper error_code for this problem
				throw std::runtime_error("hash_file works only for regular files");
			}
			std::array<char, 8192> buffer;
			auto content = Si::virtualize_source(ventura::make_file_source(
			    opened.handle, Si::make_memory_range(buffer.data(), buffer.data() + buffer.size())));
			auto hashable_content =
			    Si::make_transforming_source(content, [&buffer](ventura::file_read_result piece)
			                                 {
				                                 assert(static_cast<size_t>(piece.get().size()) <= buffer.size());
				                                 return piece.get(); // may throw
				                             });
			sha256_digest hashed = fileserver::sha256(hashable_content);
			return std::make_pair(typed_reference{blob_content_type, digest{hashed}},
			                      location{file_system_location{path(file), *size}});
		}
	}

	inline std::pair<file_repository, typed_reference> scan_directory(
	    boost::filesystem::path const &root,
	    std::function<std::pair<std::vector<char>, content_type>(directory_listing const &)> const &serialize_listing,
	    std::function<Si::error_or<std::pair<typed_reference, location>>(ventura::absolute_path const &)> const
	        &hash_file)
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
				Si::error_or<std::pair<typed_reference, location>> hashed =
				    hash_file(*ventura::absolute_path::create(i->path()));
				if (hashed.is_error())
				{
					// ignore error for now
					break;
				}
				repository.available[to_unknown_digest(hashed.get().first.referenced)].emplace_back(
				    std::move(hashed.get().second));
				add_to_listing(hashed.get().first);
				break;
			}

			case boost::filesystem::directory_file:
			{
				std::pair<file_repository, typed_reference> sub_dir =
				    scan_directory(i->path(), serialize_listing, hash_file);
				repository.merge(std::move(sub_dir.first));
				add_to_listing(sub_dir.second);
				break;
			}

			default:
				break;
			}
		}
		std::pair<std::vector<char>, content_type> typed_serialized_listing = serialize_listing(listing);
		std::vector<char> &serialized_listing = typed_serialized_listing.first;
		sha256_digest const listing_digest = sha256(Si::make_single_source(
		    Si::make_iterator_range(serialized_listing.data(), serialized_listing.data() + serialized_listing.size())));
		repository.available[to_unknown_digest(listing_digest)].emplace_back(
		    location{in_memory_location{std::move(serialized_listing)}});
		return std::make_pair(std::move(repository), typed_reference(typed_serialized_listing.second, listing_digest));
	}
}

#endif
