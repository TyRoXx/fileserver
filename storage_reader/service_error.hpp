#ifndef FILESERVER_SERVICE_ERROR_HPP
#define FILESERVER_SERVICE_ERROR_HPP

#include <silicium/config.hpp>
#include <boost/system/system_error.hpp>

namespace fileserver
{
	enum class service_error
	{
		file_not_found
	};

	struct service_error_category : boost::system::error_category
	{
		virtual const char *name() const BOOST_SYSTEM_NOEXCEPT SILICIUM_OVERRIDE;
		virtual std::string message(int ev) const SILICIUM_OVERRIDE;
		virtual bool equivalent(boost::system::error_code const &code,
		                        int condition) const BOOST_SYSTEM_NOEXCEPT SILICIUM_OVERRIDE;
	};

	boost::system::error_category const &get_system_error_category();

	inline boost::system::error_code make_error_code(service_error error)
	{
		return boost::system::error_code(static_cast<int>(error), get_system_error_category());
	}
}

namespace boost
{
	namespace system
	{
		template <>
		struct is_error_code_enum<fileserver::service_error> : std::true_type
		{
		};
	}
}

#endif
