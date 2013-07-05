HOSTNAME!=hostname
CC=c++
CFLAGS=-pedantic -Wall -Wextra -D__HOSTNAME__=\"$(HOSTNAME)\" -I/usr/local/include -I/opt/libevent/include -g3
LD=c++
#LDFLAGS=-L/usr/local/lib -L/opt/libevent/lib
LDFLAGS=-L/usr/local/lib
#DLIBS=-lssl -lcrypto -levent
DLIBS=-lssl -lcrypto
SLIBS=/opt/libevent/lib/libevent_core.a /opt/libevent/lib/libevent.a /opt/libevent/lib/libevent_openssl.a
ALLOBJS=testtool.o service.o healthcheck.o healthcheck_http.o msg.o pfctl.o

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

	
testtool.o: testtool.cpp healthcheck*.h service.h msg.h
service.o: service.cpp service.h healthcheck*.h msg.h pfctl.h
healthcheck.o: healthcheck.cpp healthcheck.h service.h msg.h pfctl.h
healthcheck_http.o: healthcheck_http.cpp healthcheck_http.h healthcheck.h service.h msg.h
msg.o: msg.cpp msg.h

#tick.o: tick.c tick.h
#msg.o: msg.c msg.h
#pfctl.o: pfctl.c pfctl.h


