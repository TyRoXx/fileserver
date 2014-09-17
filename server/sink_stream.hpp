#ifndef FILESERVER_SINK_STREAM_HPP
#define FILESERVER_SINK_STREAM_HPP

#include <silicium/sink.hpp>
#include <silicium/config.hpp>

namespace fileserver
{
	template <class Sink>
	struct sink_stream
	{
		using Ch = char;

		explicit sink_stream(Sink sink)
			: sink(std::move(sink))
		{
		}

		//! Read the current character from stream without moving the read cursor.
		Ch Peek() const
		{
			SILICIUM_UNREACHABLE();
		}

		//! Read the current character from stream and moving the read cursor to next character.
		Ch Take()
		{
			SILICIUM_UNREACHABLE();
		}

		//! Get the current read cursor.
		//! \return Number of characters read from start.
		size_t Tell()
		{
			SILICIUM_UNREACHABLE();
		}

		//! Begin writing operation at the current read pointer.
		//! \return The begin writer pointer.
		Ch* PutBegin()
		{
			SILICIUM_UNREACHABLE();
		}

		//! Write a character.
		void Put(Ch c)
		{
			Si::append(sink, c);
		}

		//! Flush the buffer.
		void Flush()
		{
		}

		//! End the writing operation.
		//! \param begin The begin write pointer returned by PutBegin().
		//! \return Number of characters written.
		size_t PutEnd(Ch* begin)
		{
			SILICIUM_UNREACHABLE();
		}

	private:

		Sink sink;
	};

	template <class Sink>
	auto make_sink_stream(Sink &&sink)
	{
		return sink_stream<typename std::decay<Sink>::type>(std::forward<Sink>(sink));
	}
}

#endif
