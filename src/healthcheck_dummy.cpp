//
// Testtool - Health check - dummy for tests
//
// Copyright (c) 2020 InnoGames GmbH
//

#include <event2/event_struct.h>
#include <event2/util.h>
#include <nlohmann/json.hpp>
#include <string.h>
#include <vector>

#include "config.h"
#include "healthcheck.h"
#include "healthcheck_dummy.h"
#include "lb_node.h"
#include "lb_pool.h"
#include "msg.h"

using namespace std;

/// Constructor for Dummy healthcheck.
Healthcheck_dummy::Healthcheck_dummy(const nlohmann::json &config,
                                     class LbNode *_parent_lbnode,
                                     string *ip_address)
    : Healthcheck(config, _parent_lbnode, ip_address) {
  type = "dummy";
}

/// Libecent callback for Dummy healthcheck.
void Healthcheck_dummy::callback(evutil_socket_t socket_fd, short what,
                                 void *arg) {
  Healthcheck_dummy *hc = (Healthcheck_dummy *)arg;
  string message = "dummy result";
  HealthcheckResult result = HC_PANIC;
  hc->end_check(result, message);
}

int Healthcheck_dummy::schedule_healthcheck(struct timespec *now) {
  // Peform general stuff for scheduled healthcheck.
  if (Healthcheck::schedule_healthcheck(now) == false)
    return false;

  return true;
}

void Healthcheck_dummy::dummy_end_check(HealthcheckResult result,
                                        string message) {
  end_check(result, message);
}