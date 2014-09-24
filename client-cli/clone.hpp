#ifndef FILESERVER_CLIENT_CLONE_HPP
#define FILESERVER_CLIENT_CLONE_HPP

#include <server/digest.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace fileserver
{
	void clone_directory(fileserver::unknown_digest const &root_digest, boost::filesystem::path const &destination, boost::asio::ip::tcp::endpoint const &server);
}

#endif
