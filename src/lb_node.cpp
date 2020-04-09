//
// Testtool - Load Balancing Node
//
// Copyright (c) 2018 InnoGames GmbH
//

#include <fmt/format.h>
#include <fmt/printf.h>
#include <syslog.h>

#include "config.h"
#include "healthcheck.h"
#include "lb_node.h"
#include "lb_pool.h"
#include "msg.h"
#include "pfctl.h"

using namespace std;

// Linked from testtool.cpp
extern bool pf_action;
extern int verbose;

/// LbNode class constructor.
///
/// Link the node and its parent pool, initialize some variables, print
/// diagnostic information if necessary.
LbNode::LbNode(string name, const nlohmann::json &config,
               class LbPool *parent_lbpool) {
  this->name = name;

  this->route_network = safe_get<string>(config, "route_network", "");

  this->ipv4_address = safe_get<string>(config, "ip4", "");
  this->ipv6_address = safe_get<string>(config, "ip6", "");

  this->parent_lbpool = parent_lbpool;

  this->admin_state = LbNodeState::STATE_UP;

  // Read initial state of host from pf.
  bool pf_state = false;

  // Both protocols IP addresses should be always present or absent
  // for each LB Node so we trust first IPv4 and then IPv6.
  if (!this->ipv4_address.empty())
    pf_is_in_table(&this->parent_lbpool->pf_name, &this->ipv4_address,
                   &pf_state);
  else
    pf_is_in_table(&this->parent_lbpool->pf_name, &this->ipv6_address,
                   &pf_state);
  if (pf_state)
    this->state = LbNodeState::STATE_UP;
  else
    this->state = LbNodeState::STATE_DOWN;
  this->checked = false;
  this->min_nodes_kept = false;
  this->max_nodes_kept = false;

  this->parent_lbpool->nodes.push_back(this);

  if (safe_get<bool>(config, "downtime", false)) {
    this->admin_state = LbNodeState::STATE_DOWN;
  }

  log(MessageType::MSG_INFO, this,
      fmt::sprintf("state: created initial_state: %s",
                   this->state_to_string()));
}

string LbNode::state_to_string() {
  if (this->admin_state == LbNodeState::STATE_DOWN)
    return "DOWNTIME";
  if (this->state == LbNodeState::STATE_UP)
    return "UP";
  return "DOWN";
}

LbNodeState LbNode::get_state() {
  if (admin_state == LbNodeState::STATE_DOWN) {
    return LbNodeState::STATE_DOWN;
  } else {
    return state;
  }
}

/// Schedules all LB Node's Healthchecks to run
///
/// Try to schedule all healthcheck of this node. Do not try if there is a
/// downtime for this node.
void LbNode::schedule_healthchecks(struct timespec *now) {
  for (unsigned int hc = 0; hc < healthchecks.size(); hc++) {
    healthchecks[hc]->schedule_healthcheck(now);
  }
}

/// Finalizes all LB Node's Healthchecks.
///
/// Some types of Healthchecks might require additional work to fully finish
/// their job. This is particulary true for ping test which requires manual
/// intervetion to check for timeouts.
void LbNode::finalize_healthchecks() {
  for (auto &hc : healthchecks) {
    hc->finalize();
  }
}

/// Checks results of all healthchecks for this node and act accordingly:
///
/// - set hard_state
/// - display messages
/// - notify pool about state changes
void LbNode::node_logic() {
  unsigned int num_healthchecks = healthchecks.size();
  unsigned int ok_healthchecks = 0;

  // Go over all healthchecks for this node and count hard STATE_UP
  // healthchecks.
  for (auto &hc : healthchecks) {
    if (hc->ran)
      checked = true;
    if (hc->hard_state == HealthcheckState::STATE_UP)
      ok_healthchecks++;
  }

  // Do not check all healthchecks and don't notify pool until all check
  // report at least once
  if (!checked)
    return;

  // Fill in node state basing on passed healthchecks. Display information.
  // Log and update pool if state changed. There is no need to check for
  // downtimes.
  state_changed = false;
  LbNodeState new_state = (ok_healthchecks < num_healthchecks)
                              ? LbNodeState::STATE_DOWN
                              : LbNodeState::STATE_UP;
  if (state == LbNodeState::STATE_UP && new_state == LbNodeState::STATE_DOWN) {
    state_changed = true;
    max_nodes_kept = false;
    state = LbNodeState::STATE_DOWN;
    log(MessageType::MSG_STATE_DOWN, this,
        fmt::sprintf("message: %d of %d checks failed",
                     num_healthchecks - ok_healthchecks, num_healthchecks));
  } else if (state == LbNodeState::STATE_DOWN &&
             new_state == LbNodeState::STATE_UP) {
    state_changed = true;
    min_nodes_kept = false;
    state = LbNodeState::STATE_UP;
    log(MessageType::MSG_STATE_DOWN, this,
        fmt::sprintf("message: all of %d checks passed", num_healthchecks));
  }

  // Notify parent pool. Do it always, no matter if there was change or not.
  // The pool might not be synced if pfctl was busy.
  parent_lbpool->pool_logic(this);
}

/// Starts a downtime.
void LbNode::start_downtime() {
  // Start downtime only once.
  if (admin_state == LbNodeState::STATE_DOWN)
    return;

  log(MessageType::MSG_STATE_DOWN, this, "downtime: starting");

  admin_state = LbNodeState::STATE_DOWN;
  this->state_changed = true;
  max_nodes_kept = false;
  // Call pool logic. It will detect a down node and remove it.
  parent_lbpool->pool_logic(this);
}

/// Ends a downtime.
void LbNode::end_downtime() {
  // Remove downtime only once.
  if (admin_state == LbNodeState::STATE_UP)
    return;

  log(MessageType::MSG_STATE_UP, this, "downtime: ending");

  admin_state = LbNodeState::STATE_UP;
  this->state_changed = true;
  parent_lbpool->pool_logic(this);
}
