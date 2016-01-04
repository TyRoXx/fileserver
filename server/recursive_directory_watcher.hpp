#ifndef FILESERVER_RECURSIVE_DIRECTORY_WATCHER_HPP
#define FILESERVER_RECURSIVE_DIRECTORY_WATCHER_HPP

#ifdef _WIN32
#include <server/win32/recursive_directory_watcher.hpp>
#else
#include <server/linux/recursive_directory_watcher.hpp>
#endif

#endif
