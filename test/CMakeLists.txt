file(GLOB sources "*.hpp" "*.cpp")
add_executable(test ${sources})
target_link_libraries(test ${Boost_LIBRARIES})
