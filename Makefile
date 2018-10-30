#
# Testtool - Makefile
#
# Copyright (c) 2018 InnoGames GmbH
#

PREFIX ?= /usr/local

CPPFLAGS= -pedantic -std=c++11 \
	  -Wall -Wextra -Wno-unused-parameter \
	  -ggdb \
	  -I/usr/local/include

LDFLAGS= -L/usr/local/lib -ggdb

SRCDIR  = ${CURDIR}/src
OBJDIR  = $(CURDIR)/obj

SOURCES = $(wildcard $(SRCDIR)/*.cpp)
OBJECTS = $(SOURCES:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)

CC=c++
LD=c++

.PATH: $(SRCDIR)

DLIBS=	-levent -levent_core -levent_pthreads -levent_openssl \
	-lyaml-cpp

SLIBS=	/usr/local/lib/libboost_system.a \
	/usr/local/lib/libfmt.so \
	/usr/local/lib/libpq.a \
	/usr/local/lib/libintl.a

UNAME_S := $(shell uname -s)
UNAME_R := $(shell uname -r)

ifeq ($(UNAME_S),FreeBSD)
ifeq ($(findstring 10.,$(UNAME_R)),10.)
DLIBS += -l:libcrypto.so.7 -l:libssl.so.7
endif
ifeq ($(findstring 11.,$(UNAME_R)),11.)
DLIBS += -l:libcrypto.so.8 -l:libssl.so.8 -lthr
endif
endif
ifeq ($(UNAME_S),Linux)
DLIBS += -l:libcrypto.so.1.0.2 -l:libssl.so.1.0.2
endif

all: testtool

testtool: $(OBJECTS)
	@echo $(OS)
	$(LD) $(LDFLAGS) $(OBJECTS) $(DLIBS) $(SLIBS) -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CPPFLAGS) -c $< -o $(OBJDIR)/$*.o

clean:
	rm -f $(OBJDIR)/*.o $(CURDIR)/testtool

test:
	./runtests.sh

install:
	install $(CURDIR)/testtool			${DESTDIR}/${PREFIX}/sbin
	install $(CURDIR)/scripts/testtool_bsdrc	${DESTDIR}/${PREFIX}/etc/rc.d/testtool
