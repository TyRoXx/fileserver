#ifndef FILESERVER_TYPED_REFERENCE_HPP
#define FILESERVER_TYPED_REFERENCE_HPP

#include <server/digest.hpp>
#include <boost/container/string.hpp>
#include <ostream>

namespace fileserver
{
	using content_type = boost::container::string;

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

	inline void print(std::ostream &out, typed_reference const &value)
	{
		out << value.type << ":";
		print(out, value.referenced);
	}
}

#endif
