#ifndef FILESERVER_DIRECTORY_LISTING_HPP
#define FILESERVER_DIRECTORY_LISTING_HPP

#include <server/sink_stream.hpp>
#include <server/typed_reference.hpp>
#include <map>

//workaround for a bug in rapidjson (SizeType is "unsigned" by default)
#define RAPIDJSON_NO_SIZETYPEDEFINE
namespace rapidjson
{
	typedef ::std::size_t SizeType;
}
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>

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

	content_type const json_listing_content_type = "json_v1";

	inline std::pair<std::vector<char>, content_type> serialize_json(directory_listing const &listing)
	{
		std::vector<char> serialized;
		auto stream = make_sink_stream(Si::make_container_sink(serialized));
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
		return std::make_pair(std::move(serialized), json_listing_content_type);
	}
}

#endif
