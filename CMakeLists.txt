cmake_minimum_required(VERSION 2.8)
project(fileserver)

if(UNIX)
	add_definitions("-Wall -Wextra -pedantic -std=c++1y -Wconversion -Wvla")
endif()

if(MSVC)
	#make Visual C++ 2013 build in parallel
	add_definitions("/MP")

	#we want more warnings
	add_definitions("/W4")

	#disable "'std::_Copy_n': Function call with parameters that may be unsafe"
	add_definitions("/wd4996")

	#workaround for Boost 1.55 Context error LNK2026
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SAFESEH:NO")
endif()

if(MSVC)
	set(Boost_USE_STATIC_LIBS ON) 
endif()
if(NOT MSVC)
	set(SILICIUM_BOOST_REQUIRED REQUIRED)
endif()
find_package(Boost ${SILICIUM_BOOST_REQUIRED} COMPONENTS unit_test_framework thread filesystem iostreams system coroutine program_options)
if(UNIX)
	add_definitions("-DBOOST_TEST_DYN_LINK")
endif()

if(WIN32)
	#Boost.Asio wants this for no reason
	add_definitions("-D_WIN32_WINDOWS")
endif()

find_package(OpenSSL REQUIRED)
include_directories(${OPENSSL_INCLUDE_DIR})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")
find_package(Silicium REQUIRED)

include_directories(${SILICIUM_INCLUDE_DIR})

include_directories("dependencies/rapidjson/include")

include_directories(".")
add_subdirectory("server")
add_subdirectory("server-cli")
add_subdirectory("client-cli")
add_subdirectory("test")
