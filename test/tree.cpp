#include <server/tree.hpp>
#include <boost/test/unit_test.hpp>

namespace
{
	inline auto make_digest(char const *c_str)
	{
		auto const * const begin = reinterpret_cast<fileserver::byte const *>(c_str);
		return fileserver::digest(begin, begin + std::strlen(c_str));
	}
}

BOOST_AUTO_TEST_CASE(write_tree_entry)
{
	fileserver::tree_entry entry;
	entry.name = fileserver::make_file_name("file.txt");
	entry.type = fileserver::tree_entry_type::blob;
	entry.content = make_digest("\x12\x34");
	std::vector<fileserver::byte> written;
	auto sink = Si::make_container_sink(written);
	fileserver::write_tree_entry(sink, entry);
	std::vector<fileserver::byte> const expected
	{
		'f', 'i', 'l', 'e', '.', 't', 'x', 't', 0x00, //name

		0x00, //type

		0x02, //content
		0x12, 0x34
	};
	BOOST_CHECK(expected == written);
}
