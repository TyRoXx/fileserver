file(GLOB sources "*.hpp" "*.cpp")
add_library(client ${sources})
target_link_libraries(client file_service)
