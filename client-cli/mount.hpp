#ifndef FILESERVER_CLIENT_MOUNT_HPP
#define FILESERVER_CLIENT_MOUNT_HPP

#include <server/digest.hpp>
#include <boost/filesystem/path.hpp>

namespace fileserver
{
	void mount_directory(fileserver::unknown_digest const &root_digest, boost::filesystem::path const &mount_point);
}

#endif
