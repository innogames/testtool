#include <iostream>
#include <sstream>
#include <vector>

#include <errno.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>

#include <event2/util.h>
#include <event2/event_struct.h>

#include "msg.h"

#include "lb_pool.h"
#include "lb_node.h"
#include "healthcheck.h"
#include "healthcheck_tcp.h"

using namespace std;

extern struct event_base	*eventBase;
extern int			 verbose;

/*
   Constructor for TCP healthcheck.
*/
Healthcheck_tcp::Healthcheck_tcp(istringstream &definition, class LbNode *_parent_lbnode): Healthcheck(definition, _parent_lbnode) {
	type = "tcp";
	log_txt(MSG_TYPE_DEBUG, "      type: tcp");
}

/*
   The callback is called by libevent, it's a static method that requires the Healthcheck object to be passed to it.
*/
void Healthcheck_tcp::callback(evutil_socket_t socket_fd, short what, void *arg) {
	Healthcheck_tcp *hc = (Healthcheck_tcp *)arg;
	if (what & EV_TIMEOUT) {
		hc->last_state = STATE_DOWN;
		if (verbose>1 || hc->hard_state != STATE_DOWN)
			log_lb(MSG_TYPE_HC_FAIL,
			    hc->parent_lbnode->parent_lbpool->name.c_str(),
			    hc->parent_lbnode->address.c_str(),
			    hc->port,
			    "Healthcheck_%s: timeout after %d,%3ds",
			    hc->type.c_str(),
			    hc->timeout.tv_sec,
			    (hc->timeout.tv_nsec/1000000));

	} else if (what & EV_READ) {
		char buf[256];
		if (read(socket_fd, buf, 255) == -1){
			hc->last_state = STATE_DOWN;
			if (verbose>1 || hc->hard_state != STATE_DOWN)
			log_lb(MSG_TYPE_HC_FAIL,
				hc->parent_lbnode->parent_lbpool->name.c_str(),
				hc->parent_lbnode->address.c_str(),
				hc->port,
				"Healthcheck_%s: connection refused",
				hc->type.c_str());
		}
	} else if (what & EV_WRITE) {
		hc->last_state = STATE_UP;
		if (verbose>1 || hc->last_state == STATE_DOWN)
			log_lb(MSG_TYPE_HC_PASS,
				hc->parent_lbnode->parent_lbpool->name.c_str(),
				hc->parent_lbnode->address.c_str(),
				hc->port,
				"Healthcheck_%s: connection successful",
				hc->type.c_str());
	}
	/* Be sure to free the memory! */
	event_del(hc->ev);
	event_free(hc->ev);
	close(socket_fd);
	hc->handle_result();
}


int Healthcheck_tcp::schedule_healthcheck(struct timespec *now) {
	struct sockaddr_in	to_addr;

	/* Peform general stuff for scheduled healthcheck. */
	if (Healthcheck::schedule_healthcheck(now) == false)
		return false;

	/* Create a socket. */
	socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (socket_fd == -1) {
		log_txt(MSG_TYPE_CRITICAL, "socket(): %s", strerror(errno));
		return false;
	}
	memset(&to_addr, 0, sizeof(sockaddr_in));
	to_addr.sin_family = AF_INET;
	to_addr.sin_addr.s_addr = inet_addr(parent_lbnode->address.c_str());
	to_addr.sin_port = htons(port);

	/* In fact I'm not really sure if it needs to be nonblocking. */
	evutil_make_socket_nonblocking(socket_fd);

	/* Sending to host is one thing, but we want answers only from our target in this socket.
	   "connect" makes the socket receive only traffic from that host. */
	connect(socket_fd, (struct sockaddr *) &to_addr, sizeof(sockaddr_in));

	/* Create an event and make it pending. */
	struct timeval timeout_tv;
	timeout_tv.tv_sec  = timeout.tv_sec;
	timeout_tv.tv_usec = timeout.tv_nsec / 1000;
	ev = event_new(eventBase, socket_fd, EV_WRITE|EV_READ|EV_TIMEOUT, Healthcheck_tcp::callback, this);
	event_add(ev, &timeout_tv);

	return true;
}

