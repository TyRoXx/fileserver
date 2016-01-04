#ifndef FILESERVER_FILE_REPOSITORY_HPP
#define FILESERVER_FILE_REPOSITORY_HPP

#include <server/location.hpp>
#include <server/digest.hpp>
#include <boost/unordered_map.hpp>

namespace fileserver
{
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
			for (auto &entry : merged.available)
			{
				std::vector<location> &locations = available[entry.first];
				for (location &location_entry : entry.second)
				{
					locations.emplace_back(std::move(location_entry));
				}
			}
		}
	};
}

#endif
