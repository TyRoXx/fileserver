file(GLOB sources "*.hpp" "*.cpp")
set(formatted ${formatted} ${sources} PARENT_SCOPE)
add_executable(client-cli ${sources})
target_link_libraries(client-cli client ${CONAN_LIBS} ${Boost_LIBRARIES} ${OPENSSL_CRYPTO_LIBRARY} ${FUSE_LIBRARIES})
