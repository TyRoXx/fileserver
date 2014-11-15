#include <client/clone.hpp>
#include <silicium/observable/for_each.hpp>
#include <silicium/observable/coroutine_generator.hpp>
#include <silicium/observable/ready_future.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/unordered_map.hpp>

namespace Si
{
	template <class T>
	struct is_optional : std::false_type
	{
	};

	template <class T>
	struct is_optional<boost::optional<T>> : std::true_type
	{
	};

	template <class Optional, class Transformation, class = typename std::enable_if<is_optional<typename std::decay<Optional>::type>::value, void>::type>
	auto map(Optional &&value, Transformation &&transform)
		-> boost::optional<decltype(std::forward<Transformation>(transform)(*std::forward<Optional>(value)))>
	{
		if (value)
		{
			return std::forward<Transformation>(transform)(*std::forward<Optional>(value));
		}
		return boost::none;
	}
}

namespace
{
	template <class To, class From>
	To sign_cast(From from)
	{
		BOOST_STATIC_ASSERT(std::is_integral<To>::value);
		BOOST_STATIC_ASSERT(std::is_integral<From>::value);
		BOOST_STATIC_ASSERT(sizeof(To) == sizeof(From));
		return static_cast<To>(from);
	}

	struct writeable_file : fileserver::writeable_file
	{
		virtual boost::system::error_code seek(fileserver::file_offset destination) SILICIUM_OVERRIDE
		{
			BOOST_FAIL("unexpected seek");
			return boost::system::error_code();
		}

		virtual boost::system::error_code write(Si::memory_range const &written) SILICIUM_OVERRIDE
		{
			return boost::system::error_code();
		}
	};

	struct readonly_directory_manipulator : fileserver::directory_manipulator
	{
		explicit readonly_directory_manipulator(boost::filesystem::path location)
			: location(std::move(location))
		{
		}

		virtual boost::system::error_code require_exists() SILICIUM_OVERRIDE
		{
			return {};
		}

		virtual std::unique_ptr<fileserver::directory_manipulator> edit_subdirectory(std::string const &name) SILICIUM_OVERRIDE
		{
			return Si::make_unique<readonly_directory_manipulator>(location / name);
		}

		virtual Si::error_or<std::unique_ptr<fileserver::writeable_file>> create_regular_file(std::string const &name) SILICIUM_OVERRIDE
		{
			return boost::system::error_code(EACCES, boost::system::system_category());
		}

		virtual Si::error_or<fileserver::read_write_file> read_write_regular_file(std::string const &name) SILICIUM_OVERRIDE
		{
			return boost::system::error_code(EACCES, boost::system::system_category());
		}

	private:

		boost::filesystem::path location;
	};

	struct file_service : fileserver::file_service
	{
		boost::unordered_map<fileserver::unknown_digest, std::shared_ptr<std::vector<char> const>> files;

		virtual Si::unique_observable<Si::error_or<fileserver::linear_file>> open(fileserver::unknown_digest const &name) SILICIUM_OVERRIDE
		{
			auto it = files.find(name);
			if (it == files.end())
			{
				return Si::erase_unique(Si::make_ready_future(Si::error_or<fileserver::linear_file>(boost::system::error_code(fileserver::service_error::file_not_found))));
			}
			auto file = it->second;
			return Si::erase_unique(Si::make_ready_future(Si::error_or<fileserver::linear_file>(
				fileserver::linear_file{
					sign_cast<fileserver::file_offset>(file->size()),
					Si::erase_unique(Si::make_coroutine_generator<Si::error_or<Si::memory_range>>([file](Si::push_context<Si::error_or<Si::memory_range>> push)
						{
							push(Si::make_memory_range(file->data(), file->data() + file->size()));
						}))
				}
			)));
		}

		virtual Si::unique_observable<Si::error_or<fileserver::file_offset>> size(fileserver::unknown_digest const &name) SILICIUM_OVERRIDE
		{
			auto it = files.find(name);
			if (it == files.end())
			{
				return Si::erase_unique(Si::make_ready_future(Si::error_or<fileserver::file_offset>(boost::system::error_code(fileserver::service_error::file_not_found))));
			}
			return Si::erase_unique(Si::make_ready_future(Si::error_or<fileserver::file_offset>(it->second->size())));
		}
	};
}

BOOST_AUTO_TEST_CASE(client_clone_empty)
{
	fileserver::unknown_digest const root;
	readonly_directory_manipulator dir(".");
	boost::asio::io_service io;
	file_service service;
	service.files.insert(std::make_pair(root, Si::to_shared(std::vector<char>{'{', '}'})));
	bool has_finished = false;
	auto all = Si::for_each(fileserver::clone_directory(root, dir, service, io), [&has_finished](boost::system::error_code ec)
	{
		has_finished = true;
		BOOST_CHECK_EQUAL(boost::system::error_code(), ec);
	});
	all.start();
	io.run();
	BOOST_CHECK(has_finished);
}
