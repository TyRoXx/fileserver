#ifndef FILESERVER_DIRECTORY_LISTING_HPP
#define FILESERVER_DIRECTORY_LISTING_HPP

#include <server/sink_stream.hpp>
#include <server/typed_reference.hpp>
#include <server/source_stream.hpp>
#include <map>

//workaround for a bug in rapidjson (SizeType is "unsigned" by default)
#define RAPIDJSON_NO_SIZETYPEDEFINE
namespace rapidjson
{
	typedef ::std::size_t SizeType;
}
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/reader.h>

namespace fileserver
{
	struct directory_listing
	{
		std::map<std::string, typed_reference> entries;
	};

	namespace detail
	{
		inline std::string const &get_digest_type_name(digest const &instance)
		{
			return Si::visit<std::string const &>(
				instance,
				[](sha256_digest const &) -> std::string const &
			{
				static std::string const name = "SHA256";
				return name;
			});
		}
	}

	static content_type const json_listing_content_type = "json_v1";

	template <class CharSink>
	void serialize_json(CharSink &&sink, directory_listing const &listing)
	{
		auto stream = make_sink_stream(std::forward<CharSink>(sink));
		rapidjson::PrettyWriter<decltype(stream)> writer(stream);
		writer.StartObject();
		for (auto const &entry : listing.entries)
		{
			writer.Key(entry.first.data(), entry.first.size());
			writer.StartObject();
			{
				typed_reference const &ref = entry.second;
				writer.Key("type");
				writer.String(ref.type.data(), ref.type.size());

				writer.Key("content");
				std::string content;
				auto const &content_digits = get_digest_digits(ref.referenced);
				encode_ascii_hex_digits(content_digits.begin(), content_digits.end(), std::back_inserter(content));
				writer.String(content.data(), content.size());

				writer.Key("hash");
				auto const &hash = detail::get_digest_type_name(ref.referenced);
				writer.String(hash.data(), hash.size());
			}
			writer.EndObject();
		}
		writer.EndObject();
	}

	template <class CharSource>
	inline Si::fast_variant<std::unique_ptr<fileserver::directory_listing>, std::size_t> deserialize_json(CharSource &&serialized)
	{
		rapidjson::Document document;
		{
			auto serialized_stream = make_source_stream(std::forward<CharSource>(serialized));
			document.ParseStream(serialized_stream);
		}
		if (document.HasParseError())
		{
			return document.GetErrorOffset();
		}
		if (!document.IsObject())
		{
			return std::size_t(0);
		}
		auto listing = Si::make_unique<fileserver::directory_listing>();
		for (auto const &entry : boost::make_iterator_range(document.MemberBegin(), document.MemberEnd()))
		{
			auto const &name = entry.name;
			if (!name.IsString())
			{
				throw std::logic_error("todo 1");
			}

			auto const &description = entry.value;
			if (!description.IsObject())
			{
				throw std::logic_error("todo 2");
			}

			auto const &type = description["type"];
			if (!type.IsString())
			{
				throw std::logic_error("todo 3");
			}

			auto const &content = description["content"];
			if (!type.IsString())
			{
				throw std::logic_error("todo 3");
			}
			auto const &parsed_content = parse_digest(content.GetString(), content.GetString() + content.GetStringLength());
			if (!parsed_content)
			{
				throw std::logic_error("todo 4");
			}

			auto const &hash = description["hash"];
			if (!hash.IsString())
			{
				throw std::logic_error("todo5");
			}
			std::string const hash_str(hash.GetString(), hash.GetStringLength());
			digest content_digest;
			if (hash_str == "SHA256")
			{
				if (parsed_content->size() != sha256_digest().bytes.size())
				{
					throw std::logic_error("todo 6");
				}
				content_digest = sha256_digest(parsed_content->data());
			}
			else
			{
				throw std::logic_error("todo 7");
			}

			listing->entries.insert(std::make_pair(
				std::string(name.GetString(), name.GetStringLength()),
				typed_reference(
					content_type(type.GetString(), type.GetStringLength()),
					content_digest
				)));
		}
		return std::move(listing);
	}
}

#endif
