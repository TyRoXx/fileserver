file(GLOB sources "*.hpp" "*.cpp")
add_executable(test ${sources})
target_link_libraries(test client ${Boost_LIBRARIES} ${SQLPP11SQLITE3_LIBRARIES} ${SQLITE_LIBRARIES})
