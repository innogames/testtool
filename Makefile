HOSTNAME!=hostname
CC=c++

#LIBEVENT=/opt/libevent-2.0
LIBEVENT=/opt/libevent-2.1

GIT_BRANCH:=`git rev-parse --abbrev-ref HEAD`
GIT_LAST_COMMIT:=`git log -1 --pretty='%H'`

CFLAGS=-pedantic -Wall -Wextra \
       -I/usr/local/include -I$(LIBEVENT)/include -g3 \
       -D__HOSTNAME__=\"$(HOSTNAME)\" \
       -D__GIT_BRANCH__="\"$(GIT_BRANCH)\"" \
       -D__GIT_LAST_COMMIT__="\"$(GIT_LAST_COMMIT)\""

LD=c++
#LDFLAGS=-L/usr/local/lib -L$(LIBEVENT)/lib
LDFLAGS=-L/usr/local/lib
#DLIBS=-lssl -lcrypto -levent
DLIBS=-lssl -lcrypto
SLIBS=$(LIBEVENT)/lib/libevent_core.a $(LIBEVENT)/lib/libevent.a $(LIBEVENT)/lib/libevent_pthreads.a $(LIBEVENT)/lib/libevent_openssl.a
#SLIBS=$(LIBEVENT)/lib/libevent_core.a $(LIBEVENT)/lib/libevent.a $(LIBEVENT)/lib/libevent_openssl.a
ALLOBJS=testtool.o lb_pool.o lb_node.o healthcheck.o healthcheck_http.o healthcheck_ping.o healthcheck_dns.o msg.o pfctl.o

all: testtool

testtool: $(ALLOBJS)
	$(LD) $(LDFLAGS) $(LIBS) $(DLIBS) $(ALLOBJS) $(SLIBS) -o $@

#%.o: %.cpp
#	$(CC) $(CFLAGS) -o $@ -c $<

clean:
	rm -f *.o testtool testtool.core

#	@echo '^' all dependencies: $^ - not supported in bsd?
#	@echo '?' more recent than the target: $?
#	@echo '+' keeps duplicates and gives you the entire list: $+
#	@echo '<' the first dependency: $<
#	@echo '@' the name of the target: $@
#	$(LD) $(LDFLAGS) $(LIBS) $(DLIBS) $(SLIBS) $(ALLOBJS) -o $@

test:
	./runtests.sh

testtool.o: testtool.cpp lb_pool.h lb_node.h healthcheck.h healthcheck_*.h msg.h
lb_pool: lb_pool.cpp lb_pool.h lb_node.h msg.h
lb_node.o: lb_node.cpp lb_node.h lb_pool.h healthcheck.h healthcheck_*.h msg.h
healthcheck.o: healthcheck.cpp healthcheck.h healthcheck_*.h msg.h
healthcheck_http.o: healthcheck_http.cpp healthcheck_http.h healthcheck.h lb_node.h msg.h
healthcheck_ping.o: healthcheck_ping.cpp healthcheck_ping.h healthcheck.h lb_node.h msg.h
healthcheck_dns.o: healthcheck_dns.cpp healthcheck_dns.h healthcheck.h lb_node.h msg.h
msg.o: msg.cpp msg.h
