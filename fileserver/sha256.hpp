#ifndef FILESERVER_SHA256_HPP
#define FILESERVER_SHA256_HPP

#include <openssl/sha.h>
#include <array>
#include <silicium/source.hpp>

namespace fileserver
{
	template <std::size_t ByteSize>
	struct digest
	{
		std::array<unsigned char, ByteSize> bytes;
	};

	using sha256_digest = digest<256 / 8>;

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
