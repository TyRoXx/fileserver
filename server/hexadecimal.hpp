#ifndef FILESERVER_HEXADECIMAL_HPP
#define FILESERVER_HEXADECIMAL_HPP

#include <silicium/optional.hpp>
#include <utility>

namespace fileserver
{
	using byte = boost::uint8_t;

	static char const lower_case_hex[] = "0123456789abcdef";

	template <class OutputIterator>
	OutputIterator encode_ascii_hex_digit(byte value, OutputIterator destination, char const *digits = &lower_case_hex[0])
	{
		*destination++ = digits[(value >> 4) & 0x0f];
		*destination++ = digits[(value     ) & 0x0f];
		return destination;
	}

	template <class InputIterator, class OutputIterator>
	std::pair<InputIterator, OutputIterator> encode_ascii_hex_digits(InputIterator begin, InputIterator end, OutputIterator destination, char const *digits = &lower_case_hex[0])
	{
		for (; begin != end; ++begin)
		{
			byte const value = *begin;
			destination = encode_ascii_hex_digit(value, destination, digits);
		}
		return std::make_pair(std::move(begin), std::move(destination));
	}

	Si::optional<unsigned char> decode_ascii_hex_digit(char digit)
	{
		switch (digit)
		{
		case '0': return 0;
		case '1': return 1;
		case '2': return 2;
		case '3': return 3;
		case '4': return 4;
		case '5': return 5;
		case '6': return 6;
		case '7': return 7;
		case '8': return 8;
		case '9': return 9;
		case 'a': case 'A': return 10;
		case 'b': case 'B': return 11;
		case 'c': case 'C': return 12;
		case 'd': case 'D': return 13;
		case 'e': case 'E': return 14;
		case 'f': case 'F': return 15;
		default:
			return Si::none;
		}
	}

	template <class InputIterator, class OutputIterator>
	std::pair<InputIterator, OutputIterator> decode_ascii_hex_bytes(InputIterator begin, InputIterator end, OutputIterator bytes)
	{
		for (;;)
		{
			if (begin == end)
			{
				break;
			}
			char const first_char = *begin;
			Si::optional<unsigned char> const first = decode_ascii_hex_digit(first_char);
			if (!first)
			{
				break;
			}
			auto const next = begin + 1;
			if (next == end)
			{
				break;
			}
			char const second_char = *next;
			Si::optional<unsigned char> const second = decode_ascii_hex_digit(second_char);
			if (!second)
			{
				break;
			}
			begin = next + 1;
			unsigned char const digit = static_cast<unsigned char>((*first * 16) + *second);
			*bytes = digit;
			++bytes;
		}
		return std::make_pair(begin, bytes);
	}
}

#endif
