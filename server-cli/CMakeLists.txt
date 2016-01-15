file(GLOB sources "*.hpp" "*.cpp")
set(formatted ${formatted} ${sources} PARENT_SCOPE)
add_executable(fileserver-cli ${sources})
target_link_libraries(fileserver-cli fileserver ${CONAN_LIBS} ${Boost_LIBRARIES} ${OPENSSL_LIBRARIES} ${URIPARSER_LIBRARIES})
