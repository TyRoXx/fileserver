#ifndef FILESERVER_SOURCE_STREAM_HPP
#define FILESERVER_SOURCE_STREAM_HPP

#include <silicium/source/source.hpp>
#include <silicium/config.hpp>

namespace fileserver
{
	template <class Source>
	struct source_stream
	{
		using Ch = char;

		explicit source_stream(Source source)
		    : source(std::move(source))
		{
		}

		//! Read the current character from stream without moving the read cursor.
		Ch Peek() /*const*/
		{
			if (buffer)
			{
				return *buffer;
			}
			buffer = Si::get(source);
			if (buffer)
			{
				return *buffer;
			}
			return '\0';
		}

		//! Read the current character from stream and moving the read cursor to next
		//! character.
		Ch Take()
		{
			Ch result = Peek();
			buffer = Si::none;
			++position;
			return result;
		}

		//! Get the current read cursor.
		//! \return Number of characters read from start.
		size_t Tell()
		{
			return position;
		}

		//! Begin writing operation at the current read pointer.
		//! \return The begin writer pointer.
		Ch *PutBegin()
		{
			SILICIUM_UNREACHABLE();
		}

		//! Write a character.
		void Put(Ch c)
		{
			boost::ignore_unused_variable_warning(c);
			SILICIUM_UNREACHABLE();
		}

		//! Flush the buffer.
		void Flush()
		{
			SILICIUM_UNREACHABLE();
		}

		//! End the writing operation.
		//! \param begin The begin write pointer returned by PutBegin().
		//! \return Number of characters written.
		size_t PutEnd(Ch *begin)
		{
			boost::ignore_unused_variable_warning(begin);
			SILICIUM_UNREACHABLE();
		}

	private:
		Source source;
		std::size_t position = 0;
		Si::optional<char> buffer;
	};

	template <class Source>
	auto make_source_stream(Source &&source)
#if !SILICIUM_COMPILER_HAS_AUTO_RETURN_TYPE
	    -> source_stream<typename std::decay<Source>::type>
#endif
	{
		return source_stream<typename std::decay<Source>::type>(std::forward<Source>(source));
	}
}

#endif
