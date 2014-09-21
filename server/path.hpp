#ifndef FILESERVER_PATH_HPP
#define FILESERVER_PATH_HPP

#include <silicium/config.hpp>
#ifndef _MSC_VER
#	include <boost/container/string.hpp>
#endif
#include <boost/filesystem/path.hpp>

namespace fileserver
{
	struct path
	{
		using value_type = boost::filesystem::path::value_type;

		path() BOOST_NOEXCEPT
		{
		}

		explicit path(boost::filesystem::path const &other)
			: characters(other.c_str())
		{
		}

#if SILICIUM_COMPILER_GENERATES_MOVES
		path(path const &) = default;
		path(path &&) BOOST_NOEXCEPT = default;
		path &operator = (path const &) = default;
#else
		path(path const &other)
			: characters(other.characters)
		{
		}

		path(path &&other) BOOST_NOEXCEPT
			: characters(std::move(other.characters))
		{
		}

		path &operator = (path const &other)
		{
			characters = other.characters;
			return *this;
		}
#endif

		path &operator = (path &&other) BOOST_NOEXCEPT
		{
#ifndef _MSC_VER
			//for unknown reasons, noexcept = default does not work for the move-assignment operator in GCC 4.8
			BOOST_STATIC_ASSERT(BOOST_NOEXCEPT_EXPR(characters = std::move(other.characters)));
#endif
			characters = std::move(other.characters);
			return *this;
		}

		boost::filesystem::path to_boost_path() const
		{
			return boost::filesystem::path(characters.begin(), characters.end());
		}

		value_type const *c_str() const BOOST_NOEXCEPT
		{
			return characters.c_str();
		}

	private:

#ifdef _MSC_VER
		std::basic_string<value_type>
#else
		boost::container::basic_string<value_type>
#endif
			characters;
	};
}

#endif
