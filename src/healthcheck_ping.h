//
// Testtool - Ping Health Check
//
// Copyright (c) 2018 InnoGames GmbH
//

#ifndef _CHECK_PING_H_
#define _CHECK_PING_H_

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/util.h>
#include <netinet/icmp6.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/ip_icmp.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

#include "healthcheck.h"

#define ICMP_FILL_DATA                                                         \
  "Dave, this conversation can serve no purpose anymore. Goodbye."

#define ICMP_FILL_SIZE sizeof(ICMP_FILL_DATA)

// As far as I understand, the target system should just reflect the packet to
// us, therefore apart from some basic, necessary headers, the rest of packet is
// all usable for any purposes we want.
struct icmp4_echo {
  struct icmp icmp_header;   // Contains icmp_type, _code, _cksum, _id and _seq
                             // and some other stuff.
  struct timespec timestamp; // I'm gonna go build my own timestamp. With
                             // blackjack and hookers!
  char data[ICMP_FILL_SIZE];
};

struct icmp6_echo {
  struct icmp6_hdr icmp6_header; // Contains icmp_type, _code, _cksum,
                                 // _id, _seq
  struct timespec timestamp;     // I'm gonna go build my own timestamp. With
                                 // blackjack and hookers!
  char data[ICMP_FILL_SIZE];
};

union icmp_echo {
  struct icmp4_echo icmp4;
  struct icmp6_echo icmp6;
};

class Healthcheck_ping : public Healthcheck {

  // Methods
public:
  Healthcheck_ping(const nlohmann::json &config, class LbNode *_parent_lbnode,
                   string *ip_address);
  int schedule_healthcheck(struct timespec *now);
  static int initialize();
  static void destroy();
  void finalize_result();

protected:
  void end_check(HealthcheckResult result, string message);
  static void callback(evutil_socket_t fd, short what, void *arg);

  // Members
private:
  // Some variables and functions are static for all ping healthchecks.
  static int socket4_fd;
  static int socket6_fd;
  static struct event *ev4;
  static struct event *ev6;
  static uint16_t ping_id;
  static uint16_t ping_global_seq;
  uint16_t ping_my_seq;

  // As ICMP socket is a raw one, we need some trick to map Echo Response to the
  // object which sent the Echo Request. So let us map the ICMP Sequence
  // Number to the Object. The object itself also knows the last Sequence
  // Number it sent.
  static Healthcheck_ping **seq_map;
};

#endif
