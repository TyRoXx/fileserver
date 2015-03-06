#ifndef FILESERVER_POOL_EXECUTOR_HPP
#define FILESERVER_POOL_EXECUTOR_HPP

#include <boost/asio/io_service.hpp>
#include <silicium/utility.hpp>

namespace fileserver
{
	template <class ThreadingAPI>
	struct pool_executor
	{
		pool_executor()
		{
		}

		explicit pool_executor(std::size_t threads)
			: m_immovable(Si::make_unique<immovable>())
		{
			for (std::size_t i = 0; i < threads; ++i)
			{
				boost::asio::io_service &queue = m_immovable->m_work_queue;
				m_workers.emplace_back(ThreadingAPI::launch_async([&queue]
				{
					queue.run();
				}));
			}
		}

		template <class Action>
		void submit(Action &&work)
		{
			assert(m_immovable);
			m_immovable->m_work_queue.post(std::forward<Action>(work));
		}

	private:

		struct immovable
		{
			boost::asio::io_service m_work_queue;
			boost::asio::io_service::work m_waiting_for_work;

			immovable()
				: m_waiting_for_work(m_work_queue)
			{
			}
		};

		std::unique_ptr<immovable> m_immovable;
		std::vector<typename ThreadingAPI::template future<void>::type> m_workers;
	};
}

#endif
