file(GLOB sources "*.hpp" "*.cpp")
add_executable(client-cli ${sources})
target_link_libraries(client-cli client ${SILICIUM_LIBRARIES} ${Boost_LIBRARIES} ${OPENSSL_CRYPTO_LIBRARY} ${FUSE_LIBRARIES})
