cmake_minimum_required(VERSION 3.11)
project(testtool)
enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)

set(default_build_type "RelWithDebInfo")

add_compile_options(
  "-Wextra" "-pedantic" "-Wno-c11-extensions"
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


# libevent
set(LibEvent_EXTRA_PREFIXES /usr/local /opt/local /usr/local/opt/libevent)
foreach(prefix ${LibEvent_EXTRA_PREFIXES})
  list(APPEND LibEvent_INCLUDE_PATHS "${prefix}/include")
  list(APPEND LibEvent_LIB_PATHS "${prefix}/lib")
endforeach()
foreach(library event event_core event_openssl)
  find_library(LibEvent_LIBRARY NAMES ${library} PATHS ${LibEvent_LIB_PATHS} REQUIRED)
  list(APPEND LibEvent_LIBRARIES ${library})
endforeach()
find_path(LibEvent_INCLUDE_DIR event.h PATHS ${LibEvent_INCLUDE_PATHS} REQUIRED)
list(APPEND _link_dirs ${LibEvent_LIB_PATHS})
list(APPEND _libs ${LibEvent_LIBRARIES})
list(APPEND _include_dirs ${LibEvent_INCLUDE_DIR})

find_package_handle_standard_args (LibEvent DEFAULT_MSG LibEvent_LIBRARIES LibEvent_INCLUDE_DIR)
mark_as_advanced(LibEvent_INCLUDE_DIR LibEvent_LIBRARIES)

# openssl
find_package(OpenSSL REQUIRED)
list(APPEND _include_dirs ${OPENSSL_INCLUDE_DIR})
list(APPEND _libs ${OPENSSL_LIBRARIES})

# libpq
find_package(PostgreSQL REQUIRED)
list(APPEND _libs ${PostgreSQL_LIBRARIES})

find_package(yaml-cpp REQUIRED)
list(APPEND _include_dirs ${YAML_CPP_INCLUDE_DIR})
list(APPEND _libs ${YAML_CPP_LIBRARIES})

if(UNIX AND NOT APPLE AND NOT LINUX)
  # Only on FreeBSD
  find_package(Intl REQUIRED)
  list(APPEND _libs thr.a)
  list(APPEND _libs ${Intl_LIBRARIES})
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
