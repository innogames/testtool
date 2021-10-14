//
// Testtool - Health check - TCP
//
// Copyright (c) 2018 InnoGames GmbH
//

#ifndef _CHECK_TCP_HPP_
#define _CHECK_TCP_HPP_

#include <event2/bufferevent.h>
#include <sstream>
#include <unistd.h>
#include <vector>

#include "healthcheck.h"

class Healthcheck_tcp : public Healthcheck {

  // Methods
public:
  Healthcheck_tcp(const nlohmann::json &config, class LbNode *_parent_lbnode,
                  string *ip_address);
  static void check_tcp_callback(struct evtcp_request *req, void *arg);
  int schedule_healthcheck(struct timespec *now);

protected:
  static void event_callback(struct bufferevent *bev, short events, void *arg);
  void end_check(HealthcheckResult result, string message);

  // Members
private:
  bufferevent *bev;
  int port;
};

#endif
