# testtool makefile

HOSTNAME       != /bin/hostname
BSD_VERSION    != uname -r | cut -d . -f 1

PREFIX ?= /usr/local

CXXFLAGS=-pedantic -std=c++11 -Wall -Wextra -Wno-unused-parameter \
       -I/usr/local/include -I$(LIBEVENT)/include -g3 \
       -D__HOSTNAME__=\"$(HOSTNAME)\"

LDFLAGS += -L/opt/libevent-2.0/lib
LDFLAGS += -L/usr/lib
LDFLAGS += -L/usr/local/lib

SRCDIR  = ${.CURDIR}/src
OBJDIR  = obj

SOURCES != find $(SRCDIR) -name '*.cpp'
OBJECTS = $(SOURCES:$(SRCDIR)/%.cpp=%.o)

CC=c++
LD=c++

.PATH: $(SRCDIR)

DLIBS=-lssl -lcrypto -lpq
SLIBS=-l:libevent_core.a -l:libevent.a -l:libevent_pthreads.a -l:libevent_openssl.a

all: testtool

testtool: $(OBJECTS)
	@echo objects: $(OBJECTS)
	$(LD) $(LDFLAGS) $(OBJECTS) $(LIBS) $(DLIBS) $(SLIBS) -o $@

clean:
	rm -f $(OBJDIR)/*.o $(OBJDIR)/testtool

test:
	./runtests.sh

install:
	install $(OBJDIR)/testtool			${DESTDIR}/${PREFIX}/bin
	install scripts/testtool_watchdog	${DESTDIR}/${PREFIX}/bin
	install scripts/testtool_bsdrc		${DESTDIR}/${PREFIX}/etc/rc.d/testtool
