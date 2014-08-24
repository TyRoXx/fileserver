file(GLOB sources "*.hpp" "*.cpp")
add_executable(fileserver-cli ${sources})
target_link_libraries(fileserver-cli fileserver ${SILICIUM_LIBRARIES} ${Boost_LIBRARIES} ${OPENSSL_CRYPTO_LIBRARY})
