#ifndef FILESERVER_PATH_HPP
#define FILESERVER_PATH_HPP

#include <boost/container/string.hpp>
#include <boost/filesystem/path.hpp>

namespace fileserver
{
	struct path
	{
		path() BOOST_NOEXCEPT
		{
		}

		explicit path(boost::filesystem::path const &other)
			: characters(other.string().begin(), other.string().end())
		{
		}

		path(path const &) = default;
		path(path &&) BOOST_NOEXCEPT = default;
		path &operator = (path const &) = default;

		path &operator = (path &&other) BOOST_NOEXCEPT
		{
			//for unknown reasons, noexcept = default does not work for the move-assignment operator in GCC 4.8
			BOOST_STATIC_ASSERT(BOOST_NOEXCEPT_EXPR(characters = std::move(other.characters)));
			characters = std::move(other.characters);
			return *this;
		}

		boost::filesystem::path to_boost_path() const
		{
			return boost::filesystem::path(characters.begin(), characters.end());
		}

		char const *c_str() const BOOST_NOEXCEPT
		{
			return characters.c_str();
		}

	private:

		boost::container::basic_string<boost::filesystem::path::value_type> characters;
	};
}

#endif
