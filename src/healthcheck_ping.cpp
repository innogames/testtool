#include <iostream>
#include <sstream>
#include <vector>
#include <fmt/format.h>
#include <fmt/printf.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <yaml-cpp/yaml.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include <sys/types.h>
#include <unistd.h>

#include <event2/event.h>
#include <event2/event_struct.h>

#include "config.h"
#include "msg.h"

#include "lb_pool.h"
#include "lb_node.h"
#include "healthcheck.h"
#include "healthcheck_ping.h"

using namespace std;

extern struct event_base	*eventBase;
extern int			 verbose;


/* In the .h file there are only declarations of static variables, here we have definitions. */
int			 Healthcheck_ping::socket4_fd;
int			 Healthcheck_ping::socket6_fd;
struct event		*Healthcheck_ping::ev4;
struct event		*Healthcheck_ping::ev6;
uint16_t		 Healthcheck_ping::ping_id;
uint16_t		 Healthcheck_ping::ping_global_seq = 0;
Healthcheck_ping	**Healthcheck_ping::seq_map;


/*
 * Taken from FreeBSD's /usr/src/sbin/ping/ping.c
 * in_cksum --
 *      Checksum routine for Internet Protocol family headers (C Version)
 */
u_short in_cksum(u_short *addr, int len) {
	int nleft, sum;
	u_short *w;
	union {
		u_short us;
		u_char  uc[2];
	} last;

	u_short answer;

	nleft = len;
	sum = 0;
	w = addr;

	/*
	 * Our algorithm is simple, using a 32 bit accumulator (sum), we add
	 * sequential 16 bit words to it, and at the end, fold back all the
	 * carry bits from the top 16 bits into the lower 16 bits.
	 */
	while (nleft > 1)  {
		sum += *w++;
		nleft -= 2;
	}

	/* mop up an odd byte, if necessary */
	if (nleft == 1) {
		last.uc[0] = *(u_char *)w;
		last.uc[1] = 0;
		sum += last.us;
	}

	/* add back carry outs from top 16 bits to low 16 bits */
	sum = (sum >> 16) + (sum & 0xffff);     /* add hi 16 to low 16 */
	sum += (sum >> 16);                     /* add carry */
	answer = ~sum;                          /* truncate to 16 bits */
	return(answer);
}


/*
   A common initializator for all healthchecks of this type. Should be called once at the startup of testtool.
*/
int Healthcheck_ping::initialize() {
	int sockopt;

	/* So I was told that using the pid number for ping id is the Unix Way... */
	ping_id = getpid();

	/* Allocate memory for seq map. */
	seq_map = (Healthcheck_ping**)calloc(1<<(sizeof(uint16_t)*8), sizeof(Healthcheck_ping*) );

	/* Create sockets for both protocols. */
	socket4_fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (socket4_fd == -1) {
		log(MSG_CRIT, fmt::sprintf("socket4() error: %s", strerror(errno)));
		return false;
	}

	/* Create sockets for both protocols. */
	struct icmp6_filter filterv6;
	ICMP6_FILTER_SETBLOCKALL(&filterv6);
	ICMP6_FILTER_SETPASS(ICMP6_DST_UNREACH, &filterv6);
	ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &filterv6);
	socket6_fd = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (socket6_fd == -1) {
		log(MSG_CRIT, fmt::sprintf("socket6() error: %s", strerror(errno)));
		return false;
	}
	sockopt = setsockopt(socket6_fd, IPPROTO_ICMPV6, ICMP6_FILTER, &filterv6, sizeof (filterv6));
	if (sockopt < 0) {
		log(MSG_CRIT, fmt::sprintf("sockopt IPv6 filter error: %s", strerror(errno)));
		return false;
	}
	log(MSG_DEBUG, fmt::sprintf("protocols initialized"));

	/* The default 9kB buffer loses some packets. */
	int newbuf = 262144;
	sockopt = setsockopt(socket4_fd, SOL_SOCKET, SO_RCVBUF, &newbuf, sizeof(int));
	if (sockopt < 0) {
		log(MSG_CRIT, fmt::sprintf("sockopt IPv4 buffer error: %s", strerror(errno)));
		return false;
	}
	sockopt = setsockopt(socket6_fd, SOL_SOCKET, SO_RCVBUF, &newbuf, sizeof(int));
	if (sockopt < 0) {
		log(MSG_CRIT, fmt::sprintf("sockopt IPv6 buffer error: %s", strerror(errno)));
		return false;
	}

	/* Create an event and make it pending. */
	ev4 = event_new(eventBase, socket4_fd, EV_READ|EV_PERSIST, Healthcheck_ping::callback, NULL);
	event_add(ev4, NULL);
	ev6 = event_new(eventBase, socket6_fd, EV_READ|EV_PERSIST, Healthcheck_ping::callback, NULL);
	event_add(ev6, NULL);

	return true;
}


/*
   A common "destructor" for all healthchecks of this type. Should be called when testtool terminates.
*/
void Healthcheck_ping::destroy() {
	event_del(ev4);
	event_free(ev4);
	close(socket4_fd);

	event_del(ev6);
	event_free(ev6);
	close(socket6_fd);

	free(seq_map);
}


/*
   Constructor for ping healthcheck. Parses ping-specific parameters.
*/
Healthcheck_ping::Healthcheck_ping(const YAML::Node& config, class LbNode *_parent_lbnode): Healthcheck(config, _parent_lbnode) {
	/* Oh wait, there are none for this healthcheck! */
	type = "ping";
}

#define OFFSETOF(type, field)    ((unsigned long) &(((type *) 0)->field))

/*
   The callback is called by libevent, it's a static method.
   Unfortunately for ping checks there is only one socket so it is impossible
   to pass a Healthcheck object related to a given event. Therefore this callback
   function must parse the received packet and map it to one of Healthcheck objects.
   Should such parsing or mapping be impossible, due to some transmission errors or
   unknown types of packet received (only Echo Reply and Destination Unreachable are
   analyzed), this callback returns without handling healthcheck result. This results
   in Healthcheck's timeout.
*/
void Healthcheck_ping::callback(evutil_socket_t socket_fd, short what, void *arg) {
	(void)(arg); /* Make compiler happy. */

	Healthcheck_ping	*healthcheck = NULL;
	unsigned char		 raw_packet[IP_MAXPACKET];
	struct ip		*ip4_packet;
	ssize_t			 ip_header_len;
	union icmp_echo		*icmp_packet;
	struct timespec		 now;
	ssize_t			 received_bytes;
	string			 message;
	uint16_t		 recvd_seq, recvd_id;

	/* There should be no other event types. */
	if (what != EV_READ)
		return;

	clock_gettime(CLOCK_MONOTONIC, &now);

	/* Prepare place for receiving data. */
	memset(raw_packet, 0, sizeof(raw_packet));

	/* Read a packet from the socket. */
	received_bytes = recvfrom(socket_fd, raw_packet, sizeof(raw_packet), 0, NULL, NULL);
	if (received_bytes <=0 ) {
		log(MSG_CRIT, fmt::sprintf("recvfrom() error: %s", strerror(errno)));
	}

	/* Calculate offset to ICMP in IP */
	if (socket_fd == socket4_fd) {
		ip4_packet = (struct ip*)raw_packet;
		ip_header_len = ip4_packet->ip_hl << 2; /* IHL is the number of 32-bit (4 bytes) words, multiply it by 4 to get bytes. */
	} else {
		ip_header_len = 0;
	}

	/* First we must check if the received packet is:
	   - ICMP at all (can't really for IPv6)
	   - A valid ICMP packet.
	   - Addressed to us (by ping id).
	 */

	icmp_packet = (union icmp_echo *)(raw_packet + ip_header_len);

	if (
		(socket_fd == socket4_fd && icmp_packet->icmp4.icmp_header.icmp_type == ICMP_ECHOREPLY) ||
		(socket_fd == socket6_fd && icmp_packet->icmp6.icmp6_header.icmp6_type == ICMP6_ECHO_REPLY)
	) {
		/*
		 * ECHO REPLY is a correct answer so it contains id and seq
		 * directly in itself. No need to dig into further headers.
		 */
		if (socket_fd == socket4_fd) {
			if (received_bytes < ip_header_len + ICMP_MINLEN)
				return;
			recvd_id = icmp_packet->icmp4.icmp_header.icmp_id;
			recvd_seq = icmp_packet->icmp4.icmp_header.icmp_seq;
		} else {
			if (received_bytes <(ssize_t)sizeof(struct icmp6_echo))
				return;
			recvd_id = icmp_packet->icmp6.icmp6_header.icmp6_id;
			recvd_seq = icmp_packet->icmp6.icmp6_header.icmp6_seq;
		}
		recvd_seq = ntohs(recvd_seq);

		/* Is it addressed to us? */
		if (recvd_id != htons(ping_id))
			return;

		/* Now let's map the received packet to a Healthcheck_icmp object. */
		if (seq_map[recvd_seq] != NULL && seq_map[recvd_seq]->ping_my_seq == recvd_seq)
			healthcheck = seq_map[recvd_seq];
		else
			return;

		long int nsec_diff;
		if (socket_fd == socket4_fd) {
			nsec_diff = (now.tv_sec - icmp_packet->icmp4.timestamp.tv_sec) * 1000000000 + (now.tv_nsec - icmp_packet->icmp4.timestamp.tv_nsec);
		} else {
			nsec_diff = (now.tv_sec - icmp_packet->icmp6.timestamp.tv_sec) * 1000000000 + (now.tv_nsec - icmp_packet->icmp6.timestamp.tv_nsec);
		}

		int ms_full = nsec_diff / 1000000;
		int ms_dec  = (nsec_diff - ms_full * 1000000) / 1000;

		message = fmt::sprintf("reply after %d.%dms", ms_full, ms_dec);

		healthcheck->end_check(HC_PASS, message);
	}
}


/*
   Due to lack of possibility to use typical libevent timeout mechanism on raw sockets,
   it is necessary to check timeout of this healthcheck manually.
*/
void Healthcheck_ping::finalize_result() {
	struct timespec now;
	string message;

	/* Check for timeouts only for checks that are still running. */
	if (is_running == false)
		return;

	clock_gettime(CLOCK_MONOTONIC, &now);

	timespecsub(&now, &last_checked);

	if (now.tv_sec > this->timeout.tv_sec ||
	    (now.tv_sec == this->timeout.tv_sec &&
	     now.tv_nsec > this->timeout.tv_usec * 1000)) {
		message = fmt::sprintf("timeout after %d.%03ds",
			this->timeout.tv_sec,
			this->timeout.tv_usec / 1000
		);
		ping_my_seq = 0;
		end_check(HC_FAIL, message);
	}
}


int Healthcheck_ping::schedule_healthcheck(struct timespec *now) {

	/* Peform general stuff for scheduled healthcheck. */
	if (Healthcheck::schedule_healthcheck(now) == false)
		return false;

	/* Increase the ICMP sequence number. It is zeroed elsewehere, so we always start with 1. */
	ping_global_seq ++;

	/* Create the mapping between the ping Sequence Number and the object. */
	ping_my_seq = ping_global_seq;
	seq_map[ping_my_seq] = this;

	if (parent_lbnode->address_family == AF_INET) {
		struct sockaddr_in	 to_addr;
		struct icmp4_echo	 echo_request;

		/* Build the ICMP Echo Request packet to be send. */
		memset(&echo_request, 0, sizeof(echo_request));

		/* Fill in the headers. */
		echo_request.icmp_header.icmp_type = ICMP_ECHO;
		echo_request.icmp_header.icmp_code = 0;
		echo_request.icmp_header.icmp_id = htons(ping_id);
		echo_request.icmp_header.icmp_seq = htons(ping_my_seq);

		/* Remember time when this request was sent. */
		memcpy(&echo_request.timestamp, now, sizeof(struct timespec));

		/* Fill in the data. */
		memcpy(echo_request.data, ICMP_FILL_DATA, ICMP_FILL_SIZE);

		/* Calculate packet checksum. */
		echo_request.icmp_header.icmp_cksum = in_cksum((uint16_t *)&echo_request, sizeof(icmp4_echo));

		/* Set the to_addr, a real sockaddr_in is needed instead of strings. */
		memset(&to_addr, 0, sizeof(sockaddr_in));
		to_addr.sin_family = AF_INET;
		to_addr.sin_addr.s_addr = inet_addr(parent_lbnode->address.c_str());
		to_addr.sin_port = htons(0);

		/* Send the echo request. */
		int bsent = sendto(socket4_fd, (void *) &echo_request, sizeof(icmp4_echo), 0, (struct sockaddr *) &to_addr, sizeof(struct sockaddr_in));
		if (bsent<0) {
			return false;
		}

	} else if (parent_lbnode->address_family == AF_INET6) {
		struct sockaddr_in6	 to_addr;
		struct icmp6_echo	 echo_request;

		/* Build the ICMP Echo Request packet to be send. */
		memset(&echo_request, 0, sizeof(echo_request));

		/* Fill in the headers. */
		echo_request.icmp6_header.icmp6_type = ICMP6_ECHO_REQUEST;
		echo_request.icmp6_header.icmp6_code = 0;
		echo_request.icmp6_header.icmp6_id = htons(ping_id);
		echo_request.icmp6_header.icmp6_seq = htons(ping_my_seq);

		/* Remember time when this request was sent. */
		memcpy(&echo_request.timestamp, now, sizeof(struct timespec));

		/* Fill in the data. */
		memcpy(echo_request.data, ICMP_FILL_DATA, ICMP_FILL_SIZE);

		/* Calculate packet checksum. */
		echo_request.icmp6_header.icmp6_cksum = in_cksum((uint16_t *)&echo_request, sizeof(icmp6_echo));

		/* Set the to_addr, a real sockaddr_in is needed instead of strings. */
		memset(&to_addr, 0, sizeof(sockaddr_in6));
		to_addr.sin6_family = AF_INET6;
		inet_pton(AF_INET6, parent_lbnode->address.c_str(), &to_addr.sin6_addr);
		to_addr.sin6_port = htons(0);

		/* Send the echo request. */
		int bsent = sendto(socket6_fd, (void *) &echo_request, sizeof(icmp6_echo), 0, (struct sockaddr *) &to_addr, sizeof(struct sockaddr_in6));
		if (bsent<0) {
			return false;
		}
	}

	return true;
}


/*
 * Override end_check() method to clean up things
 */
void Healthcheck_ping::end_check(HealthcheckResult result, string message) {

	/*
	 * Clean sequence mapping for this instance of Healthcheck.
	 * This is required so that ICMP Echo Reply coming after timeout
	 * won't match.
	 */
	seq_map[this->ping_my_seq] = NULL;

	/* Call parent method. */
	Healthcheck::end_check(result, message);
}
