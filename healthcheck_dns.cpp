#include <iostream>
#include <sstream>
#include <vector>

#include <errno.h>
#include <string.h>
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

#include "msg.h"

#include "lb_pool.h"
#include "lb_node.h"
#include "healthcheck.h"
#include "healthcheck_dns.h"

using namespace std;

extern struct event_base	*eventBase;
extern int			 verbose;


/* In the .h file there are only declarations of static variables, here we have definitions. */
uint16_t			 Healthcheck_dns::global_transaction_id;


/*
   The DNS healthcheck. It has the following limitations:
   - Does not support truncated messages. Only the first answering datagram is parsed.
   - Does not really check for what is in the answer. It only checks if the number of answer sections is bigger than 0 and the transaction number.
*/


/*
   Build a question section. The function requires an already allocated buffer, hopefully long enough.
   It returns the lenght of the created section.
*/
unsigned int build_dns_question(string &dns_query, char *question_buffer) {
	/*
	   Another option would be to allocate memory in this function.
	   Length would be strlen(dns_query)+5 bytes.
	   But then it would have to be copied to buffer of packet to be sent.
	   This function requires that dns_query is already ending with '.'.
	*/

	int label_start = 0;
	int label_len   = 0;

	/* Get the length of original string. */
	int origlen = strlen(dns_query.c_str());

	/* Copy the original query to qname+1. */
	strcpy(question_buffer+1,dns_query.c_str());

	/* Replace dots with lenght of labels after them. See RFC1035 4.1.2. */
	for (int i=1; i<origlen+2; i++) {
		if (i==origlen+1 || question_buffer[i] == '.') {
			question_buffer[label_start] = label_len;
			label_len = 0;
			label_start = i;
		} else {
			label_len ++;
		}
	}

	/* Set qtype to A record. */
	question_buffer[origlen+2] = 1;

	/* Set qclass to IN class. */
	question_buffer[origlen+4] = 1;

	return origlen+5;
}

void Healthcheck_dns::confline_callback(string &var, istringstream &val) {
	if (var == "query") {
		val >> this->dns_query;
		/* Add ensure that query ends with dot. */
		if (this->dns_query.at(this->dns_query.length()-1) != '.') {
			this->dns_query += '.';
		}
	}
}

/*
   Constructor for DNS healthcheck. Parses DNS-specific parameters.
*/
Healthcheck_dns::Healthcheck_dns(istringstream &definition, class LbNode *_parent_lbnode): Healthcheck(definition, _parent_lbnode) {
	/* Set defaults. */
	this->port = 53;
	this->read_confline(definition);
	log_txt(MSG_TYPE_DEBUG, "      type: dns, port: %d, query: %s", this->port, this->dns_query.c_str());

	type = "dns";
}


/*
   The callback function for DNS check.
*/
void Healthcheck_dns::callback(evutil_socket_t socket_fd, short what, void *arg) {
	Healthcheck_dns *healthcheck = (Healthcheck_dns *)arg;

	char			 raw_packet[DNS_BUFFER_SIZE];
	int			 bytes_received;
	struct dns_header	*dns_query_struct = (struct dns_header*)raw_packet;

	/* Prepare memory. It must be pure and clean. */
	memset (&raw_packet, 0, sizeof(raw_packet));

	if (what & EV_TIMEOUT) {
		healthcheck->last_state = STATE_DOWN;
		if (verbose>1 || healthcheck->hard_state != STATE_DOWN)
			log_lb(MSG_TYPE_HC_FAIL,
			    healthcheck->parent_lbnode->parent_lbpool->name.c_str(),
			    healthcheck->parent_lbnode->address.c_str(),
			    healthcheck->port,
			    "Healthcheck_%s: timeout after %d,%03ds",
			    healthcheck->type.c_str(),
			    healthcheck->timeout.tv_sec,
			    (healthcheck->timeout.tv_nsec/1000000));
	}
	else if (what & EV_READ) {
		bytes_received = recv(socket_fd, &raw_packet, DNS_BUFFER_SIZE, 0);

		if (bytes_received == -1) {
			/* This happens when the target host is not in the arp table and therefore nothing was ever sent to it.
			   Although sending send() returns no error.
			   Or when an ICMP dst unreachable is received */
			healthcheck->last_state = STATE_DOWN;
			if (verbose>1 || healthcheck->hard_state != STATE_DOWN)
				log_lb(MSG_TYPE_HC_FAIL,
				    healthcheck->parent_lbnode->parent_lbpool->name.c_str(),
				    healthcheck->parent_lbnode->address.c_str(),
				    healthcheck->port,
				    "Healthcheck_%s: connection rejected",
				    healthcheck->type.c_str());
		}
		else if (bytes_received < (int)sizeof(struct dns_header) || bytes_received > DNS_BUFFER_SIZE) {
			/* Size of the received message shall be between the size of header and the maximum dns packet size. */
			healthcheck->last_state = STATE_DOWN;
			if (verbose>1 || healthcheck->hard_state != STATE_DOWN)
				log_lb(MSG_TYPE_HC_FAIL,
				    healthcheck->parent_lbnode->parent_lbpool->name.c_str(),
				    healthcheck->parent_lbnode->address.c_str(),
				    healthcheck->port,
				    "Healthcheck_%s: received malformed data",
				    healthcheck->type.c_str());
		}
		else if (ntohs(dns_query_struct->ancount) == 0 ) {
			/* No answers means that the server knows nothing about the domain. Therefore it fails the check. */
			healthcheck->last_state = STATE_DOWN;
			if (verbose>1 || healthcheck->hard_state != STATE_DOWN)
				log_lb(MSG_TYPE_HC_FAIL,
				    healthcheck->parent_lbnode->parent_lbpool->name.c_str(),
				    healthcheck->parent_lbnode->address.c_str(),
				    healthcheck->port,
				    "Healthcheck_%s: received no DNS answers",
				    healthcheck->type.c_str());
		}
		else if (ntohs(dns_query_struct->qid) != healthcheck->my_transaction_id ) {
			/* Received transaction id must be the same as in the last query sent to the server. */
			healthcheck->last_state = STATE_DOWN;
			if (verbose>1 || healthcheck->hard_state != STATE_DOWN)
				log_lb(MSG_TYPE_HC_FAIL,
				    healthcheck->parent_lbnode->parent_lbpool->name.c_str(),
				    healthcheck->parent_lbnode->address.c_str(),
				    healthcheck->port,
				    "Healthcheck_%s: received wrong transaction id",
				    healthcheck->type.c_str());
		} else {
			/* Finally it seems that all is fine. */
			if (verbose>1 || healthcheck->last_state == STATE_DOWN)
				log_lb(MSG_TYPE_HC_PASS,
				    healthcheck->parent_lbnode->parent_lbpool->name.c_str(),
				    healthcheck->parent_lbnode->address.c_str(),
				    healthcheck->port,
				    "Healthcheck_%s: received a DNS answer",
				    healthcheck->type.c_str());
			healthcheck->last_state = STATE_UP;

			/*
			   We do not really check the contents of the answer sections.
			   Seeing that there is any answer given, we know that the DNS server functions properly.
			*/
		}
	}

	/* Be sure to free the memory! */
	event_del(healthcheck->ev);
	close(socket_fd);
	healthcheck->handle_result();
}


int Healthcheck_dns::schedule_healthcheck(struct timespec *now) {
	char			raw_packet[DNS_BUFFER_SIZE]; /* This should be enough for our purposes. */
	struct sockaddr_in	to_addr;
	unsigned int		question_length;
	unsigned int		total_length;

	/* Peform general stuff for scheduled healthcheck. */
	if (Healthcheck::schedule_healthcheck(now) == false)
		return false;

	/* Prepare memory. It must be pure and clean. */
	memset (&raw_packet, 0, sizeof(raw_packet));

	struct dns_header	*dns_query_struct = (struct dns_header*)raw_packet;
	char			*dns_question = raw_packet + sizeof(struct dns_header);

	/* Fill in the query struct. */
	dns_query_struct->qid = htons(my_transaction_id = ++Healthcheck_dns::global_transaction_id);

	dns_query_struct->rd = 1;     /* 1 bit.  Do the recursvie query if needed. */
	dns_query_struct->tc = 0;     /* 1 bit.  Message is not truncated. */
//	dns_query_struct->aa;         /* 1 bit.  Valid in responses. */
	dns_query_struct->opcode = 0; /* 4 bits. Normal message. */
	dns_query_struct->qr = 0;     /* 1 bit.  It is a query. */

//	dns_query_struct->rcode :4;   /* 4 bits. Valid in responses. */
//	dns_query_struct->unused :3;  /* 3 bits. It's unused. */
//	dns_query_struct->ra: 1;      /* 1 bit.  Valid in responses. */

	dns_query_struct->qdcount = htons(1); /* There is 1 question to be sent. */

	question_length = build_dns_question(dns_query, dns_question);
	total_length = sizeof(struct dns_header) + question_length;

	/* Set the to_addr, a real sockaddr_in is needed instead of strings. */
	memset(&to_addr, 0, sizeof(sockaddr_in));
	to_addr.sin_family = AF_INET;
	to_addr.sin_addr.s_addr = inet_addr(parent_lbnode->address.c_str());
	to_addr.sin_port = htons(port);

	/* Create a socket. */
	socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (socket_fd == -1) {
		log_txt(MSG_TYPE_CRITICAL, "socket(): %s", strerror(errno));
		return false;
	}
	/* In fact I'm not really sure if it needs to be nonblocking. */
	evutil_make_socket_nonblocking(socket_fd);

	/* Sending to host is one thing, but we want answers only from our target in this socket.
	   "connect" makes the socket receive only traffic from that host. */
	connect(socket_fd, (struct sockaddr *) &to_addr, sizeof(sockaddr_in));

	/* Create an event and make it pending. */
	struct timeval timeout_tv;
	timeout_tv.tv_sec  = timeout.tv_sec;
	timeout_tv.tv_usec = timeout.tv_nsec / 1000;
	ev = event_new(eventBase, socket_fd, EV_READ|EV_TIMEOUT, Healthcheck_dns::callback, this);
	event_add(ev, &timeout_tv);

	/* On connected socket we use send, not sendto. */
	if (send(socket_fd, (void *) raw_packet, total_length, 0)<0)
		return false;

	return true;
}

