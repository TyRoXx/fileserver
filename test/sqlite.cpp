#ifdef FILESERVER_HAS_SQLITE

#include <sqlpp11/sqlite3/sqlite3.h>
#include <sqlite3.h>
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(test_sqlpp11_sqlite3)
{
	sqlpp::sqlite3::connection_config config;
	config.path_to_database = ":memory:";
	config.flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	sqlpp::sqlite3::connection database(config);
}

#endif
