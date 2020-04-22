file(GLOB testtool_src "*.cpp" "*.h")

add_executable(testtool ${testtool_src})

target_link_libraries(testtool ${_libs})

install(TARGETS testtool DESTINATION sbin)
install(
    PROGRAMS rc/testtool_freebsd
    DESTINATION etc/rc.d
    RENAME testtool
)
