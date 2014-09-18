#include <server/directory_listing.hpp>
#include <silicium/memory_source.hpp>
#include <boost/test/unit_test.hpp>

namespace
{
	inline std::vector<char> vector_from_c_str(char const *s)
	{
		return std::vector<char>(s, s + std::strlen(s));
	}
}

BOOST_AUTO_TEST_CASE(directory_listing_json_v1_serialize_empty)
{
	fileserver::directory_listing const empty;
	std::pair<std::vector<char>, fileserver::content_type> const encoded = fileserver::serialize_json(empty);
	BOOST_CHECK(vector_from_c_str("{}") == encoded.first);
}

BOOST_AUTO_TEST_CASE(directory_listing_json_v1_deserialize_empty)
{
	auto source = Si::make_c_str_source("{}");
	Si::fast_variant<std::unique_ptr<fileserver::directory_listing>, std::size_t> const parsed = fileserver::deserialize_json(source);
	auto *listing = Si::try_get_ptr<std::unique_ptr<fileserver::directory_listing>>(parsed);
	BOOST_REQUIRE(listing);
	BOOST_REQUIRE(*listing);
	BOOST_CHECK((*listing)->entries.empty());
}

namespace
{
	std::string const a_non_ascii_name = "\xc3\x84\xc3\xa3";
	std::vector<char> const a_json_directory_listing = vector_from_c_str((
		"{\n"
		"    \"\xc3\x84\xc3\xa3\": {\n"
		"        \"type\": \"blob\",\n"
		"        \"content\": \"" + std::string(256 / 4, '0') + "\",\n"
		"        \"hash\": \"SHA256\"\n"
		"    }\n"
		"}").c_str()
		);
	fileserver::directory_listing a_directory_listing()
	{
		return
		{{
			{a_non_ascii_name, fileserver::typed_reference("blob", fileserver::sha256_digest())}
		}};
	}
}

BOOST_AUTO_TEST_CASE(directory_listing_json_v1_serialize_unicode)
{
	auto const listing = a_directory_listing();
	std::pair<std::vector<char>, fileserver::content_type> const encoded = fileserver::serialize_json(listing);
	BOOST_CHECK(a_json_directory_listing == encoded.first);
}

BOOST_AUTO_TEST_CASE(directory_listing_json_v1_deserialize_unicode)
{
	auto source = Si::make_container_source(a_json_directory_listing);
	Si::fast_variant<std::unique_ptr<fileserver::directory_listing>, std::size_t> const parsed = fileserver::deserialize_json(source);
	auto * const listing = Si::try_get_ptr<std::unique_ptr<fileserver::directory_listing>>(parsed);
	BOOST_REQUIRE(listing);
	BOOST_REQUIRE(*listing);
	auto const expected = a_directory_listing();
	BOOST_CHECK((*listing)->entries == expected.entries);
}

BOOST_AUTO_TEST_CASE(directory_listing_json_v1_deserialize_error)
{
	auto source = Si::make_c_str_source("{ ? }");
	Si::fast_variant<std::unique_ptr<fileserver::directory_listing>, std::size_t> const parsed = fileserver::deserialize_json(source);
	BOOST_CHECK_EQUAL(2,
		Si::visit<std::size_t>(parsed,
			[](std::unique_ptr<fileserver::directory_listing> const &) -> std::size_t
	{
		BOOST_FAIL("failure expected");
		return 0;
	},
			[](std::size_t position)
	{
		return position;
	}));
}
