#include "mount.hpp"
#include <server/path.hpp>
#include <fuse.h>

namespace fileserver
{
	namespace
	{
		char const * const hello_path = "/hello";
		char const * const hello_str = "Hello, fuse!\n";

		int hello_getattr(const char *path, struct stat *stbuf)
		{
			int res = 0;

			memset(stbuf, 0, sizeof(struct stat));
			if (strcmp(path, "/") == 0) {
				stbuf->st_mode = S_IFDIR | 0755;
				stbuf->st_nlink = 2;
			} else if (strcmp(path, hello_path) == 0) {
				stbuf->st_mode = S_IFREG | 0444;
				stbuf->st_nlink = 1;
				stbuf->st_size = strlen(hello_str);
			} else
				res = -ENOENT;

			return res;
		}

		static int hello_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
					 off_t offset, struct fuse_file_info *fi)
		{
			(void) offset;
			(void) fi;

			if (strcmp(path, "/") != 0)
				return -ENOENT;

			filler(buf, ".", NULL, 0);
			filler(buf, "..", NULL, 0);
			filler(buf, hello_path + 1, NULL, 0);

			return 0;
		}

		static int hello_open(const char *path, struct fuse_file_info *fi)
		{
			if (strcmp(path, hello_path) != 0)
				return -ENOENT;

			if ((fi->flags & 3) != O_RDONLY)
				return -EACCES;

			return 0;
		}

		static int hello_read(const char *path, char *buf, size_t size, off_t offset,
					  struct fuse_file_info *fi)
		{
			size_t len;
			(void) fi;
			if(strcmp(path, hello_path) != 0)
				return -ENOENT;

			len = strlen(hello_str);
			if (static_cast<size_t>(offset) < len) {
				if (offset + size > len)
					size = len - offset;
				memcpy(buf, hello_str + offset, size);
			} else
				size = 0;

			return static_cast<int>(size);
		}

		struct user_data_for_fuse
		{

		};

		struct chan_deleter
		{
			fileserver::path mount_point;

			void operator()(fuse_chan *chan) const
			{
				fuse_unmount(mount_point.c_str(), chan);
			}
		};

		struct fuse_deleter
		{
			void operator()(fuse *f) const
			{
				fuse_destroy(f);
			}
		};
	}

	void mount_directory(fileserver::unknown_digest const &root_digest, boost::filesystem::path const &mount_point)
	{
		chan_deleter deleter;
		deleter.mount_point = fileserver::path(mount_point);
		fuse_args args{};
		std::unique_ptr<fuse_chan, chan_deleter> chan(fuse_mount(mount_point.c_str(), &args), std::move(deleter));
		if (!chan)
		{
			throw std::runtime_error("fuse_mount failure");
		}
		fuse_operations operations{};
		operations.getattr = hello_getattr;
		operations.readdir = hello_readdir;
		operations.open = hello_open;
		operations.read = hello_read;
		user_data_for_fuse user_data;
		std::unique_ptr<fuse, fuse_deleter> const f(fuse_new(chan.get(), &args, &operations, sizeof(operations), &user_data));
		if (!f)
		{
			throw std::runtime_error("fuse_new failure");
		}

		//fuse_new seems to take ownership of the fuse_chan
		chan.release();

		fuse_loop(f.get());
	}
}
