#ifndef FILESERVER_RECURSIVE_DIRECTORY_WATCHER_HPP
#define FILESERVER_RECURSIVE_DIRECTORY_WATCHER_HPP

#include "pool_executor.hpp"
#include <silicium/linux/inotify.hpp>
#include <silicium/file_notification.hpp>
#include <silicium/c_string.hpp>
#include <silicium/observable/erased_observer.hpp>
#include <silicium/observable/transform.hpp>
#include <silicium/std_threading.hpp>
#include <silicium/asio/posting_observable.hpp>
#include <silicium/linux/single_directory_watcher.hpp>
#include <silicium/observable/total_consumer.hpp>
#include <silicium/utility.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/asio/strand.hpp>

namespace fileserver
{
	namespace detail
	{
		template <class OutputIterator>
		void convert_to_portable_notifications_generic(std::vector<Si::linux::file_notification> &&linux_notifications, OutputIterator portable_out)
		{
			for (Si::linux::file_notification &linux_notification : linux_notifications)
			{
				auto portable = Si::linux::to_portable_file_notification(std::move(linux_notification));
				if (!portable)
				{
					continue;
				}
				*portable_out = std::move(*portable);
				++portable_out;
			}
		}

		std::vector<Si::file_notification> convert_to_portable_notifications(std::vector<Si::linux::file_notification> &&linux_notifications)
		{
			std::vector<Si::file_notification> portable_notifications;
			portable_notifications.reserve(linux_notifications.size());
			convert_to_portable_notifications_generic(std::move(linux_notifications), std::back_inserter(portable_notifications));
			return portable_notifications;
		}
	}

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
			m_root_strand = Si::make_unique<boost::asio::io_service::strand>(io);
			m_inotify = notification_consumer(
				notification_handler(
					[this](std::vector<Si::linux::file_notification> notifications)
					{
						handle_file_notifications(std::move(notifications));
						return Si::nothing();
					},
					Si::asio::posting_observable<Si::linux::inotify_observable, boost::asio::io_service::strand>(
						*m_root_strand,
						Si::linux::inotify_observable(io)
					)
				)
			);
			m_scanners = pool_executor<Si::std_threading>(std::thread::hardware_concurrency());
			m_root_strand->dispatch([this, root_path = Si::path(root.c_str())]() mutable
			{
				begin_scan(nullptr, std::move(root_path));
			});
			m_inotify.start();
			return {};
		}

		template <class Observer>
		void async_get_one(Observer &&receiver)
		{
			//TODO: avoid the additional indirection (shared_ptr -> unique_ptr -> observer)
			auto movable_receiver = Si::to_shared(Si::erased_observer<element_type>(std::forward<Observer>(receiver)));
			m_root_strand->dispatch(
				[movable_receiver, this]()
			{
				Si::visit<void>(
					m_receiver_or_result,
					[this, movable_receiver](Si::nothing)
					{
						m_receiver_or_result = std::move(*movable_receiver);
					},
					[](Si::erased_observer<element_type> &)
					{
						SILICIUM_UNREACHABLE();
					},
					[this, movable_receiver](Si::error_or<std::vector<Si::file_notification>> &existing_result)
					{
						element_type result = std::move(existing_result);
						m_receiver_or_result = Si::nothing();
						movable_receiver->got_element(std::move(result));
					}
				);
			});
		}

	private:

		struct directory
		{
			Si::path absolute_path;
			Si::linux::watch_descriptor watch;
			std::map<Si::path, directory> sub_directories;
		};

		typedef Si::transformation<
			std::function<Si::nothing (std::vector<Si::linux::file_notification>)>,
			Si::asio::posting_observable<
				Si::linux::inotify_observable,
				boost::asio::io_service::strand
			>
		> notification_handler;

		typedef Si::total_consumer<notification_handler> notification_consumer;

		std::unique_ptr<boost::asio::io_service::strand> m_root_strand;
		notification_consumer m_inotify;
		directory m_root;
		std::map<int, directory *> m_watch_descriptor_to_directory;
		Si::fast_variant<
			Si::nothing,
			Si::erased_observer<element_type>,
			Si::error_or<std::vector<Si::file_notification>>
		> m_receiver_or_result;
		pool_executor<Si::std_threading> m_scanners;

		directory *find_directory_by_watch_descriptor(int wd) const BOOST_NOEXCEPT
		{
			assert(wd >= 0);
			auto i = m_watch_descriptor_to_directory.find(wd);
			if (i == m_watch_descriptor_to_directory.end())
			{
				return nullptr;
			}
			assert(i->second);
			return i->second;
		}

		void handle_file_notifications(std::vector<Si::linux::file_notification> notifications)
		{
			// precondition: Method is running on the root strand
			assert(m_root_strand->running_in_this_thread());

			for (Si::linux::file_notification const &notification : notifications)
			{
				auto type = Si::linux::to_portable_file_notification_type(notification.mask);
				if (!type)
				{
					continue;
				}
				switch (*type)
				{
				case Si::file_notification_type::add:
					{
						if ((notification.mask & IN_ISDIR) != IN_ISDIR)
						{
							break;
						}
						directory * const parent_of_the_new_dir = find_directory_by_watch_descriptor(notification.watch_descriptor);
						if (!parent_of_the_new_dir)
						{
							break;
						}
						begin_scan(parent_of_the_new_dir, parent_of_the_new_dir->absolute_path / notification.name);
						break;
					}

				case Si::file_notification_type::remove:
				case Si::file_notification_type::move_self:
				case Si::file_notification_type::remove_self:
				case Si::file_notification_type::change_content:
				case Si::file_notification_type::change_metadata:
				case Si::file_notification_type::change_content_or_metadata:
					break;
				}
			}
			notify_observer(std::move(notifications));
		}

		void notify_observer(Si::error_or<std::vector<Si::linux::file_notification>> &&notifications)
		{
			// precondition: Method is running on the root strand
			assert(m_root_strand->running_in_this_thread());

			Si::visit<void>(
				m_receiver_or_result,
				[this, &notifications](Si::nothing)
				{
					m_receiver_or_result = Si::map(std::move(notifications), &detail::convert_to_portable_notifications);
				},
				[this, &notifications](Si::erased_observer<element_type> &receiver)
				{
					auto receiver_on_stack = std::move(receiver);
					m_receiver_or_result = Si::nothing();
					receiver_on_stack.got_element(Si::map(std::move(notifications), &detail::convert_to_portable_notifications));
				},
				[this, &notifications](Si::error_or<std::vector<Si::file_notification>> &existing_result)
				{
					boost::ignore_unused_variable_warning(existing_result);
					throw std::logic_error("todo");
				}
			);
		}

		void begin_scan(directory *parent, Si::path directory_to_scan)
		{
			assert(m_root_strand->running_in_this_thread());

			directory *scanned;
			if (parent)
			{
				Si::path name = leaf(directory_to_scan);
				directory &child = parent->sub_directories[name];
				scanned = &child;
			}
			else
			{
				scanned = &m_root;
			}
			scanned->absolute_path = directory_to_scan;
			scanned->watch = m_inotify.get_input().get_input().get_input().watch(directory_to_scan.c_str(), IN_ALL_EVENTS).get();
			m_watch_descriptor_to_directory.insert(std::make_pair(scanned->watch.get_watch_descriptor(), scanned));

			recursive_directory_watcher &shared_this = *this;
			m_scanners.submit([&shared_this, scanned, directory_to_scan = std::move(directory_to_scan)]() mutable
			{
				auto result = scan(
					*scanned,
					std::move(directory_to_scan),
					shared_this
				);
				shared_this.m_root_strand->dispatch([&shared_this, scanned, result = std::move(result)]() mutable
				{
					shared_this.notify_observer(std::move(result));
				});
			});
		}

		static Si::error_or<std::vector<Si::linux::file_notification>> scan(
			directory &scanned,
			Si::path directory_to_scan,
			recursive_directory_watcher &shared_this)
		{
			boost::system::error_code ec;
			boost::filesystem::directory_iterator i(directory_to_scan.c_str(), ec);
			if (!!ec)
			{
				return ec;
			}

			std::vector<Si::linux::file_notification> artificial_notifications;
			while (i != boost::filesystem::directory_iterator())
			{
				auto leaf = i->path().leaf();
				Si::path sub_name(leaf);

				switch (i->status().type())
				{
				case boost::filesystem::directory_file:
					{
						{
							Si::path child(i->path().c_str());
							shared_this.m_root_strand->dispatch([&shared_this, &scanned, child = std::move(child)]() mutable
							{
								shared_this.begin_scan(&scanned, std::move(child));
							});
						}
						artificial_notifications.emplace_back(IN_ISDIR | IN_CREATE, std::move(sub_name), -1);
						break;
					}

				case boost::filesystem::regular_file:
					{
						artificial_notifications.emplace_back(IN_CREATE, std::move(sub_name), -1);
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
			return std::move(artificial_notifications);
		}
	};
}

#endif
