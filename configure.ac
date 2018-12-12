#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_INIT(testtool, 1.0.64, kajetan.staszkiewicz@innogames.com)
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_PROG_CC
AC_PROG_CXX

AC_FUNC_FORK
AC_CHECK_FUNCS([clock_gettime gettimeofday memset socket strerror])

AC_CHECK_HEADERS([arpa/inet.h netinet/in.h stdlib.h string.h sys/socket.h \
		  sys/time.h syslog.h unistd.h \
		  fmt/printf.h \
		])

AC_CONFIG_FILES([Makefile src/Makefile rc/Makefile])

AC_CONFIG_HEADERS([config.h])         # make it generate the config.h
AC_OUTPUT
