#ifndef FILESERVER_FILE_SERVICE_HPP
#define FILESERVER_FILE_SERVICE_HPP

#include "linear_file.hpp"
#include <server/digest.hpp>

namespace fileserver
{
	struct file_service
	{
		virtual ~file_service();
		virtual Si::unique_observable<Si::error_or<linear_file>> open(unknown_digest const &name) = 0;
		virtual Si::unique_observable<Si::error_or<file_offset>> size(unknown_digest const &name) = 0;
	};
}

#endif
