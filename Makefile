# testtool makefile

HOSTNAME       != /bin/hostname
BSD_VERSION    != uname -r | cut -d . -f 1
GIT_BRANCH     != git rev-parse --abbrev-ref HEAD
GIT_LAST_COMMIT!= git log -1 --pretty='%H'

CXXFLAGS=-pedantic -std=c++11 -Wall -Wextra -Wno-unused-parameter -Wno-literal-suffix \
       -I/usr/local/include -I$(LIBEVENT)/include -g3 \
       -D__HOSTNAME__=\"$(HOSTNAME)\" \
       -D__GIT_BRANCH__="\"$(GIT_BRANCH)\"" \
       -D__GIT_LAST_COMMIT__="\"$(GIT_LAST_COMMIT)\"" \

LDFLAGS += -L/opt/libevent-2.0/lib
LDFLAGS += -L/usr/local/lib

CC=c++
LD=c++

DLIBS=-lssl -lcrypto
SLIBS=-l:libevent_core.a -l:libevent.a -l:libevent_pthreads.a -l:libevent_openssl.a

ALLOBJS=testtool.o config.o lb_vip.o lb_pool.o lb_node.o healthcheck.o healthcheck_tcp.o healthcheck_http.o healthcheck_ping.o healthcheck_dns.o msg.o pfctl.o mechanism.o dummy_mechanism.o pf_mechanism.o

all: testtool

testtool: $(ALLOBJS)
	$(LD) $(LDFLAGS) $(ALLOBJS) $(LIBS) $(DLIBS) $(SLIBS) -o $@

clean:
	rm -f *.o testtool testtool.core

test:
	./runtests.sh

testtool.o: testtool.cpp testtool.h lb_vip.h lb_pool.h lb_node.h healthcheck.h healthcheck_*.h msg.h
lb_vip.o: lb_vip.cpp lb_vip.h lb_pool.h mechanism.h dummy_mechanism.h pf_mechanism.h
lb_pool: lb_pool.cpp lb_vip.h lb_pool.h lb_node.h msg.h
lb_node.o: lb_node.cpp lb_node.h lb_pool.h healthcheck.h healthcheck_*.h msg.h mechanism.h
healthcheck.o: healthcheck.cpp healthcheck.h healthcheck_*.h msg.h
healthcheck_tcp.o: healthcheck_tcp.cpp healthcheck_tcp.h healthcheck.h lb_node.h msg.h
healthcheck_http.o: healthcheck_http.cpp healthcheck_http.h healthcheck.h lb_node.h msg.h
healthcheck_ping.o: healthcheck_ping.cpp healthcheck_ping.h healthcheck.h lb_node.h msg.h
healthcheck_dns.o: healthcheck_dns.cpp healthcheck_dns.h healthcheck.h lb_node.h msg.h
mechanism.o: mechanism.cpp mechanism.h lb_node.h lb_vip.h msg.h
dummy_mechanism.o: dummy_mechanism.cpp dummy_mechanism.h mechanism.h lb_node.h lb_vip.h
pf_mechanism.o: pf_mechanism.cpp pf_mechanism.h mechanism.h lb_node.h lb_vip.h
msg.o: msg.cpp msg.h
