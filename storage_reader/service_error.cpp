#include "service_error.hpp"
#include <silicium/config.hpp>
#include <boost/concept_check.hpp>

namespace fileserver
{
	const char *service_error_category::name() const BOOST_SYSTEM_NOEXCEPT
	{
		return "service error";
	}

	std::string service_error_category::message(int ev) const
	{
		switch (ev)
		{
		case static_cast<int>(service_error::file_not_found):
			return "file not found";
		}
		SILICIUM_UNREACHABLE();
	}

	bool service_error_category::equivalent(boost::system::error_code const &code,
	                                        int condition) const BOOST_SYSTEM_NOEXCEPT
	{
		boost::ignore_unused_variable_warning(code);
		boost::ignore_unused_variable_warning(condition);
		return false;
	}

	boost::system::error_category const &get_system_error_category()
	{
		static service_error_category const instance;
		return instance;
	}
}
