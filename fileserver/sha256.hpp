#ifndef FILESERVER_SHA256_HPP
#define FILESERVER_SHA256_HPP

#include <openssl/sha.h>
#include <array>

namespace fileserver
{
	struct sha256_digest
	{
		std::array<unsigned char, 256 / 8> bytes;
	};

	template <class RangeOfByteArrayViews>
	sha256_digest sha256(RangeOfByteArrayViews &&content)
	{
		SHA256_CTX state;
		SHA256_Init(&state);
		for (auto const &byte_array_view : content)
		{
			using boost::begin;
			using boost::end;
			SHA256_Update(&state, begin(byte_array_view), std::distance(begin(byte_array_view), end(byte_array_view)));
		}
		sha256_digest result;
		SHA256_Final(result.bytes.data(), &state);
		return result;
	}
}

#endif
