#ifndef FILESERVER_RECURSIVE_DIRECTORY_WATCHER_HPP
#define FILESERVER_RECURSIVE_DIRECTORY_WATCHER_HPP

#include <silicium/single_directory_watcher.hpp>
#include <silicium/c_string.hpp>

namespace fileserver
{
	struct recursive_directory_watcher
	{
		typedef Si::error_or<std::vector<Si::file_notification>> element_type;

		explicit recursive_directory_watcher(Si::native_path_string root);

		template <class Observer>
		void async_get_one(Observer &&receiver);

	private:


	};
}

#endif
