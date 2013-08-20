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

#include <arpa/nameser.h>
#include <arpa/nameser_compat.h>

#include "lb_pool.h"
#include "lb_node.h"
#include "healthcheck.h"
#include "healthcheck_dns.h"
#include "msg.h"

using namespace std;

extern struct event_base	*eventBase;
extern int			 verbose;

uint16_t			 Healthcheck_dns::global_transaction_id;


/*
   The DNS healthcheck. It has the following limitations:
   - Does not support truncated messages. Only the first answering datagram is parsed.
   - Does not really check for what is in the answer. It only checks if the number of answer sections is bigger than 0.
*/


/*
   Build a question section. The function requires an already allocated buffer, hopefully long enough.
   It returns the lenght of the created section.
*/
unsigned int build_dns_question(char *dns_query, char *question_buffer) {
	/*
	   Another option would be to allocate memory in this function.
	   Length would be strlen(dns_query)+5 bytes.
	   But then it would have to be copied to buffer of packet to be sent.
	*/

	int label_start = 0;
	int label_len   = 0;

	/* Get the length of original string, if it does not end with a '.', make it one character longer. */
	int origlen = strlen(dns_query);
	if ( dns_query[origlen-1] != '.' )
		origlen++;

	/* Copy the original query to qname+1, make sure that the last char is a '.'. */
	strcpy(question_buffer+1,dns_query);
	question_buffer[origlen]='.';


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


/*
   Constructor for ping healthcheck. Parses ping-specific parameters.
*/
Healthcheck_dns::Healthcheck_dns(istringstream &definition, class LbNode *_parent_lbnode): Healthcheck(definition, string("dns"), _parent_lbnode) {

	/* The string "parameters" was filled in by Healthcheck constructor, now turing it into a stream to read all the params. */
	istringstream ss_parameters(parameters);

	/* Read record type. */
	std::string dns_query;

	getline(ss_parameters, dns_query, ':');
	this->dns_query = new char[dns_query.length()+1];
	strcpy(this->dns_query, dns_query.c_str());

	if (verbose>0)
		cout << "query: " << dns_query << endl;
}


void Healthcheck_dns::callback(evutil_socket_t socket_fd, short what, void *arg) {
	Healthcheck_dns *healthcheck = (Healthcheck_dns *)arg;

	char			 raw_packet[DNS_BUFFER_SIZE];
	int			 bytes_received;
	struct dns_header	*dns_query_struct = (struct dns_header*)raw_packet;

	/* Prepare memory. It must be pure and clean. */
	memset (&raw_packet, 0, sizeof(raw_packet));

	if (what & EV_TIMEOUT) {
		healthcheck->last_state = STATE_DOWN;
		showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - Healthcheck_%s: "CL_RED"timeout after %d,%ds"CL_RESET"\n",
			healthcheck->parent_lbnode->parent_lbpool->name.c_str(), healthcheck->parent_lbnode->address.c_str(), healthcheck->port, healthcheck->type.c_str(), healthcheck->timeout.tv_sec, (healthcheck->timeout.tv_nsec/10000000));
	}
	else if (what & EV_READ) {
		bytes_received = recv(socket_fd, &raw_packet, DNS_BUFFER_SIZE, 0);

		if (bytes_received == -1) {
			/* This happens when the target host is not in the arp table and therefore nothing was even sent to it.
			   Although sending send() returns no error. */
			healthcheck->last_state = STATE_DOWN;
			showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - Healthcheck_%s: "CL_RED"connection rejected"CL_RESET"\n",
					healthcheck->parent_lbnode->parent_lbpool->name.c_str(), healthcheck->parent_lbnode->address.c_str(), healthcheck->port, healthcheck->type.c_str());
		}
		else if (bytes_received < (int)sizeof(struct dns_header) || bytes_received > DNS_BUFFER_SIZE) {
			/* There should be at least dns_header received. */
			healthcheck->last_state = STATE_DOWN;
			showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - Healthcheck_%s: "CL_RED"received malformed data"CL_RESET"\n",
					healthcheck->parent_lbnode->parent_lbpool->name.c_str(), healthcheck->parent_lbnode->address.c_str(), healthcheck->port, healthcheck->type.c_str());
		}
		else if (ntohs(dns_query_struct->ancount) == 0 ) {
			/* No answers means that the server knows nothing about the domain. Therefore it fails the test. */
			healthcheck->last_state = STATE_DOWN;
			showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - Healthcheck_%s: "CL_RED"received no DNS answers"CL_RESET"\n",
					healthcheck->parent_lbnode->parent_lbpool->name.c_str(), healthcheck->parent_lbnode->address.c_str(), healthcheck->port, healthcheck->type.c_str());
		}
		else if (ntohs(dns_query_struct->qid) != healthcheck->my_transaction_id ) {
			/* Received transaction id must be the same as in the last query sent to the server. */
			healthcheck->last_state = STATE_DOWN;
			showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - Healthcheck_%s: "CL_RED"received wrong transaction id"CL_RESET"\n",
					healthcheck->parent_lbnode->parent_lbpool->name.c_str(), healthcheck->parent_lbnode->address.c_str(), healthcheck->port, healthcheck->type.c_str());
		} else {
			/* Finally it seems that all is fine. */
			if (verbose>1 || healthcheck->last_state == STATE_DOWN)
				showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - Healthcheck_%s: "CL_GREEN"received a DNS answer"CL_RESET"\n",
					healthcheck->parent_lbnode->parent_lbpool->name.c_str(), healthcheck->parent_lbnode->address.c_str(), healthcheck->port, healthcheck->type.c_str());
			healthcheck->last_state = STATE_UP; /* Service is UP */

			/*
			   We do not really check the contents of the answer sections.
			   Seeing that there is any answer given, we know that the DNS server functions properly.
			*/
		}
	}

	event_del(healthcheck->ev);
	close(socket_fd);
	healthcheck->handle_result();
}


int Healthcheck_dns::schedule_healthcheck() {
	char			raw_packet[DNS_BUFFER_SIZE]; /* This should be enough for our purposes. */
	struct sockaddr_in	to_addr;
	unsigned int		question_length;
	unsigned int		total_length;

	/* Peform general stuff for scheduled healthcheck. */
	if (Healthcheck::schedule_healthcheck() == false)
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
		printf("socket(): %s\n", strerror(errno));
		return false;
	}
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

