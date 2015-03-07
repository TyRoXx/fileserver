#ifndef FILESERVER_WIN32_RECURSIVE_DIRECTORY_WATCHER_HPP
#define FILESERVER_WIN32_RECURSIVE_DIRECTORY_WATCHER_HPP

#include <silicium/error_or.hpp>
#include <silicium/file_notification.hpp>
#include <silicium/absolute_path.hpp>
#include <boost/asio/io_service.hpp>

namespace fileserver
{
	struct recursive_directory_watcher
	{
		typedef Si::error_or<std::vector<Si::file_notification>> element_type;

		explicit recursive_directory_watcher(boost::asio::io_service &io, Si::absolute_path root)
		{
		}

		boost::system::error_code start(boost::asio::io_service &io, Si::absolute_path root)
		{
		}

		template <class Observer>
		void async_get_one(Observer &&receiver)
		{
		}

	private:

	};
}

#endif
