#include <iostream>
#include <sstream>
#include <vector>

#include <errno.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

#include <event2/event.h>
#include <event2/event_struct.h>

#include "healthcheck.h"
#include "healthcheck_ping.h"
#include "msg.h"

using namespace std;

extern struct event_base	*eventBase;
extern int			 verbose;


/* In the .h file there are only declarations, here we have definitions. */
int			 Healthcheck_ping::socket_fd;
struct event		*Healthcheck_ping::ev;
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

	/* So I was told that using the pid number for ping id is the Unix Way... */
	ping_id = getpid();

	/* Allocate memory for seq map. */
	seq_map = (Healthcheck_ping**)calloc(1<<(sizeof(uint16_t)*8), sizeof(Healthcheck_ping*) );

	/* Create a socket. */
	socket_fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (socket_fd == -1) {
		printf("socket(): %s\n", strerror(errno));
		return false;
	}

	/* The default 9kB buffer loses some packets. */
	int newbuf = 262144;
	setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &newbuf, sizeof(int));

	int bufsize;
	socklen_t bufbuflen;
	getsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, (char*)&bufsize, &bufbuflen);
	printf ("The Healthcheck_ping's socket buffer is %d bytes long.\n", bufsize);

	/* Create an event and make it pending. */
	ev = event_new(eventBase, socket_fd, EV_READ|EV_PERSIST, Healthcheck_ping::callback, NULL);
	event_add(ev, NULL);

	return true;
}


/*
   A common destructor for all healthchecks of this type. Should be called when testtool terminates.
*/
void Healthcheck_ping::destroy() {
	event_del(ev);
	event_free(ev);
	close(socket_fd);
	free(seq_map);
}


/*
   Constructor for ping healthcheck. Parses ping-specific parameters.
*/
Healthcheck_ping::Healthcheck_ping(string &definition, class Service &service): Healthcheck(definition, service) {
	/* Oh wait, there are none for this test! */

	if (verbose>0)
		cout << endl;
}


/*
   The callback is called by libevent, it's a static method.
   Unfortunately for ping tests there is only one socket so it is impossible
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
	char			 raw_packet[IP_MAXPACKET];
	struct ip		*ip_packet;
	int			 ip_header_len;
	struct icmp_echo_struct	*icmp_packet;
	struct timespec		 now;
	int			 received_bytes;

	/* There should be no other event types. */
	if (what != EV_READ)
		return;

	clock_gettime(CLOCK_MONOTONIC, &now);

	/* Prepare place for receiving data. */
	memset(raw_packet, 0, sizeof(raw_packet));

	/* Read a packet from the socket. */
	received_bytes = recvfrom(socket_fd, raw_packet, sizeof(raw_packet), 0, NULL, NULL);
	if (received_bytes <=0 ) {
		printf("recvfrom error\n");
		perror("recvfrom");
	}

	ip_packet = (struct ip *)raw_packet;
	
	ip_header_len = ip_packet->ip_hl << 2; /* IHL is the number of 32-bit (4 bytes) words, multiply it by 4 to get bytes. */

	/* First we must check if the received packet is:
	   - ICMP at all
	   - A valid ICMP packet.
	   - Addressed to us (by ping id).
	 */

	/* Is it ICMP? */
	if (ip_packet->ip_p != IPPROTO_ICMP && received_bytes < ip_header_len + ICMP_MINLEN)
		return;

	/* Looks like a real ICMP packet, let us proceed decoding it. */
	icmp_packet = (struct icmp_echo_struct *)(raw_packet + ip_header_len);

	/* A destination unreachable packet contains the IP header of the original request.

	   We have to move our pointers a bit. */
	if (icmp_packet->icmp_header.icmp_type == ICMP_UNREACH) {
		ip_packet = (struct ip*)&icmp_packet->icmp_header.icmp_data; /* Move to place after icmp header. */
	
		ip_header_len = ip_packet->ip_hl << 2; /* IHL is the number of 32-bit (4 bytes) words, multiply it by 4 to get bytes. */

		/* Cast ip_packet to char*, so + operation on pointer is performed in bytes and not in struct ips. */
		icmp_packet = (struct icmp_echo_struct *)((char*)ip_packet + ip_header_len);

		/* Is it addressed to us? */
		if (icmp_packet->icmp_header.icmp_id != htons(ping_id))
			return;

		/* Now let's map the received packet to a Healthcheck_icmp object. */
		uint16_t this_seq = ntohs(icmp_packet->icmp_header.icmp_seq);
		if (seq_map[this_seq] != NULL && seq_map[this_seq]->ping_my_seq == this_seq)
			healthcheck = seq_map[this_seq];
		else
			return;

		if (verbose>1 || healthcheck->hard_state != STATE_DOWN)
			showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - healthcheck_%s "CL_RED"Received a Destination Unreachable message, seq: %d."CL_RESET"\n",
					healthcheck->parent->name.c_str(), healthcheck->address.c_str(), healthcheck->port, healthcheck->type.c_str(), ntohs(icmp_packet->icmp_header.icmp_seq));

		healthcheck->last_state = STATE_DOWN;
		healthcheck->handle_result();
		return;
	}

	
	/* Finally! The answer we are waiting for! Yay! */
	if (icmp_packet->icmp_header.icmp_type == ICMP_ECHOREPLY) {
		/* Is it addressed to us? */
		if (icmp_packet->icmp_header.icmp_id != htons(ping_id))
			return;

		/* Now let's map the received packet to a Healthcheck_icmp object. */
		uint16_t this_seq = ntohs(icmp_packet->icmp_header.icmp_seq);
		if (seq_map[this_seq] != NULL && seq_map[this_seq]->ping_my_seq == this_seq)
			healthcheck = seq_map[this_seq];
		else
			return;

		long int nsec_diff  = (now.tv_sec - icmp_packet->timestamp.tv_sec) * 1000000000 + (now.tv_nsec - icmp_packet->timestamp.tv_nsec);

		int ms_full = nsec_diff / 1000000;
		int ms_dec  = (nsec_diff - ms_full * 1000000) / 1000;

		if (verbose>1 || healthcheck->last_state == STATE_DOWN)
			showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - healthcheck_%s "CL_GREEN"got ICMP Echo Reply in: %d.%dms, seq: %d"CL_RESET"\n",
					healthcheck->parent->name.c_str(), healthcheck->address.c_str(), healthcheck->port, healthcheck->type.c_str(), ms_full, ms_dec, ntohs(icmp_packet->icmp_header.icmp_seq));

		healthcheck->last_state = STATE_UP;
		healthcheck->handle_result();
		return;
	}
}


int Healthcheck_ping::schedule_healthcheck() {
	struct sockaddr_in	 to_addr;
	struct icmp_echo_struct	 echo_request;

	struct timespec		 timediff;
	struct timespec		 now;

	/* Special case for raw sockets where no proper timeout callback is possible.
	   Check when was the last ping sent for this Healthcheck instance and generate a timeout error if too much time has passed. */
	if (is_running) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		timediff = now; /* Don't break "now", it is needed later. */

		timespecsub(&timediff, &last_checked);

		if (timespeccmp(&timediff, &timeout ,> )) {
			if (verbose>1 || hard_state != STATE_DOWN)
				showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - healthcheck_%s "CL_RED"timeout after %d,%ds, seq %d"CL_RESET"\n",
						parent->name.c_str(), address.c_str(), port, type.c_str(), timeout.tv_sec, (timeout.tv_nsec/10000000), ping_my_seq);

			ping_my_seq = 0;	
			last_state = STATE_DOWN;
			handle_result();
			return false;
		}
	}

	/* Peform general stuff for scheduled healthcheck. */
	if (Healthcheck::schedule_healthcheck() == false)
		return false;

	/* Increase the ICMP sequence number. It is zeroed elsewehere, so we always start with 1.*/
	ping_global_seq ++;

	/* Create the mapping between the ping Sequence Number and the object. */
	ping_my_seq = ping_global_seq;
	seq_map[ping_my_seq] = this;

	/* Build the ICMP Echo Request packet to be send. */
	memset(&echo_request, 0, sizeof(echo_request));

	/* Fill in the headers. */
	echo_request.icmp_header.icmp_type = ICMP_ECHO;
	echo_request.icmp_header.icmp_code = 0;
	echo_request.icmp_header.icmp_id = htons(ping_id);
	echo_request.icmp_header.icmp_seq = htons(ping_my_seq);

	clock_gettime(CLOCK_MONOTONIC, &now);
	echo_request.timestamp = now;

	/* Fill in the data. */
	memcpy(echo_request.data, ICMP_FILL_DATA, ICMP_FILL_SIZE); 

	/* Calculate packet checksum. */
	echo_request.icmp_header.icmp_cksum = in_cksum((uint16_t *)&echo_request, sizeof(icmp_echo_struct));

	/* Set the to_addr, a real sockaddr_in is needed instead of strings. */
	memset(&to_addr, 0, sizeof(sockaddr_in));
	to_addr.sin_family = AF_INET;
	to_addr.sin_addr.s_addr = inet_addr(address.c_str());
	to_addr.sin_port = htons(port);

	/* Send the echo request. */
	int bsent = sendto(socket_fd, (void *) &echo_request, sizeof(icmp_echo_struct), 0, (struct sockaddr *) &to_addr, sizeof(struct sockaddr_in));
	if (bsent<0) {
		return false;
	}

	return true;
}

