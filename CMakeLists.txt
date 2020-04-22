cmake_minimum_required(VERSION 3.11)
project(testtool)

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
message("boost include: ${Boost_INCLUDE_DIR}")

find_package(fmt 3.3.0 REQUIRED)
# Does not provide include dir as it seems
list(APPEND _libs fmt::fmt-header-only)

find_package(nlohmann_json 3.3.0 REQUIRED)
# Does not provide include dir as it seems
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
list(APPEND _libs event event_core event_pthreads event_openssl)

find_package(OpenSSL REQUIRED)
list(APPEND _include_dirs ${OPENSSL_INCLUDE_DIR})
list(APPEND _libs ${OPENSSL_LIBRARIES})

find_package(PostgreSQL REQUIRED)
list(APPEND _libs ${PostgreSQL_LIBRARIES})

find_package(yaml-cpp REQUIRED)
list(APPEND _include_dirs ${YAML_CPP_INCLUDE_DIR})
list(APPEND _libs ${YAML_CPP_LIBRARIES})

if(UNIX AND NOT APPLE AND NOT LINUX)
  # Only on FreeBSD
  list(APPEND _libs thr.a)
endif()

include_directories(${_include_dirs})
link_directories(${_link_dirs})

add_subdirectory(src)
