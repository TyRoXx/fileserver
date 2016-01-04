file(GLOB_RECURSE sources "*.hpp" "*.cpp")
set(formatted ${formatted} ${sources} PARENT_SCOPE)
add_library(fileserver ${sources})
