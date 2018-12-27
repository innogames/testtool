//
// Testtool - Health check - TCP
//
// Copyright (c) 2018 InnoGames GmbH
//

#include <arpa/inet.h>
#include <errno.h>
#include <event2/event_struct.h>
#include <event2/util.h>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <iostream>
#include <sstream>
#include <string.h>
#include <sys/socket.h>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "config.h"
#include "healthcheck.h"
#include "healthcheck_tcp.h"
#include "lb_node.h"
#include "lb_pool.h"
#include "msg.h"

using namespace std;

extern struct event_base *eventBase;
extern int verbose;

/// Constructor for TCP healthcheck.
Healthcheck_tcp::Healthcheck_tcp(const YAML::Node &config,
                                 class LbNode *_parent_lbnode,
                                 string *ip_address)
    : Healthcheck(config, _parent_lbnode, ip_address) {
  this->port = parse_int(config["hc_port"], 80);
  type = "tcp";

  this->log_prefix = fmt::sprintf("port: %d", this->port);
}

/// Libecent callback for TCP healthcheck.
///
/// it's a static method that requires the Healthcheck object to be passed to
/// it.
void Healthcheck_tcp::callback(evutil_socket_t socket_fd, short what,
                               void *arg) {
  Healthcheck_tcp *hc = (Healthcheck_tcp *)arg;
  string message = fmt::sprintf("wrong event %d", what);
  HealthcheckResult result = HC_PANIC;

  if (what & EV_TIMEOUT) {
    result = HC_FAIL;
    message = fmt::sprintf("timeout after %d.%03ds", hc->timeout.tv_sec,
                           hc->timeout.tv_usec / 1000);
  }

  if (what & EV_READ && result != HC_FAIL) {
    // A rejected connection is reported with READ event to us.
    char buf[256];
    if (read(socket_fd, buf, 255) == -1) {
      result = HC_FAIL;
      message = "connection refused";
    }
  }

  if (what & EV_WRITE && result != HC_FAIL) {
    result = HC_PASS;
    message = "connection successful";
  }

  // Be sure to free the memory!
  event_del(hc->ev);
  event_free(hc->ev);
  close(socket_fd);
  hc->end_check(result, message);
}

int Healthcheck_tcp::schedule_healthcheck(struct timespec *now) {
  int result;

  // Peform general stuff for scheduled healthcheck.
  if (Healthcheck::schedule_healthcheck(now) == false)
    return false;

  // Create a socket.
  socket_fd = socket(address_family, SOCK_STREAM, 0);
  if (socket_fd == -1) {
    this->end_check(HC_FAIL,
                    fmt::sprintf("socket() error %s", strerror(errno)));
    return false;
  }

  // In fact I'm not really sure if it needs to be nonblocking.
  evutil_make_socket_nonblocking(socket_fd);

  int pton_res;
  if (address_family == AF_INET) {
    struct sockaddr_in to_addr4;
    memset(&to_addr4, 0, sizeof(to_addr4));
    to_addr4.sin_family = AF_INET;
    pton_res = inet_pton(AF_INET, ip_address->c_str(), &to_addr4.sin_addr);
    to_addr4.sin_port = htons(port);
    result = connect(socket_fd, (struct sockaddr *)&to_addr4, sizeof(to_addr4));
  } else if (address_family == AF_INET6) {
    struct sockaddr_in6 to_addr6;
    memset(&to_addr6, 0, sizeof(to_addr6));
    to_addr6.sin6_family = AF_INET6;
    pton_res = inet_pton(AF_INET6, ip_address->c_str(), &to_addr6.sin6_addr);
    to_addr6.sin6_port = htons(port);
    result = connect(socket_fd, (struct sockaddr *)&to_addr6, sizeof(to_addr6));

  } else {
    return false;
  }

  if (result == -1 && errno != EINPROGRESS) {
    this->end_check(HC_FAIL, fmt::sprintf("connect() error %s pton %d",
                                          strerror(errno), pton_res));
    close(socket_fd);
    return false;
  }

  // Create an event and make it pending.
  ev = event_new(eventBase, socket_fd, EV_WRITE | EV_READ | EV_TIMEOUT,
                 Healthcheck_tcp::callback, this);
  event_add(ev, &this->timeout);

  return true;
}
