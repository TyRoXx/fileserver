#ifndef FILESERVER_STORAGE_READER_HPP
#define FILESERVER_STORAGE_READER_HPP

#include "linear_file.hpp"
#include <server/digest.hpp>

namespace fileserver
{
	struct storage_reader
	{
		virtual ~storage_reader();
		virtual Si::unique_observable<Si::error_or<linear_file>> open(unknown_digest const &name) = 0;
		virtual Si::unique_observable<Si::error_or<file_offset>> size(unknown_digest const &name) = 0;
	};
}

#endif
