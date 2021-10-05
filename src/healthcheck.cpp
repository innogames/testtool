//
// Testtool - Health Check Generals
//
// Copyright (c) 2018 InnoGames GmbH
//

#define FMT_HEADER_ONLY

#include <fmt/format.h>
#include <fmt/printf.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdlib.h>
#include <typeinfo>

#include "config.h"
#include "healthcheck.h"
#include "healthcheck_dns.h"
#include "healthcheck_dummy.h"
#include "healthcheck_http.h"
#include "healthcheck_ping.h"
#include "healthcheck_postgres.h"
#include "healthcheck_tcp.h"
#include "lb_node.h"
#include "lb_pool.h"
#include "msg.h"
#include "pfctl.h"
#include "time_helper.h"

using namespace std;

extern int verbose;

/// Constructor of Healthcheck class.
///
/// Link the healthcheck and its parent node, initialize some variables,
/// print diagnostic information if necessary.  Remember that this
/// constructor is called from each healthcheck's type-specific
/// constructor!  And it is called *before* that constructor does its
/// own work!
Healthcheck::Healthcheck(const nlohmann::json &config,
                         class LbNode *_parent_lbnode, string *ip_address) {
  // Pretend that the healthcheck was performed just a moment ago.
  // This is necessary to perform the check in proper time.
  clock_gettime(CLOCK_MONOTONIC, &last_checked);

  // Link with parent lbnode.
  parent_lbnode = _parent_lbnode;
  parent_lbnode->healthchecks.push_back(this);
  this->ip_address = ip_address;

  // Determine type of IP address given to this health check.
  // Checks based on functions of libevent take IP address in string form
  // and perform their own magic. Custom checks like tcp or ping operate
  // on old style structures and have different code for each address
  // family so we can as well help them.
  struct addrinfo hint, *res = NULL;
  int ret;
  memset(&hint, 0, sizeof hint);
  hint.ai_family = PF_UNSPEC;
  hint.ai_flags = AI_NUMERICHOST;
  ret = getaddrinfo(this->ip_address->c_str(), NULL, &hint, &res);
  if (ret) {
    freeaddrinfo(res);
    throw(NotLbPoolException(fmt::sprintf("Unable to parse IP address '%s'",
                                          this->ip_address->c_str())));
  } else {
    this->address_family = res->ai_family;
    if (this->address_family == AF_INET)
      this->af_string = "IPv4";
    if (this->address_family == AF_INET6)
      this->af_string = "IPv6";
    freeaddrinfo(res);
  }

  // Set defaults, same as with old testtool.
  this->check_interval = safe_get<int>(config, "hc_interval", 2);
  this->max_failed_checks = safe_get<int>(config, "hc_max_failed", 3);
  int tmp_timeout = safe_get<int>(config, "hc_timeout", 1500);
  // Timeout was read in ms, convert it to s and μs.
  this->timeout.tv_sec = tmp_timeout / 1000;
  this->timeout.tv_usec = (tmp_timeout % 1000) * 1000;
  // Random delay to spread healthchecks in space-time continuum.
  this->extra_delay = rand() % 1000;

  this->is_running = false;
  this->ran = false;

  // Initialize healthchecks state basing on state of parent node.
  // Proper initial state for the healthcheck guarantees no
  // unnecessary messages.
  if (parent_lbnode->is_up()) {
    hard_state = HealthcheckState::STATE_UP;
    last_state = HealthcheckState::STATE_UP;
    failure_counter = 0;
  } else {
    hard_state = HealthcheckState::STATE_DOWN;
    last_state = HealthcheckState::STATE_DOWN;
    failure_counter = max_failed_checks;
  }
}

/// Healthcheck factory
///
/// - reads type of healthcheck
/// - creates an object of the required type, pass the config to it
/// - returns it
Healthcheck *Healthcheck::healthcheck_factory(const nlohmann::json &config,
                                              class LbNode *_parent_lbnode,
                                              string *ip_address) {

  Healthcheck *new_healthcheck = NULL;

  std::string type = safe_get<string>(config, "hc_type", "");

  if (type == "http")
    new_healthcheck = new Healthcheck_http(config, _parent_lbnode, ip_address);
  else if (type == "https")
    new_healthcheck = new Healthcheck_https(config, _parent_lbnode, ip_address);
  else if (type == "tcp")
    new_healthcheck = new Healthcheck_tcp(config, _parent_lbnode, ip_address);
  else if (type == "ping")
    new_healthcheck = new Healthcheck_ping(config, _parent_lbnode, ip_address);
  else if (type == "postgres")
    new_healthcheck =
        new Healthcheck_postgres(config, _parent_lbnode, ip_address);
  else if (type == "dns")
    new_healthcheck = new Healthcheck_dns(config, _parent_lbnode, ip_address);
  else if (type == "dummy")
    new_healthcheck = new Healthcheck_dummy(config, _parent_lbnode, ip_address);
  else
    return NULL;

  log(MessageType::MSG_INFO, new_healthcheck, "state: created");

  return new_healthcheck;
}

/// Schedule a healthcheck to be run
///
/// Healthcheck scheduling is done via type-specific method, but that one
/// calls this general method in parent class.  This method checks if
/// the healthcheck can be run now.  Sub-class method should terminate,
/// if this one forbids the check from being run.
int Healthcheck::schedule_healthcheck(struct timespec *now) {

  // Do not schedule the healthcheck twice.
  if (is_running)
    return false;

  // Check if host should be checked at this time.
  if (timespec_diff_ms(now, &last_checked) <
      check_interval * 1000 + extra_delay)
    return false;

  memcpy(&last_checked, now, sizeof(struct timespec));
  is_running = true;

  if (verbose > 1)
    log(MessageType::MSG_INFO, this, "scheduling");

  return true;
}

/// Finalize a healthcheck
///
/// This metod allows the check to have some final thoughts on its result.
void Healthcheck::finalize() {
  // Nothing to do here, it is used only for some types of healthchecks.
}

// End a health check
//
// It is the last function that has to be called at the end of
// the health check.  If it wouldn't be called, the process is not
// going to continue.
void Healthcheck::end_check(HealthcheckResult result, string message) {
  MessageType log_type;
  string statemsg;

  switch (result) {
  case HealthcheckResult::HC_PASS:
    log_type = MessageType::MSG_STATE_UP;
    this->last_state = HealthcheckState::STATE_UP;
    statemsg = fmt::sprintf("state: up message: %s", message);
    this->handle_result(statemsg);
    break;

  case HealthcheckResult::HC_FAIL:
    log_type = MessageType::MSG_STATE_DOWN;
    this->last_state = HealthcheckState::STATE_DOWN;
    statemsg = fmt::sprintf("state: down message: %s", message);
    this->handle_result(statemsg);
    break;

  case HealthcheckResult::HC_DRAIN:
    log_type = MessageType::MSG_STATE_DOWN;
    this->last_state = HealthcheckState::STATE_DRAIN;
    statemsg = fmt::sprintf("state: down with draining, message: %s", message);
    this->handle_result(statemsg);
    break;

  case HealthcheckResult::HC_PANIC:
    log_type = MessageType::MSG_CRIT;
    statemsg = fmt::sprintf("state: failure message: %s", message);
    log(log_type, this, statemsg);
    exit(2);
    break;
  }

  this->parent_lbnode->node_logic();
}

/// Handle healthcheck's result once it's finished.
///
/// This method handles the change betwen UP and DOWN hard_state.
/// It performs no pf actions, this is to be done via lb_node or lb_pool!
void Healthcheck::handle_result(string message) {
  string fail_message;
  bool changed = false;
  MessageType log_level = MessageType::MSG_INFO;

  // If a healtcheck has passed, zero the failure counter.
  if (last_state == HealthcheckState::STATE_UP)
    failure_counter = 0;

  if (hard_state == HealthcheckState::STATE_UP) {
    switch (last_state) {
    case HealthcheckState::STATE_UP:
      // No change, make compiler happy.
      break;
    case HealthcheckState::STATE_DOWN:
      // Change from UP to DOWN. The healthcheck has failed.
      changed = true;
      log_level = MessageType::MSG_STATE_DOWN;
      failure_counter++;
      fail_message =
          fmt::sprintf("failure: %d of %d", failure_counter, max_failed_checks);
      // Mark the hard DOWN state only after the number of failed checks is
      // reached.
      if (failure_counter >= max_failed_checks) {
        hard_state = HealthcheckState::STATE_DOWN;
      }
      break;
    case HealthcheckState::STATE_DRAIN:
      // Change from UP to DRAIN. The healthcheck has failed with draining.
      changed = true;
      log_level = MessageType::MSG_STATE_DOWN;
      // Make it fail immediately not waiting for max_failed.
      hard_state = HealthcheckState::STATE_DRAIN;
      break;
    }
  } else if (last_state == HealthcheckState::STATE_UP) {
    // Change from any state to UP. The healthcheck has passed again.
    hard_state = HealthcheckState::STATE_UP;
    failure_counter = 0;
    changed = true;
    log_level = MessageType::MSG_STATE_UP;
  }

  if (changed || verbose) {
    log(log_level, this,
        fmt::sprintf("protocol: %s %s %s", af_string, message, fail_message));
  }

  // Mark the check as not running, so it can be scheduled again.
  is_running = false;
  ran = true;
}

int Healthcheck::timeout_to_ms() { return timeval_to_ms(&this->timeout); }
