#ifndef FILESERVER_LINEAR_FILE_HPP
#define FILESERVER_LINEAR_FILE_HPP

#include "service_error.hpp"
#include <silicium/ptr_observable.hpp>
#include <silicium/error_or.hpp>
#include <silicium/socket_observable.hpp>
#include <cstdint>

namespace fileserver
{
	using file_offset = std::intmax_t;

	struct linear_file
	{
		file_offset size;
		Si::unique_observable<Si::error_or<Si::incoming_bytes>> content;

#ifdef _MSC_VER
		linear_file()
		{
		}

		linear_file(file_offset size, Si::unique_observable<Si::error_or<Si::incoming_bytes>> content)
			: size(size)
			, content(std::move(content))
		{
		}

		linear_file(linear_file &&other)
			: size(other.size)
			, content(std::move(other.content))
		{
		}

		linear_file &operator = (linear_file &&other)
		{
			size = other.size;
			content = std::move(other.content);
			return *this;
		}

		SILICIUM_DELETED_FUNCTION(linear_file(linear_file const &))
		SILICIUM_DELETED_FUNCTION(linear_file &operator = (linear_file const &))
#endif
	};
}

#endif
