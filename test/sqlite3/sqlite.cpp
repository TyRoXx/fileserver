#include <sqlpp11/sqlite3/sqlite3.h>
#include <sqlpp11/ppgen.h>
#include <sqlpp11/sqlpp11.h>
#include <sqlite3.h>
#include <boost/test/unit_test.hpp>

SQLPP_DECLARE_TABLE(
	(known_file_location)
	,
	(hash, varchar(64), SQLPP_NOT_NULL)
	(hash_code, blob, SQLPP_NOT_NULL)
	(uri, text, SQLPP_NOT_NULL)
)

BOOST_AUTO_TEST_CASE(test_sqlpp11_sqlite3)
{
	sqlpp::sqlite3::connection_config config;
	config.path_to_database = ":memory:";
	config.flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	sqlpp::sqlite3::connection database(config);
	known_file_location::known_file_location location;

	//TODO: create table from the existing description in the C++ code
	database.execute(
		"CREATE TABLE known_file_location ("
		"	hash varchar(64) NOT NULL,"
		"	hash_code blob NOT NULL,"
		"	uri text NOT NULL"
		")"
	);

	database(insert_into(location).set(location.hash = "sha256", location.hash_code = "", location.uri = "/tmp/t.bin"));

	auto results = database(select(location.hash, location.hash_code, location.uri).from(location).where(true));
	auto current_row = results.begin();
	BOOST_REQUIRE(current_row != results.end());
	BOOST_CHECK_EQUAL("sha256", current_row->hash);
	BOOST_CHECK_EQUAL("", current_row->hash_code);
	BOOST_CHECK_EQUAL("/tmp/t.bin", current_row->uri);
	++current_row;
	BOOST_CHECK(results.end() == current_row);
}
