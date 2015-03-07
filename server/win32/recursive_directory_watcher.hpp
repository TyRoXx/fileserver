#ifndef FILESERVER_WIN32_RECURSIVE_DIRECTORY_WATCHER_HPP
#define FILESERVER_WIN32_RECURSIVE_DIRECTORY_WATCHER_HPP

#include <silicium/error_or.hpp>
#include <silicium/file_notification.hpp>
#include <silicium/absolute_path.hpp>
#include <silicium/win32/overlapped_directory_changes.hpp>
#include <silicium/observable/transform.hpp>
#include <silicium/observable/total_consumer.hpp>
#include <boost/asio/io_service.hpp>

namespace fileserver
{
	struct recursive_directory_watcher
	{
		typedef Si::error_or<std::vector<Si::file_notification>> element_type;

		explicit recursive_directory_watcher(boost::asio::io_service &io, Si::absolute_path root)
		{
			auto ec = start(io, std::move(root));
			if (!!ec)
			{
				boost::throw_exception(boost::system::system_error(ec));
			}
		}

		boost::system::error_code start(boost::asio::io_service &io, Si::absolute_path root)
		{
			auto handle_notifications = [this](std::vector<Si::win32::file_notification> notifications) -> Si::nothing
			{
				return{};
			};
			m_changes = consumer(handler(handle_notifications, Si::win32::overlapped_directory_changes(io, root, true)));
			return{};
		}

		template <class Observer>
		void async_get_one(Observer &&receiver)
		{
		}

	private:
		
		typedef Si::transformation<
			std::function<Si::nothing(std::vector<Si::win32::file_notification>)>,
			Si::win32::overlapped_directory_changes
		> handler;

		typedef Si::total_consumer<handler> consumer;

		consumer m_changes;
	};
}

#endif
