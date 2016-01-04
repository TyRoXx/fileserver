#ifndef FILESERVER_PATH_HPP
#define FILESERVER_PATH_HPP

#include <ventura/absolute_path.hpp>
#include <silicium/c_string.hpp>

namespace fileserver
{
	typedef ventura::absolute_path path;

#ifdef _WIN32
	inline Si::os_string to_native_range(ventura::absolute_path const &path)
	{
		return to_os_string(path);
	}
#else
	inline ventura::absolute_path to_native_range(ventura::absolute_path path)
	{
		return path;
	}
#endif

	inline Si::os_c_string safe_c_str(Si::os_string const &string)
	{
		return Si::os_c_string(string.c_str());
	}

	inline Si::c_string safe_c_str(ventura::absolute_path const &path)
	{
		return path.safe_c_str();
	}
}

#endif
