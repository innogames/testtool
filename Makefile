# testtool makefile

PREFIX ?= /usr/local

CPPFLAGS= -pedantic -std=c++11 \
	  -Wall -Wextra -Wno-unused-parameter \
	  -g3 \
	  -I/usr/local/include

LDFLAGS= -L/usr/local/lib

SRCDIR  = ${CURDIR}/src
OBJDIR  = $(CURDIR)/obj

SOURCES = $(wildcard $(SRCDIR)/*.cpp)
OBJECTS = $(SOURCES:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)

CC=c++
LD=c++

.PATH: $(SRCDIR)

DLIBS=	-levent -levent_core -levent_pthreads -levent_openssl \
	-lpq -lyaml-cpp

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),FreeBSD)
DLIBS += -l:libcrypto.so.7 -l:libssl.so.7
endif
ifeq ($(UNAME_S),Linux)
DLIBS += -l:libcrypto.so.1.0.2 -l:libssl.so.1.0.2
endif

all: testtool

testtool: $(OBJECTS)
	@echo $(OS)
	$(LD) $(LDFLAGS) $(OBJECTS) $(DLIBS) -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CPPFLAGS) -c $< -o $(OBJDIR)/$*.o

clean:
	rm -f $(OBJDIR)/*.o $(CURDIR)/testtool

test:
	./runtests.sh

install:
	install $(CURDIR)testtool			${DESTDIR}/${PREFIX}/sbin
	install $(CURDIR)/scripts/testtool_watchdog	${DESTDIR}/${PREFIX}/sbin
	install $(CURDIR)/scripts/testtool_bsdrc	${DESTDIR}/${PREFIX}/etc/rc.d/testtool
