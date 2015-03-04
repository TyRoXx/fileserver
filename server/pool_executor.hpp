#ifndef FILESERVER_POOL_EXECUTOR_HPP
#define FILESERVER_POOL_EXECUTOR_HPP

#include <boost/asio/io_service.hpp>

namespace fileserver
{
	template <class ThreadingAPI>
	struct pool_executor
	{
		pool_executor()
		{
		}

		explicit pool_executor(std::size_t threads)
		{
			for (std::size_t i = 0; i < threads; ++i)
			{
				m_workers.emplace_back(ThreadingAPI::launch_async([this]
				{
					m_work_queue.run();
				}));
			}
		}

		template <class Action>
		void submit(Action &&work)
		{
			m_work_queue.post(std::forward<Action>(work));
		}

	private:

		boost::asio::io_service m_work_queue;
		std::vector<typename ThreadingAPI::template future<void>::type> m_workers;
		std::unique_ptr<boost::asio::io_service::work> m_waiting_for_work;
	};
}

#endif
