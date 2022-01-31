//
// Testtool - Health check - TCP
//
// Copyright (c) 2018 InnoGames GmbH
//

#define FMT_HEADER_ONLY

#include <arpa/inet.h>
#include <errno.h>
#include <event2/buffer.h>
#include <event2/event_struct.h>
#include <event2/util.h>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string.h>
#include <sys/socket.h>
#include <vector>

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
Healthcheck_tcp::Healthcheck_tcp(const nlohmann::json &config,
                                 class LbNode *_parent_lbnode,
                                 string *ip_address)
    : Healthcheck(config, _parent_lbnode, ip_address) {
  this->port = safe_get<int>(config, "hc_port", 80);
  type = "tcp";

  this->log_prefix = fmt::sprintf("port: %d", this->port);
}

/// Libevent callback for TCP healthcheck.
///
/// It's a static method that requires the Healthcheck object to be passed to
/// it.
void Healthcheck_tcp::event_callback(struct bufferevent *bev, short events,
                                     void *arg) {
  (void)(bev);
  Healthcheck_tcp *hc = (Healthcheck_tcp *)arg;

  string message = fmt::sprintf("wrong event %d", events);
  HealthcheckResult result = HealthcheckResult::HC_PANIC;

  if (events & BEV_EVENT_CONNECTED) {
    result = HealthcheckResult::HC_PASS;
    message = "connection successful";
  }

  if (events & BEV_EVENT_ERROR) {
    result = HealthcheckResult::HC_FAIL;
    message = "connection refused";
  }

  if (events & BEV_EVENT_TIMEOUT) {
    result = HealthcheckResult::HC_FAIL;
    message = fmt::sprintf("timeout after %dms", hc->timeout_to_ms());
  }

  return hc->end_check(result, message);
}

int Healthcheck_tcp::schedule_healthcheck(struct timespec *now) {
  int result;

  // Peform general stuff for scheduled healthcheck.
  if (Healthcheck::schedule_healthcheck(now) == false)
    return false;

  bev = bufferevent_socket_new(eventBase, -1, 0 | BEV_OPT_CLOSE_ON_FREE);
  if (bev == NULL) {
    throw HealthcheckSchedulingException(
        fmt::sprintf("bufferevent_socket_new errno %d", errno));
  }

  int pton_res;
  if (address_family == AF_INET) {
    struct sockaddr_in to_addr4;
    memset(&to_addr4, 0, sizeof(to_addr4));
    to_addr4.sin_family = AF_INET;
    pton_res = inet_pton(AF_INET, ip_address->c_str(), &to_addr4.sin_addr);
    to_addr4.sin_port = htons(port);
    result = bufferevent_socket_connect(bev, (struct sockaddr *)&to_addr4,
                                        sizeof(to_addr4));
  } else if (address_family == AF_INET6) {
    struct sockaddr_in6 to_addr6;
    memset(&to_addr6, 0, sizeof(to_addr6));
    to_addr6.sin6_family = AF_INET6;
    pton_res = inet_pton(AF_INET6, ip_address->c_str(), &to_addr6.sin6_addr);
    to_addr6.sin6_port = htons(port);
    result = bufferevent_socket_connect(bev, (struct sockaddr *)&to_addr6,
                                        sizeof(to_addr6));
  } else {
    throw HealthcheckSchedulingException(
        fmt::sprintf("Wrong address family: %d", address_family));
  }

  if (result == -1 && EVUTIL_SOCKET_ERROR() != EINPROGRESS) {
    this->end_check(
        HealthcheckResult::HC_FAIL,
        fmt::sprintf("connect() error: %d errno: %s pton: %d", result,
                     evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()),
                     pton_res));

    return false;
  }

  bufferevent_set_timeouts(bev, &this->timeout, &this->timeout);
  bufferevent_setcb(bev, NULL, NULL, &event_callback, this);
  // bufferevent_setcb(bev, &rw_callback, &rw_callback, &event_callback, this);
  bufferevent_enable(bev, 0);
  // bufferevent_enable(bev, EV_READ | EV_WRITE);

  return true;
}

/// Overrides end_check() method to clean up things
void Healthcheck_tcp::end_check(HealthcheckResult result, string message) {

  if (verbose >= 2 && result != HealthcheckResult::HC_PASS &&
      this->bev != NULL) {
    char *error = evutil_socket_error_to_string(evutil_socket_geterror());

    if (error != NULL && strlen(error) > 0)
      message += fmt::sprintf(", socket() error: %s", strerror(errno));
  }

  if (this->bev != NULL) {
    bufferevent_free(this->bev);
    this->bev = NULL;
  }

  Healthcheck::end_check(result, message);
}
