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

LbNodeState state_from_config(string s) {
  if (s == "online")
    return LbNodeState::STATE_UP;
  if (s == "deploy_online")
    return LbNodeState::STATE_UP;
  if (s == "deploy_offline")
    return LbNodeState::STATE_DRAIN;
  if (s == "maintenance")
    return LbNodeState::STATE_DOWNTIME;
  if (s == "retired")
    return LbNodeState::STATE_DOWNTIME;
  return LbNodeState::STATE_UP;
}

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

  // State starts as defined in config.
  state = state_from_config(safe_get<string>(config, "state", "online"));

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

  // Overwrite state if it's up in config but not in pf.
  if (!pf_state and state == LbNodeState::STATE_UP)
    this->state = LbNodeState::STATE_DOWN;

  this->checked = false;
  this->min_nodes_kept = false;
  this->max_nodes_kept = false;

  this->parent_lbpool->nodes.insert(this);

  log(MessageType::MSG_INFO, this,
      fmt::sprintf("state: created initial_state: %s",
                   this->state_to_string()));
}

string LbNode::state_to_string() {
  return LbNodeStateNames[static_cast<int>(this->state)];
}

bool LbNode::is_up() {
  // Ignore downtimes, report if LbNode is fully up.
  return (this->state == LbNodeState::STATE_UP);
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
  unsigned int drain_healthchecks = 0;

  // Go over all healthchecks for this node and count hard STATE_UP
  // healthchecks.
  for (auto &hc : healthchecks) {
    if (hc->ran)
      checked = true;
    if (hc->hard_state == HealthcheckState::STATE_UP)
      ok_healthchecks++;
    if (hc->hard_state == HealthcheckState::STATE_DRAIN)
      drain_healthchecks++;
  }

  // Do not check all healthchecks and don't notify pool until all check
  // report at least once
  if (!checked)
    return;

  // Fill in node state basing on passed healthchecks. Display information.
  // Log and update pool if state changed. There is no need to check for
  // downtimes.
  state_changed = false;

  LbNodeState new_state = LbNodeState::STATE_DOWN;

  if (drain_healthchecks)
    new_state = LbNodeState::STATE_DRAIN;
  else if (ok_healthchecks == num_healthchecks)
    new_state = LbNodeState::STATE_UP;

  if (state == LbNodeState::STATE_UP) {
    switch (new_state) {
    case LbNodeState::STATE_DOWN:
      // Change from UP to DOWN
      state_changed = true;
      max_nodes_kept = false;
      state = LbNodeState::STATE_DOWN;
      log(MessageType::MSG_STATE_DOWN, this,
          fmt::sprintf("message: %d of %d checks failed",
                       num_healthchecks - ok_healthchecks, num_healthchecks));
      break;
    case LbNodeState::STATE_DRAIN:
      // Change from UP to DRAIN
      state_changed = true;
      max_nodes_kept = false;
      state = LbNodeState::STATE_DRAIN;
      log(MessageType::MSG_STATE_DOWN, this,
          fmt::sprintf("message: %d of %d checks draining", drain_healthchecks,
                       num_healthchecks));
      break;
    case LbNodeState::STATE_DOWNTIME:
      // Handled LbNode::change_downtime().
    case LbNodeState::STATE_UP:
      // No change.
      break;
    }
  } else if (new_state == LbNodeState::STATE_UP) {
    // Change from any state to UP
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

/// Starts or ends a downtime.
void LbNode::change_downtime(string s) {
  LbNodeState new_state = state_from_config(s);
  kill_states = !(new_state == LbNodeState::STATE_DRAIN);

  // Do nothing if there is no change
  if (new_state == state)
    return;

  if (new_state < LbNodeState::STATE_DOWN) {
    log(MessageType::MSG_STATE_DOWN, this,
        fmt::sprintf("downtime: starting, drain: %s",
                     kill_states ? "no" : "yes"));
    state = new_state;
    max_nodes_kept = false;
    // Enable pool logic. It will consider a downtimed node down and remove it.
    this->state_changed = true;
  }

  // Ignore online state coming from loaded config if currently not in
  // downtime.
  if (state >= LbNodeState::STATE_DOWN)
    return;

  if (new_state >= LbNodeState::STATE_DOWN) {
    // Pretend the node is down. It will be re-enabled
    // after the next healthcheck run.
    state = LbNodeState::STATE_DOWN;
    log(MessageType::MSG_STATE_UP, this, "downtime: ending");
    // Enable pool logic. If min_nodes is to be fulfilled, we can't depend only
    // on next check to finish.
    this->state_changed = true;
  }

  // Call pool logic only once so that changes of downtimes to multiple LB Nodes
  // are processed as a single change.
  parent_lbpool->pool_logic(this);
}
