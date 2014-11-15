#ifndef FILESERVER_SHA256_HPP
#define FILESERVER_SHA256_HPP

#include <server/byte.hpp>
#include <openssl/sha.h>
#include <array>
#include <silicium/source/source.hpp>
#include <boost/range/begin.hpp>
#include <boost/range/end.hpp>

namespace fileserver
{
	template <std::size_t ByteSize>
	struct fixed_digest
	{
		std::array<byte, ByteSize> bytes;

		fixed_digest() BOOST_NOEXCEPT
		{
			bytes.fill(0);
		}

		template <class InputIterator>
		explicit fixed_digest(InputIterator from)
		{
			std::copy_n(from, bytes.size(), bytes.begin());
		}
	};

	template <std::size_t ByteSize>
	bool operator == (fixed_digest<ByteSize> const &left, fixed_digest<ByteSize> const &right)
	{
		return left.bytes == right.bytes;
	}

	using sha256_digest = fixed_digest<256 / 8>;

	template <class BytesViewSource>
	sha256_digest sha256(BytesViewSource &&content)
	{
		SHA256_CTX state;
		SHA256_Init(&state);
		for (;;)
		{
			auto const &byte_array_view = Si::get(content);
			if (!byte_array_view)
			{
				break;
			}
			using boost::begin;
			using boost::end;
			SHA256_Update(&state, begin(*byte_array_view), std::distance(begin(*byte_array_view), end(*byte_array_view)));
		}
		sha256_digest result;
		SHA256_Final(result.bytes.data(), &state);
		return result;
	}
}

#endif
