cmake_minimum_required(VERSION 3.11)
project(testtool)
enable_testing()

set(default_build_type "RelWithDebInfo")

add_compile_options(
  "-Wall" "-pedantic" "-std=c++11"
)

if (APPLE)
  # Include packages installed from brew
  set(OPENSSL_ROOT_DIR /usr/local/opt/openssl)
  set(nlohmann_json_DIR /usr/local/opt/nlohmann-json/lib/cmake/nlohmann_json/)
endif()

set(Boost_USE_STATIC_LIBS ON)
find_package(Boost REQUIRED COMPONENTS system)
list(APPEND _include_dirs ${Boost_INCLUDE_DIR})
list(APPEND _libs ${Boost_LIBRARIES})

find_package(fmt 3.3.0 REQUIRED)

find_package(nlohmann_json 3.3.0 REQUIRED)
if (APPLE)
  # Does not provide include dir as it seems
  list(APPEND _include_dirs /usr/local/opt/nlohmann-json/include)
endif()
list(APPEND _libs nlohmann_json::nlohmann_json)

# find_package(libevent REQUIRED) # Does not come with cmake support?
# Libraries will hopefully be in default directory
if (APPLE)
  list(APPEND _include_dirs /usr/local/opt/libevent/include)
  list(APPEND _link_dirs /usr/local/opt/libevent/lib)
endif()
if(UNIX AND NOT APPLE AND NOT LINUX)
  set(_link_dirs /usr/local/lib)
  set(_include_dirs /usr/local/include)
endif()
list(APPEND _libs event.so event_core.so event_openssl.so)

find_package(OpenSSL REQUIRED)
list(APPEND _include_dirs ${OPENSSL_INCLUDE_DIR})
list(APPEND _libs ${OPENSSL_LIBRARIES})

find_package(PostgreSQL REQUIRED)
list(APPEND _libs libpq.a)

find_package(yaml-cpp REQUIRED)
list(APPEND _include_dirs ${YAML_CPP_INCLUDE_DIR})
list(APPEND _libs ${YAML_CPP_LIBRARIES})

if(UNIX AND NOT APPLE AND NOT LINUX)
  # Only on FreeBSD
  list(APPEND _libs thr.a)
  list(APPEND _libs libintl.a)
endif()

include_directories(${_include_dirs})
link_directories(${_link_dirs})

file(GLOB testtool_sources "src/*.cpp")
file(GLOB testtool_headers "src/*.h")
foreach(testtool_source ${testtool_sources})
  string(REGEX REPLACE ".*/" "" testtool_library ${testtool_source})
  add_library("${testtool_library}" OBJECT ${testtool_source} ${testool_headers})
  list(APPEND testtool_libraries ${testtool_library})
endforeach()

add_executable(testtool)
target_link_libraries(testtool ${_libs} ${testtool_libraries})
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
