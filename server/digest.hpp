#ifndef FILESERVER_DIGEST_HPP
#define FILESERVER_DIGEST_HPP

#include <server/byte.hpp>
#include <boost/container/string.hpp>

namespace fileserver
{
	using digest = boost::container::basic_string<byte>;
}

#endif
