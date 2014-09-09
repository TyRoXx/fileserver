#ifndef FILESERVER_TREE_HPP
#define FILESERVER_TREE_HPP

#include <server/digest.hpp>
#include <silicium/source.hpp>
#include <silicium/sink.hpp>
#include <boost/container/vector.hpp>

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

	namespace detail
	{
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
			auto type = Si::get(in);
			if (!type)
			{
				return boost::none;
			}
			switch (*type)
			{
			case 0:
				{
					sha256_digest value;
					if ((value.bytes.data() + value.bytes.size()) !=
					     in.copy_next(boost::make_iterator_range(value.bytes.data(), value.bytes.data() + value.bytes.size())))
					{
						return boost::none;
					}
					return digest{value};
				}

			default:
				return boost::none;
			}
		}
	}

	boost::optional<tree_entry> read_tree_entry(Si::source<byte> &in)
	{
		auto name = detail::read_file_name(in);
		if (!name)
		{
			return boost::none;
		}
		auto type = detail::read_tree_entry_type(in);
		if (!type)
		{
			return boost::none;
		}
		auto content = detail::read_digest(in);
		if (!content)
		{
			return boost::none;
		}
		return tree_entry{std::move(*name), std::move(*type), std::move(*content)};
	}

	namespace detail
	{
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
	}

	void write_digest(Si::sink<byte> &out, digest const &value)
	{
		return Si::visit<void>(
			value,
			[&out](sha256_digest const &sha256)
		{
			Si::append(out, static_cast<byte>(0));
			out.append(boost::make_iterator_range(sha256.bytes.data(), sha256.bytes.data() + sha256.bytes.size()));
		});
	}

	void write_tree_entry(Si::sink<byte> &out, tree_entry const &entry)
	{
		out.append(boost::make_iterator_range(entry.name.data(), entry.name.data() + entry.name.size()));
		Si::append(out, static_cast<byte>(0));
		detail::write_tree_entry_type(out, entry.type);
		write_digest(out, entry.content);
	}

	namespace detail
	{
		template <class Integer>
		void encode_big_endian(Si::sink<byte> &out, Integer value)
		{
			for (std::size_t i = 0; i < sizeof(value); ++i)
			{
				std::size_t const bits_in_a_byte = 8;
				byte const current_byte = static_cast<byte>(value >> ((sizeof(value) - i - 1) * bits_in_a_byte));
				Si::append(out, current_byte);
			}
		}
	}

	template <class TreeEntryPtrSource>
	void write_tree(Si::sink<byte> &out, boost::uint64_t entry_count, TreeEntryPtrSource &&entries, byte version = 1)
	{
		Si::append(out, version);
		detail::encode_big_endian(out, entry_count);
		for (boost::uint64_t i = 0; i < entry_count; ++i)
		{
			tree_entry const &entry = **Si::get(entries);
			write_tree_entry(out, entry);
		}
	}
}

#endif
