#include <server/digest.hpp>
#include <silicium/source.hpp>
#include <silicium/sink.hpp>
#include <boost/container/vector.hpp>
#include <boost/test/unit_test.hpp>

namespace fileserver
{
	using file_name = boost::container::vector<byte>;

	file_name make_file_name(char const *c_str)
	{
		auto * const begin = reinterpret_cast<byte const *>(c_str);
		return file_name(begin, begin + std::strlen(c_str));
	}

	enum class tree_entry_type
	{
		blob,
		tree
	};

	struct tree_entry
	{
		file_name name;
		tree_entry_type type;
		digest content;
	};

	template <class Element>
	bool read_until_terminator(Si::sink<Element> &out, Si::source<Element> &in, Element const &terminator)
	{
		for (;;)
		{
			auto element = Si::get(in);
			if (!element)
			{
				return false;
			}
			if (*element == terminator)
			{
				return true;
			}
			Si::append(out, std::move(*element));
		}
	}

	boost::optional<file_name> read_file_name(Si::source<byte> &in)
	{
		file_name result;
		auto result_writer = Si::make_container_sink(result);
		if (read_until_terminator(result_writer, in, static_cast<byte>(0)))
		{
			return std::move(result);
		}
		return boost::none;
	}

	boost::optional<tree_entry_type> read_tree_entry_type(Si::source<byte> &in)
	{
		auto raw = Si::get(in);
		if (!raw)
		{
			return boost::none;
		}
		switch (*raw)
		{
		case 0: return tree_entry_type::blob;
		case 1: return tree_entry_type::tree;
		default:
			return boost::none;
		}
	}

	boost::optional<digest> read_digest(Si::source<byte> &in)
	{
		auto length = Si::get(in);
		if (!length)
		{
			return boost::none;
		}
		return Si::take<byte, digest>(in, *length);
	}

	boost::optional<tree_entry> read_tree_entry(Si::source<byte> &in)
	{
		auto name = read_file_name(in);
		if (!name)
		{
			return boost::none;
		}
		auto type = read_tree_entry_type(in);
		if (!type)
		{
			return boost::none;
		}
		auto content = read_digest(in);
		if (!content)
		{
			return boost::none;
		}
		return tree_entry{std::move(*name), std::move(*type), std::move(*content)};
	}

	void write_tree_entry_type(Si::sink<byte> &out, tree_entry_type type)
	{
		byte value = 0;
		switch (type)
		{
		case tree_entry_type::blob: value = 0; break;
		case tree_entry_type::tree: value = 1; break;
		}
		Si::append(out, value);
	}

	void write_tree_entry(Si::sink<byte> &out, tree_entry const &entry)
	{
		out.append(boost::make_iterator_range(entry.name.data(), entry.name.data() + entry.name.size()));
		Si::append(out, static_cast<byte>(0));
		write_tree_entry_type(out, entry.type);
		assert(entry.content.size() <= std::numeric_limits<byte>::max());
		Si::append(out, static_cast<byte>(entry.content.size()));
		out.append(boost::make_iterator_range(entry.content.data(), entry.content.data() + entry.content.size()));
	}
}

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
