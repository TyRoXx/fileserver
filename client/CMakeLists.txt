file(GLOB sources "*.hpp" "*.cpp")
set(formatted ${formatted} ${sources} PARENT_SCOPE)
add_library(client ${sources})
target_link_libraries(client storage_reader)
