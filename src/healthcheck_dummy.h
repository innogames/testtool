//
// Testtool - Health check - dummy for tests
//
// Copyright (c) 2020 InnoGames GmbH
//

#ifndef _CHECK_DUMMY_HPP_
#define _CHECK_DUMMY_HPP_

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/util.h>
#include <sstream>
#include <unistd.h>
#include <vector>

#include "healthcheck.h"

class Healthcheck_dummy : public Healthcheck {

  // Methods
public:
  Healthcheck_dummy(const nlohmann::json &config, class LbNode *_parent_lbnode,
                    string *ip_address);
  static void check_dummy_callback(struct evtcp_request *req, void *arg);
  int schedule_healthcheck(struct timespec *now);
  void dummy_end_check(HealthcheckResult result, string message);

protected:
  static void callback(evutil_socket_t fd, short what, void *arg);
};

#endif
