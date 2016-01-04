#ifndef FILESERVER_LOCATION_HPP
#define FILESERVER_LOCATION_HPP

#include <server/path.hpp>
#include <silicium/variant.hpp>

namespace fileserver
{
	struct file_system_location
	{
		path where;
		boost::uint64_t size;
	};

	struct in_memory_location
	{
		std::vector<char> content;
	};

	using location = Si::variant<file_system_location, in_memory_location>;

	boost::uint64_t location_file_size(location const &location)
	{
		return Si::visit<boost::uint64_t>(location,
		                                  [](file_system_location const &file)
		                                  {
			                                  return file.size;
			                              },
		                                  [](in_memory_location const &memory)
		                                  {
			                                  return memory.content.size();
			                              });
	}
}

#endif
