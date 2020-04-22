find_package(GTest REQUIRED)
include(GoogleTest)

configure_file(cmake.h.in ${CMAKE_BINARY_DIR}/generated/cmake.h)
include_directories(${CMAKE_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/src)
link_directories(${CMAKE_BINARY_DIR}/src/CMakeFiles/testtool.dir/)

file(GLOB testtool_test_src "*.cpp" "*.h")

add_executable(testtool_test ${testtool_test_src} )
add_dependencies(testtool_test testtool)

target_link_libraries(testtool_test
  GTest::GTest GTest::Main
  ${_libs}
  msg.cpp.o
  config.cpp.o
  healthcheck.cpp.o
  healthcheck_dns.cpp.o
  healthcheck_dummy.cpp.o
  healthcheck_http.cpp.o
  healthcheck_ping.cpp.o
  healthcheck_postgres.cpp.o
  healthcheck_tcp.cpp.o
  lb_node.cpp.o
  lb_pool.cpp.o
)

# Create autoconf-compatible "check" commmand which also fixes
# the issue of CMake not including dependency from test to test binaries
add_custom_target(check
 COMMAND ${CMAKE_CTEST_COMMAND}
 DEPENDS testtool_Test
)

gtest_discover_tests(testtool_test)
