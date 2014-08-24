#include <boost/asio/io_service.hpp>
#include <silicium/connecting_observable.hpp>
#include <silicium/total_consumer.hpp>
#include <silicium/http/http.hpp>
#include <silicium/coroutine.hpp>

int main()
{
	boost::asio::io_service io;
	boost::asio::ip::tcp::socket socket(io);
	Si::connecting_observable connector(socket, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 8080));
	auto connecting = Si::make_total_consumer(Si::make_coroutine<Si::nothing>([&connector](Si::yield_context<Si::nothing> &yield)
	{
		boost::optional<boost::system::error_code> const error = yield.get_one(connector);
		assert(error);

	}));
	connecting.start();
	io.run();
}
