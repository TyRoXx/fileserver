#include <server/tree.hpp>
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(write_tree_entry)
{
	fileserver::tree_entry entry;
	entry.name = fileserver::make_file_name("file.txt");
	entry.type = fileserver::tree_entry_type::blob;
	entry.content = fileserver::sha256_digest();
	std::vector<fileserver::byte> written;
	auto sink = Si::make_container_sink(written);
	fileserver::write_tree_entry(sink, entry);
	std::vector<fileserver::byte> const expected
	{
		'f', 'i', 'l', 'e', '.', 't', 'x', 't', 0x00, //name

		0x00, //type

		0x00, //content
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	BOOST_CHECK(expected == written);
}

BOOST_AUTO_TEST_CASE(write_tree)
{
	std::vector<fileserver::byte> written;
	auto sink = Si::make_container_sink(written);
	fileserver::tree_entry const entry{fileserver::make_file_name("a.bin"), fileserver::tree_entry_type::blob, fileserver::sha256_digest{}};
	auto entries = Si::make_generator_source<fileserver::tree_entry const *>([&entry]() -> boost::optional<fileserver::tree_entry const *>
	{
		return &entry;
	});
	fileserver::write_tree(sink, 1, entries);
	std::vector<fileserver::byte> const expected
	{
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, //count

		'a', '.', 'b', 'i', 'n', 0x00, //name

		0x00, //type

		0x00, //content
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	BOOST_CHECK(expected == written);
}
