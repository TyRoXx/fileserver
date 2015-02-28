find_path(SQLPP11SQLITE3_INCLUDE_DIR NAMES sqlpp11/sqlite3/sqlite3.h)
mark_as_advanced(SQLPP11SQLITE3_INCLUDE_DIR)
find_library(SQLPP11SQLITE3_LIBRARY NAMES sqlpp11-connector-sqlite3)
mark_as_advanced(SQLPP11SQLITE3_LIBRARY)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SQLPP11SQLITE3 DEFAULT_MSG SQLPP11SQLITE3_INCLUDE_DIR SQLPP11SQLITE3_LIBRARY)
if(SQLPP11SQLITE3_FOUND)
	set(SQLPP11SQLITE3_INCLUDE_DIRS ${SQLPP11SQLITE3_INCLUDE_DIR})
	set(SQLPP11SQLITE3_LIBRARIES ${SQLPP11SQLITE3_LIBRARY})
else()
	set(SQLPP11SQLITE3_INCLUDE_DIRS)
	set(SQLPP11SQLITE3_LIBRARIES)
endif()
