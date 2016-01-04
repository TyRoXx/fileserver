#ifndef FILESERVER_TYPED_REFERENCE_HPP
#define FILESERVER_TYPED_REFERENCE_HPP

#include <server/digest.hpp>
#ifdef _MSC_VER
#include <string>
#else
#include <boost/container/string.hpp>
#endif
#include <ostream>
#include <tuple>

namespace fileserver
{
	using content_type =
#ifdef _MSC_VER
	    std::string
#else
	    boost::container::string
#endif
	    ;

	content_type const blob_content_type = "blob";

	struct typed_reference
	{
		content_type type;
		digest referenced;

		typed_reference()
		{
		}

		explicit typed_reference(content_type type, digest referenced)
		    : type(std::move(type))
		    , referenced(std::move(referenced))
		{
		}
	};

	inline bool operator==(typed_reference const &left, typed_reference const &right)
	{
		return std::tie(left.type, left.referenced) == std::tie(right.type, right.referenced);
	}

	inline void print(std::ostream &out, typed_reference const &value)
	{
		out << value.type << ":";
		print(out, value.referenced);
	}
}

#endif
