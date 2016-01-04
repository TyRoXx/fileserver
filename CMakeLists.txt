cmake_minimum_required(VERSION 2.8)
project(fileserver)

if(UNIX)
	if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
		add_definitions("-std=c++1y")
	else()
		execute_process(COMMAND ${CMAKE_CXX_COMPILER} -dumpversion OUTPUT_VARIABLE GCC_VERSION)
		if(GCC_VERSION VERSION_GREATER 4.7)
			add_definitions(-std=c++1y)
		else()
			add_definitions(-std=c++0x)
		endif()
	endif()
	add_definitions("-Wall -Wextra -Wconversion -Wvla")
	add_definitions("-Wno-missing-field-initializers")

	option(FILESERVER_PEDANTIC "pass -pedantic to the compiler (may generate useless warnings)" ON)
	if(FILESERVER_PEDANTIC)
		add_definitions("-pedantic")
	endif()
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
find_package(Boost ${SILICIUM_BOOST_REQUIRED} COMPONENTS unit_test_framework thread filesystem iostreams system coroutine context program_options)
if(UNIX)
	add_definitions("-DBOOST_TEST_DYN_LINK")
endif()
include_directories(SYSTEM ${Boost_INCLUDE_DIR})
link_directories(${Boost_LIBRARY_DIR})

if(WIN32)
	#Boost.Asio wants this for no reason
	add_definitions("-D_WIN32_WINDOWS")

	add_definitions("-DBOOST_ASIO_HAS_IOCP")
endif()

find_package(OpenSSL REQUIRED)
include_directories(SYSTEM ${OPENSSL_INCLUDE_DIR})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")

set(SILICIUM_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/silicium" CACHE PATH "")
find_package(Silicium REQUIRED)

include_directories(SYSTEM ${SILICIUM_INCLUDE_DIR})

set(VENTURA_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ventura" CACHE PATH "")
include_directories(SYSTEM ${VENTURA_INCLUDE_DIR})

find_package(UriParser REQUIRED)
if(URIPARSER_FOUND)
	include_directories(SYSTEM ${URIPARSER_INCLUDE_DIR})
endif()

find_package(SQLPP11)
find_package(SQLPP11SQLite3)
if(SQLPP11_FOUND)
	find_package(SQLite3)
endif()
if(SQLPP11_FOUND AND SQLPP11SQLITE3_FOUND AND SQLITE_FOUND)
	include_directories(${SQLPP11_INCLUDE_DIRS})
	include_directories(SYSTEM ${SQLPP11SQLITE3_INCLUDE_DIRS})
	include_directories(SYSTEM ${SQLITE3_INCLUDE_DIRS})
	set(FILESERVER_HAS_SQLITE ON)
else()
	if(NOT SQLPP11_FOUND)
		message(STATUS "Did not find sqlpp11")
	endif()
	if(NOT SQLPP11SQLITE3_FOUND)
		message(STATUS "Did not find the SQLite3 connector for sqlpp11")
	endif()
	if(NOT SQLITE_FOUND)
		message(STATUS "Did not find SQLite3")
	endif()
	set(FILESERVER_HAS_SQLITE OFF)
endif()

include_directories(SYSTEM "dependencies/rapidjson/include")

if(UNIX)
	find_package(FUSE REQUIRED)
	if(FUSE_FOUND)
		#fuse wants this to be defined
		add_definitions("-D_FILE_OFFSET_BITS=64")
		add_definitions("-DFUSE_USE_VERSION=26")
		include_directories(${FUSE_INCLUDE_DIRS})
	endif()
endif()

include_directories(".")
add_subdirectory("storage_reader")
add_subdirectory("server")
add_subdirectory("server-cli")
add_subdirectory("client")
add_subdirectory("client-cli")
add_subdirectory("test")

if(WIN32)
	set(CLANG_FORMAT "C:/Program Files/LLVM/bin/clang-format.exe" CACHE TYPE PATH)
else()
	set(CLANG_FORMAT "clang-format-3.7" CACHE TYPE PATH)
endif()
add_custom_target(clang-format COMMAND ${CLANG_FORMAT} -i ${formatted} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
