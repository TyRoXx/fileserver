#include <server/directory_listing.hpp>
#include <boost/test/unit_test.hpp>

namespace
{
	inline std::vector<char> vector_from_c_str(char const *s)
	{
		return std::vector<char>(s, s + std::strlen(s));
	}
}

BOOST_AUTO_TEST_CASE(directory_listing_json_v1_empty)
{
	fileserver::directory_listing const empty;
	std::pair<std::vector<char>, fileserver::content_type> const encoded = fileserver::serialize_json(empty);
	BOOST_CHECK(vector_from_c_str("{}") == encoded.first);
}

BOOST_AUTO_TEST_CASE(directory_listing_json_v1_unicode)
{
	std::string const non_ascii_name = "\xc3\x84\xc3\xa3";
	fileserver::directory_listing const listing
	{
		{
			{non_ascii_name, fileserver::typed_reference("blob", fileserver::sha256_digest())}
		}
	};
	std::pair<std::vector<char>, fileserver::content_type> const encoded = fileserver::serialize_json(listing);
	auto const expected = vector_from_c_str((
		"{\n"
		"    \"\xc3\x84\xc3\xa3\": {\n"
		"        \"type\": \"blob\",\n"
		"        \"content\": \"" + std::string(256 / 4, '0') + "\",\n"
		"        \"hash\": \"SHA256\"\n"
		"    }\n"
		"}").c_str()
		);
	BOOST_CHECK(expected == encoded.first);
}
