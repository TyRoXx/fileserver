#ifndef FILESERVER_DIGEST_HPP
#define FILESERVER_DIGEST_HPP

#include <server/sha256.hpp>
#include <server/hexadecimal.hpp>
#include <silicium/fast_variant.hpp>
#include <boost/container/string.hpp>

namespace fileserver
{
	using digest = Si::fast_variant<sha256_digest>;
	using unknown_digest = boost::container::basic_string<byte>;

	inline auto get_digest_digits(digest const &original)
	{
		return Si::visit<boost::iterator_range<byte const *>>(
			original,
			[](sha256_digest const &d)
		{
			return boost::make_iterator_range(d.bytes.data(), d.bytes.data() + d.bytes.size());
		});
	}

	inline unknown_digest to_unknown_digest(digest const &original)
	{
		auto &&digits = get_digest_digits(original);
		return unknown_digest{digits.begin(), digits.end()};
	}

	inline void print(std::ostream &out, digest const &value)
	{
		out << Si::visit<char const *>(value, [](sha256_digest const &)
		{
			return "SHA256";
		}) << ":";
		auto &&digits = get_digest_digits(value);
		encode_ascii_hex_digits(digits.begin(), digits.end(), std::ostreambuf_iterator<char>(out));
	}

	template <class InputIterator>
	boost::optional<unknown_digest> parse_digest(InputIterator begin, InputIterator end)
	{
		unknown_digest result;
		auto const rest = decode_ascii_hex_bytes(begin, end, std::back_inserter(result));
		if (rest.first != end)
		{
			return boost::none;
		}
		return std::move(result);
	}

	inline std::string format_digest(unknown_digest const &value)
	{
		std::string formatted;
		encode_ascii_hex_digits(value.begin(), value.end(), std::back_inserter(formatted));
		return formatted;
	}
}

#endif
