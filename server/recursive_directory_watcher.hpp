#ifndef FILESERVER_RECURSIVE_DIRECTORY_WATCHER_HPP
#define FILESERVER_RECURSIVE_DIRECTORY_WATCHER_HPP

#include <silicium/linux/inotify.hpp>
#include <silicium/file_notification.hpp>
#include <silicium/c_string.hpp>
#include <boost/filesystem/operations.hpp>

namespace fileserver
{
	struct recursive_directory_watcher
	{
		typedef Si::error_or<std::vector<Si::file_notification>> element_type;

		explicit recursive_directory_watcher(boost::asio::io_service &io, Si::native_path_string root)
		{
			auto ec = start(io, root);
			if (!!ec)
			{
				boost::throw_exception(boost::system::system_error(ec));
			}
		}

		boost::system::error_code start(boost::asio::io_service &io, Si::native_path_string root)
		{
			m_inotify = Si::linux::inotify_observable(io);
			auto scanned_root = scan(root);
			if (scanned_root.is_error())
			{
				return scanned_root.error();
			}
			m_root = std::move(scanned_root).get();
			return {};
		}

		template <class Observer>
		void async_get_one(Observer &&receiver)
		{

		}

	private:

		Si::linux::inotify_observable m_inotify;

		struct directory
		{
			Si::linux::watch_descriptor watch;
			std::map<Si::path, directory> sub_directories;
		};

		directory m_root;

		Si::error_or<directory> scan(Si::native_path_string scanned_directory)
		{
			boost::system::error_code ec;
			boost::filesystem::directory_iterator i(scanned_directory.c_str(), ec);
			if (!!ec)
			{
				return ec;
			}
			while (i != boost::filesystem::directory_iterator())
			{
				switch (i->status().type())
				{
				case boost::filesystem::directory_file:
					{
						break;
					}

				case boost::filesystem::regular_file:
					{
						break;
					}

				default:
					break;
				}

				i.increment(ec);
				if (!!ec)
				{
					return ec;
				}
			}
			return {};
		}
	};
}

#endif
