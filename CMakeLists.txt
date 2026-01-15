cmake_minimum_required(VERSION 3.31)
project(testtool)
enable_testing()

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)

set(default_build_type "RelWithDebInfo")

add_compile_options(
  "-Wextra" "-pedantic"
)

find_package(PkgConfig REQUIRED)

# Threading
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

# openssl
if (APPLE)
  set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl)
endif()
find_package(OpenSSL REQUIRED)
list(APPEND _include_dirs ${OPENSSL_INCLUDE_DIR})
list(APPEND _libs ${OPENSSL_LIBRARIES})

# boost
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost REQUIRED)
list(APPEND _include_dirs ${Boost_INCLUDE_DIR})

# fmt
find_package(fmt 3.3.0 REQUIRED)
list(APPEND _libs fmt::fmt)

# nlohmann_json
if (APPLE)
  set(nlohmann_json_DIR /opt/homebrew/opt/nlohmann-json/lib/cmake/nlohmann_json/)
endif()
find_package(nlohmann_json 3.3.0 REQUIRED)
list(APPEND _libs nlohmann_json::nlohmann_json)

# libevent
if (APPLE)
  set(ENV{PKG_CONFIG_PATH} "/opt/homebrew/opt/libevent/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
pkg_check_modules(LibEvent REQUIRED libevent libevent_openssl)
list(APPEND _libs ${LibEvent_LIBRARIES})
list(APPEND _include_dirs ${LibEvent_INCLUDE_DIRS})
list(APPEND _link_dirs ${LibEvent_LIBRARY_DIRS})

# libpq
find_package(PostgreSQL REQUIRED)
list(APPEND _include_dirs ${PostgreSQL_INCLUDE_DIRS})
list(APPEND _libs ${PostgreSQL_LIBRARIES})

# yaml-cpp
if (APPLE)
  set(ENV{PKG_CONFIG_PATH} "/opt/homebrew/opt/yaml-cpp/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
pkg_check_modules(YAML_CPP REQUIRED yaml-cpp)
list(APPEND _include_dirs ${YAML_CPP_INCLUDE_DIRS})
list(APPEND _libs ${YAML_CPP_LIBRARIES})
list(APPEND _link_dirs ${YAML_CPP_LIBRARY_DIRS})

# Intl (FreeBSD only)
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  find_package(Intl REQUIRED)
  list(APPEND _libs ${Intl_LIBRARIES})
endif()

include_directories(${_include_dirs})
link_directories(${_link_dirs})

file(GLOB testtool_sources "src/*.cpp")
file(GLOB testtool_headers "src/*.h")
foreach(testtool_source ${testtool_sources})
  string(REGEX REPLACE ".*/" "" testtool_library ${testtool_source})
  add_library("${testtool_library}" OBJECT ${testtool_source} ${testool_headers})
  target_link_libraries("${testtool_library}" PRIVATE ${_libs})
  target_include_directories("${testtool_library}" PRIVATE ${_include_dirs})
  list(APPEND testtool_libraries ${testtool_library})
endforeach()

add_executable(testtool)
target_link_libraries(testtool Threads::Threads ${_libs} ${testtool_libraries})
install(TARGETS testtool DESTINATION sbin)
install(
    PROGRAMS rc/testtool_freebsd
    DESTINATION etc/rc.d
    RENAME testtool
)

list(APPEND testtool_test_libraries ${testtool_libraries})
list(FILTER testtool_test_libraries EXCLUDE REGEX "^(msg|pfctl_worker|pfctl|testtool).cpp$")

find_package(GTest REQUIRED)
include(GoogleTest)

configure_file("tests/cmake_dirs.h.in" ${CMAKE_BINARY_DIR}/generated/cmake_dirs.h)
include_directories(${CMAKE_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/src)

file(GLOB testtool_test_src "tests/*.cpp" "tests/*.h")
add_executable(testtool_test ${testtool_test_src})
add_dependencies(testtool_test ${testtool_test_libraries})
gtest_discover_tests(testtool_test)

target_link_libraries(testtool_test
  Threads::Threads
  GTest::GTest
  GTest::Main 
  ${testtool_test_libraries}
  ${_libs}
)

# Create autoconf-compatible "check" commmand which also fixes
# the issue of CMake not including dependency from test to test binaries
add_custom_target(check
 COMMAND ${CMAKE_CTEST_COMMAND}
 DEPENDS testtool_test
)
