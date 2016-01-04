#ifndef FILESERVER_WIN32_RECURSIVE_DIRECTORY_WATCHER_HPP
#define FILESERVER_WIN32_RECURSIVE_DIRECTORY_WATCHER_HPP

#include <silicium/error_or.hpp>
#include <ventura/file_notification.hpp>
#include <ventura/absolute_path.hpp>
#include <ventura/win32/overlapped_directory_changes.hpp>
#include <ventura/win32/single_directory_watcher.hpp>
#include <silicium/observable/transform.hpp>
#include <silicium/observable/total_consumer.hpp>
#include <ventura/path_segment.hpp>
#include <boost/asio/io_service.hpp>

namespace fileserver
{
	struct recursive_directory_watcher
	{
		typedef Si::error_or<std::vector<ventura::file_notification>> element_type;

		explicit recursive_directory_watcher(boost::asio::io_service &io, ventura::absolute_path root)
		{
			auto ec = start(io, std::move(root));
			if (!!ec)
			{
				boost::throw_exception(boost::system::system_error(ec));
			}
		}

		boost::system::error_code start(boost::asio::io_service &io, ventura::absolute_path root)
		{
			auto do_handle_notifications = [this](
			    Si::error_or<std::vector<ventura::win32::file_notification>> notifications) -> Si::nothing
			{
				this->handle_notifications(std::move(notifications));
				return {};
			};
			m_changes = consumer(
			    handler(do_handle_notifications, ventura::win32::overlapped_directory_changes(io, root, true)));
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
			std::map<ventura::path_segment, directory> sub_directories;
		};

		typedef Si::transformation<
		    std::function<Si::nothing(Si::error_or<std::vector<ventura::win32::file_notification>>)>,
		    ventura::win32::overlapped_directory_changes> handler;

		typedef Si::total_consumer<handler> consumer;

		consumer m_changes;
		directory m_root;

		void handle_notifications(Si::error_or<std::vector<ventura::win32::file_notification>> notifications)
		{
			if (notifications.is_error())
			{
				throw std::logic_error("to do");
			}
			std::vector<ventura::file_notification> portable_notifications;
			for (ventura::win32::file_notification &notification : notifications.get())
			{
				Si::optional<Si::error_or<ventura::file_notification>> portable_notification =
				    ventura::win32::to_portable_file_notification(std::move(notification));
				if (!portable_notification)
				{
					continue;
				}

				if (portable_notification->is_error())
				{
					throw std::logic_error("to do");
				}

				switch (portable_notification->get().type)
				{
				case ventura::file_notification_type::add:
				{
					if (!portable_notification->get().is_directory)
					{
						break;
					}

					break;
				}

				case ventura::file_notification_type::remove:
				case ventura::file_notification_type::move_self:
				case ventura::file_notification_type::change_content:
				case ventura::file_notification_type::change_content_or_metadata:
				case ventura::file_notification_type::change_metadata:
				case ventura::file_notification_type::remove_self:
					break;
				}

				portable_notifications.emplace_back(portable_notification->move_value());
			}
		}
	};
}

#endif
