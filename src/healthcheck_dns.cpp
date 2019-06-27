//
// Testtool - DNS Health Check
//
// It has the following limitations:
//
// * It does not support truncated messages.  Only the first answering
//   datagram is parsed.
// * It does not really check for what is in the answer.  It only
//   checks if the number of answer sections is bigger than 0 and
//   the transaction number.
//
// Copyright (c) 2018 InnoGames GmbH
//

#include <arpa/inet.h>
#include <errno.h>
#include <event2/event.h>
#include <event2/event_struct.h>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <iostream>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <vector>

#include "config.h"
#include "healthcheck.h"
#include "healthcheck_dns.h"
#include "lb_node.h"
#include "lb_pool.h"
#include "msg.h"

using namespace std;

extern struct event_base *eventBase;
extern int verbose;

// In the .h file there are only declarations of static variables,
// here we have definitions.
uint16_t Healthcheck_dns::global_transaction_id;

static unsigned int build_dns_question(string &dns_query,
                                       char *question_buffer);

/// Constructor for DNS healthcheck.
///
/// It parses DNS-specific parameters.
Healthcheck_dns::Healthcheck_dns(const nlohmann::json &config,
                                 class LbNode *_parent_lbnode,
                                 string *ip_address)
    : Healthcheck(config, _parent_lbnode, ip_address) {

  // Set defaults
  this->type = "dns";
  this->port = safe_get<int>(config, "hc_port", 53);
  this->dns_query = safe_get<string>(config, "hc_query", ".");
  if (this->dns_query.at(this->dns_query.length() - 1) != '.')
    this->dns_query += '.';

  this->log_prefix =
      fmt::sprintf("query: '%s' port: %d", this->dns_query, this->port);
}

int Healthcheck_dns::schedule_healthcheck(struct timespec *now) {
  char raw_packet[DNS_BUFFER_SIZE]; // This should be enough for our purposes.
  unsigned int question_length;
  unsigned int total_length;
  struct sockaddr_in to_addr4;
  struct sockaddr_in6 to_addr6;

  // Peform general stuff for scheduled healthcheck
  if (Healthcheck::schedule_healthcheck(now) == false)
    return false;

  // Prepare memory
  memset(&raw_packet, 0, sizeof(raw_packet));

  struct dns_header *dns_query_struct = (struct dns_header *)raw_packet;
  char *dns_question = raw_packet + sizeof(struct dns_header);

  // Fill in the query struct
  dns_query_struct->qid =
      htons(my_transaction_id = ++Healthcheck_dns::global_transaction_id);

  dns_query_struct->rd = 1; // 1 bit.  Do the recursvie query if needed.
  dns_query_struct->tc = 0; // 1 bit.  Message is not truncated.
  //	dns_query_struct->aa;         // 1 bit.  Valid in responses.
  dns_query_struct->opcode = 0; // 4 bits. Normal message.
  dns_query_struct->qr = 0;     // 1 bit.  It is a query.

  //	dns_query_struct->rcode :4;   // 4 bits. Valid in responses.
  //	dns_query_struct->unused :3;  // 3 bits. It's unused.
  //	dns_query_struct->ra: 1;      // 1 bit.  Valid in responses.

  dns_query_struct->qdcount = htons(1); // There is 1 question to be sent.

  question_length = build_dns_question(dns_query, dns_question);
  total_length = sizeof(struct dns_header) + question_length;

  int pton_res;
  if (address_family == AF_INET) {
    memset(&to_addr4, 0, sizeof(to_addr4));
    to_addr4.sin_family = AF_INET;
    pton_res = inet_pton(AF_INET, ip_address->c_str(), &to_addr4.sin_addr);
    to_addr4.sin_port = htons(port);
    socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  } else if (address_family == AF_INET6) {
    memset(&to_addr6, 0, sizeof(to_addr6));
    to_addr6.sin6_family = AF_INET6;
    pton_res = inet_pton(AF_INET6, ip_address->c_str(), &to_addr6.sin6_addr);
    to_addr6.sin6_port = htons(port);
    socket_fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  } else {
    return false;
  }

  if (socket_fd == -1) {
    log(MSG_CRIT, this, fmt::sprintf("socket(): error %s", strerror(errno)));
    return false;
  }

  evutil_make_socket_nonblocking(socket_fd);

  // Sending to host is one thing, but we want answers only from
  // our target in this socket.  "connect" makes the socket
  // receive only traffic from that host.

  if (address_family == AF_INET) {
    connect(socket_fd, (struct sockaddr *)&to_addr4, sizeof(to_addr4));
  } else if (address_family == AF_INET6) {
    connect(socket_fd, (struct sockaddr *)&to_addr6, sizeof(to_addr6));
  }

  // Create an event and make it pending
  this->ev =
      event_new(eventBase, socket_fd, EV_READ, Healthcheck_dns::callback, this);
  event_add(this->ev, &this->timeout);

  // On connected socket we use send, not sendto.
  if (send(socket_fd, (void *)raw_packet, total_length, 0) < 0)
    return false;

  return true;
}

/// Builds a DNS packet's question section
///
/// The function requires an already allocated buffer, hopefully long
/// enough.  It returns the lenght of the created section.
//
/// Another option would be to allocate memory in this function.
/// Length would be strlen(dns_query) + 5 bytes.  But then, it would
/// have to be copied to buffer of packet to be sent.  This function
/// requires that dns_query is already ending with '.'.
static unsigned int build_dns_question(string &dns_query,
                                       char *question_buffer) {
  int label_start = 0;
  int label_len = 0;

  // Get the length of original string
  int origlen = strlen(dns_query.c_str());

  // Copy the original query to qname + 1
  strcpy(question_buffer + 1, dns_query.c_str());

  // Replace dots with lenght of labels after them (see RFC1035 4.1.2)
  for (int i = 1; i < origlen + 2; i++) {
    if (i == origlen + 1 || question_buffer[i] == '.') {
      question_buffer[label_start] = label_len;
      label_len = 0;
      label_start = i;
    } else {
      label_len++;
    }
  }

  // Set qtype to A record
  question_buffer[origlen + 2] = 1;

  // Set qclass to IN class
  question_buffer[origlen + 4] = 1;

  return origlen + 5;
}

/// The callback function for DNS check
void Healthcheck_dns::callback(evutil_socket_t socket_fd, short what,
                               void *arg) {
  Healthcheck_dns *healthcheck = (Healthcheck_dns *)arg;

  char raw_packet[DNS_BUFFER_SIZE];
  int bytes_received;
  struct dns_header *dns_query_struct = (struct dns_header *)raw_packet;
  string message = "bad event";
  HealthcheckResult result = HC_PANIC;

  // Prepare memory
  memset(&raw_packet, 0, sizeof(raw_packet));

  if (what & EV_TIMEOUT) {
    result = HC_FAIL;
    message =
        fmt::sprintf("timeout after %d.%03ds", healthcheck->timeout.tv_sec,
                     healthcheck->timeout.tv_usec / 1000);
  } else if (what & EV_READ) {
    bytes_received = recv(socket_fd, &raw_packet, DNS_BUFFER_SIZE, 0);

    if (bytes_received == -1) {
      // This happens when the target host is not in the arp table and
      // therefore nothing was ever sent to it. Although sending send() returns
      // no error. Or when an ICMP dst unreachable is received.
      result = HC_FAIL;
      message = "connection refused";
    } else if (bytes_received < (int)sizeof(struct dns_header) ||
               bytes_received > DNS_BUFFER_SIZE) {
      // Size of the received message shall be between the size of header and
      // the maximum dns packet size.
      result = HC_FAIL;
      message = "received malformed data";
    } else if (ntohs(dns_query_struct->ancount) == 0) {
      // No answers means that the server knows nothing about the domain.
      // Therefore it fails the check.
      result = HC_FAIL;
      message = "received no DNS answers";
    } else if (ntohs(dns_query_struct->qid) != healthcheck->my_transaction_id) {
      // Received transaction id must be the same as in the last query sent to
      // the server.
      result = HC_FAIL;
      message = "received wrong transaction id";
    } else {
      // Finally, it seems that all is fine.
      result = HC_PASS;
      message = "received a DNS answer";

      // We do not really check the contents of the answer sections.
      // Seeing that there is any answer given, we assume that the DNS server
      // functions properly.
    }
  }

  // Be sure to free the memory
  event_free(healthcheck->ev);
  close(socket_fd);
  healthcheck->end_check(result, message);
}
