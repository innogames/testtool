cmake_minimum_required(VERSION 3.11)
project(testtool)

set(default_build_type "RelWithDebInfo")

add_compile_options(
  "-Wall" "-pedantic" "-std=c++11"
)

include_directories(/usr/local/include)
link_directories(/usr/local/lib)

set(Boost_USE_STATIC_LIBS ON)
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(PostgreSQL REQUIRED)

file(GLOB testtool_src "src/*.cpp")
add_executable(testtool ${testtool_src})
target_link_libraries(testtool
    event.a event_core.a event_pthreads.a event_openssl.a
    thr.a pq.a intl.a
    yaml-cpp fmt
    ${OPENSSL_LIBRARIES}
    ${Boost_LIBRARIES}
)
install(TARGETS testtool DESTINATION sbin)
install(
    PROGRAMS rc/testtool_freebsd
    DESTINATION etc/rc.d
    RENAME testtool
)
