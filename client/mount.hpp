#ifndef FILESERVER_CLIENT_MOUNT_HPP
#define FILESERVER_CLIENT_MOUNT_HPP

#include <server/path.hpp>
#include <server/digest.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace fileserver
{
	void mount_directory(fileserver::unknown_digest const &root_digest, fileserver::path const &mount_point,
	                     boost::asio::ip::tcp::endpoint const &server);
}

#endif
