#ifndef FILESERVER_DIGEST_HPP
#define FILESERVER_DIGEST_HPP

#include <server/sha256.hpp>
#include <silicium/fast_variant.hpp>
#include <boost/container/string.hpp>

namespace fileserver
{
	using digest = Si::fast_variant<sha256_digest>;
	using unknown_digest = boost::container::basic_string<byte>;

	auto get_digest_digits(digest const &original)
	{
		return Si::visit<boost::iterator_range<byte const *>>(
			original,
			[](sha256_digest const &d)
		{
			return boost::make_iterator_range(d.bytes.data(), d.bytes.data() + d.bytes.size());
		});
	}

	unknown_digest to_unknown_digest(digest const &original)
	{
		auto &&digits = get_digest_digits(original);
		return unknown_digest{digits.begin(), digits.end()};
	}
}

#endif
