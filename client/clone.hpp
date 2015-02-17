#ifndef FILESERVER_CLIENT_CLONE_HPP
#define FILESERVER_CLIENT_CLONE_HPP

#include "file_service/file_service.hpp"
#include <server/digest.hpp>
#include <silicium/error_or.hpp>
#include <silicium/file_handle.hpp>
#include <silicium/observable/ptr.hpp>
#include <silicium/open.hpp>
#include <silicium/source/file_source.hpp>
#include <silicium/to_shared.hpp>
#include <silicium/to_unique.hpp>
#include <silicium/source/transforming_source.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace fileserver
{
	struct writeable_file
	{
		virtual ~writeable_file()
		{
		}
		virtual boost::system::error_code seek(file_offset destination) = 0;
		virtual boost::system::error_code write(Si::memory_range const &written) = 0;
	};

	struct readable_file
	{
		virtual ~readable_file()
		{
		}
		virtual
		Si::error_or<
			std::unique_ptr<
				Si::source<
					Si::error_or<
						Si::memory_range
					>
				>
			>
		> read(file_offset begin) = 0;
	};

	struct read_write_file
	{
		std::unique_ptr<readable_file> readable;
		std::unique_ptr<writeable_file> writeable;

#if !SILICIUM_COMPILER_GENERATES_MOVES
		read_write_file() BOOST_NOEXCEPT
		{}

		read_write_file(read_write_file &&other) BOOST_NOEXCEPT
			: readable(std::move(other.readable))
			, writeable(std::move(other.writeable))
		{
		}

		read_write_file &operator = (read_write_file &&other) BOOST_NOEXCEPT
		{
			readable = std::move(other.readable);
			writeable = std::move(other.writeable);
			return *this;
		}

		read_write_file(std::unique_ptr<readable_file> readable, std::unique_ptr<writeable_file> writeable) BOOST_NOEXCEPT
			: readable(std::move(readable))
			, writeable(std::move(writeable))
		{
		}
#endif
	};

	struct directory_manipulator
	{
		virtual ~directory_manipulator()
		{
		}
		virtual boost::system::error_code require_exists() = 0;
		virtual std::unique_ptr<directory_manipulator> edit_subdirectory(std::string const &name) = 0;
		virtual Si::error_or<std::unique_ptr<writeable_file>> create_regular_file(std::string const &name) = 0;
		virtual Si::error_or<read_write_file> read_write_regular_file(std::string const &name) = 0;
	};

	inline boost::system::error_code write_all(Si::native_file_descriptor destination, boost::iterator_range<char const *> buffer)
	{
		std::size_t total_written = 0;
		while (total_written < static_cast<size_t>(buffer.size()))
		{
#ifdef _WIN32
			DWORD written = 0;
			DWORD const piece = static_cast<DWORD>(std::min(buffer.size() - total_written, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
			if (!WriteFile(destination, buffer.begin() + total_written, piece, &written, nullptr))
			{
				return boost::system::error_code(GetLastError(), boost::system::native_ecat);
			}
			total_written += written;
#else
			ssize_t rc = write(destination, buffer.begin() + total_written, buffer.size() - total_written);
			if (rc < 0)
			{
				return boost::system::error_code(errno, boost::system::native_ecat);
			}
			total_written += static_cast<size_t>(rc);
#endif
		}
		return boost::system::error_code();
	}

	inline boost::system::error_code seek_absolute(Si::native_file_descriptor file, boost::uint64_t destination)
	{
#ifdef _WIN32
		LARGE_INTEGER destinationConverted;
		destinationConverted.QuadPart = destination;
		if (!SetFilePointerEx(file, destinationConverted, nullptr, SEEK_SET))
		{
			return boost::system::error_code(GetLastError(), boost::system::native_ecat);
		}
#else
		auto position = lseek(file, destination, SEEK_SET);
		if (static_cast<boost::uint64_t>(position) != destination)
		{
			return boost::system::error_code(errno, boost::system::posix_category);
		}
#endif
		return boost::system::error_code();
	}

	struct filesystem_writeable_file : writeable_file
	{
		explicit filesystem_writeable_file(std::shared_ptr<Si::file_handle> file)
			: file(std::move(file))
		{
		}

		virtual boost::system::error_code seek(file_offset destination) SILICIUM_OVERRIDE
		{
			return seek_absolute(file->handle, destination);
		}

		virtual boost::system::error_code write(Si::memory_range const &written) SILICIUM_OVERRIDE
		{
			assert(file);
			return write_all(file->handle, written);
		}

	private:

		std::shared_ptr<Si::file_handle> file;
	};

	struct filesystem_readable_file : readable_file
	{
		explicit filesystem_readable_file(std::shared_ptr<Si::file_handle> file)
			: file(std::move(file))
			, buffer(8192)
		{
		}

		virtual
		Si::error_or<
			std::unique_ptr<
				Si::source<
					Si::error_or<
						Si::memory_range
					>
				>
			>
		> read(file_offset begin) SILICIUM_OVERRIDE
		{
			boost::system::error_code ec = seek_absolute(file->handle, begin);
			if (ec)
			{
				return ec;
			}
			return Si::to_unique(
				Si::make_transforming_source(
					Si::make_file_source(file->handle, Si::make_iterator_range(buffer.data(), buffer.data() + buffer.size())),
					[this](Si::error_or<Si::memory_range> bytes_read)
					{
						return Si::map(bytes_read, [this](Si::memory_range bytes_read) -> Si::memory_range
						{
							assert(static_cast<size_t>(bytes_read.size()) < buffer.size());
							return bytes_read;
						});
					})
				);
		}

	private:

		std::shared_ptr<Si::file_handle> file;
		std::vector<char> buffer;
	};

	struct filesystem_directory_manipulator : fileserver::directory_manipulator
	{
		explicit filesystem_directory_manipulator(boost::filesystem::path root)
			: root(std::move(root))
		{
		}

		virtual boost::system::error_code require_exists() SILICIUM_OVERRIDE
		{
			boost::system::error_code ec;
			boost::filesystem::create_directories(root, ec);
			return ec;
		}

		virtual std::unique_ptr<directory_manipulator> edit_subdirectory(std::string const &name) SILICIUM_OVERRIDE
		{
			return Si::make_unique<filesystem_directory_manipulator>(root / name);
		}

		virtual Si::error_or<std::unique_ptr<writeable_file>> create_regular_file(std::string const &name) SILICIUM_OVERRIDE
		{
			auto opened = Si::create_file(root / name);
			if (opened.is_error())
			{
				return opened.error();
			}
			return Si::make_unique<filesystem_writeable_file>(Si::to_shared(std::move(opened.get())));
		}

		virtual Si::error_or<read_write_file> read_write_regular_file(std::string const &name) SILICIUM_OVERRIDE
		{
			return Si::map(Si::open_read_write(root / name), [](Si::file_handle file) -> read_write_file
			{
				auto shared_file = Si::to_shared(std::move(file));
				return read_write_file
				{
					Si::make_unique<filesystem_readable_file>(shared_file),
					Si::make_unique<filesystem_writeable_file>(shared_file)
				};
			});
		}

	private:

		boost::filesystem::path root;
	};

	Si::unique_observable<boost::system::error_code>
	clone_directory(unknown_digest const &root_digest, directory_manipulator &destination, file_service &server, boost::asio::io_service &io);
}

#endif
