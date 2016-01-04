#ifndef FILESERVER_WIN32_RECURSIVE_DIRECTORY_WATCHER_HPP
#define FILESERVER_WIN32_RECURSIVE_DIRECTORY_WATCHER_HPP

#include <silicium/error_or.hpp>
#include <silicium/file_notification.hpp>
#include <silicium/absolute_path.hpp>
#include <silicium/win32/overlapped_directory_changes.hpp>
#include <silicium/win32/single_directory_watcher.hpp>
#include <silicium/observable/transform.hpp>
#include <silicium/observable/total_consumer.hpp>
#include <silicium/path_segment.hpp>
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
			auto do_handle_notifications = [this](
			    std::vector<Si::win32::file_notification> notifications) -> Si::nothing
			{
				this->handle_notifications(std::move(notifications));
				return {};
			};
			m_changes =
			    consumer(handler(do_handle_notifications, Si::win32::overlapped_directory_changes(io, root, true)));
			return {};
		}

		template <class Observer>
		void async_get_one(Observer &&receiver)
		{
		}

	private:
		enum class directory_state
		{
			scanning,
			scanned
		};

		struct directory
		{
			directory_state state = directory_state::scanning;
			std::map<Si::path_segment, directory> sub_directories;
		};

		typedef Si::transformation<std::function<Si::nothing(std::vector<Si::win32::file_notification>)>,
		                           Si::win32::overlapped_directory_changes> handler;

		typedef Si::total_consumer<handler> consumer;

		consumer m_changes;
		directory m_root;

		void handle_notifications(std::vector<Si::win32::file_notification> notifications)
		{
			std::vector<Si::file_notification> portable_notifications;
			for (Si::win32::file_notification &notification : notifications)
			{
				Si::optional<Si::file_notification> portable_notification =
				    Si::win32::to_portable_file_notification(std::move(notification));
				if (!portable_notification)
				{
					continue;
				}

				switch (portable_notification->type)
				{
				case Si::file_notification_type::add:
				{
					if (!portable_notification->is_directory)
					{
						break;
					}

					break;
				}

				case Si::file_notification_type::remove:
				case Si::file_notification_type::move_self:
				case Si::file_notification_type::change_content:
				case Si::file_notification_type::change_content_or_metadata:
				case Si::file_notification_type::change_metadata:
				case Si::file_notification_type::remove_self:
					break;
				}

				portable_notifications.emplace_back(std::move(*portable_notification));
			}
		}
	};
}

#endif
