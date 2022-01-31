//
// Testtool - Load Balancing Node
//
// Copyright (c) 2018 InnoGames GmbH
//

#define FMT_HEADER_ONLY

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

LbNodeAdminState admin_state_from_config(string s) {
  if (s == "online")
    return LbNodeAdminState::STATE_ENABLED;
  if (s == "deploy_online")
    return LbNodeAdminState::STATE_ENABLED;
  if (s == "deploy_offline")
    return LbNodeAdminState::STATE_DRAIN_HARD;
  if (s == "maintenance")
    return LbNodeAdminState::STATE_DOWNTIME;
  if (s == "retired")
    return LbNodeAdminState::STATE_DOWNTIME;
  return LbNodeAdminState::STATE_ENABLED;
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

  this->admin_state =
      admin_state_from_config(safe_get<string>(config, "state", "online"));

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

  this->parent_lbpool->nodes.insert(this);

  log(MessageType::MSG_INFO, this,
      fmt::sprintf("state: created state: %s admin_state: %s",
                   this->get_state_string(), this->get_admin_state_string()));
}

string LbNode::get_state_string() {
  return LbNodeStateNames[static_cast<int>(this->state)];
}

string LbNode::get_admin_state_string() {
  return LbNodeAdminStateNames[static_cast<int>(this->admin_state)];
}

// Determine if LB Node is up or down by joining State and AdminState.
//
bool LbNode::is_up() {
  if (this->admin_state < LbNodeAdminState::STATE_ENABLED)
    return false;
  return (this->state == LbNodeState::STATE_UP);
}

// Determine if LB Node is up or down by joining State and AdminState, return it
// as string.
//
string LbNode::is_up_string() { return this->is_up() ? "up" : "down"; }

/// Schedules all LB Node's Healthchecks to run
///
/// Try to schedule all healthcheck of this node. Do not try if there is a
/// downtime for this node.
void LbNode::schedule_healthchecks(struct timespec *now) {
  bool ret;
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

  LbNodeState new_state = (ok_healthchecks < num_healthchecks)
                              ? LbNodeState::STATE_DOWN
                              : LbNodeState::STATE_UP;

  LbNodeAdminState new_admin_state = (drain_healthchecks == 0)
                                         ? LbNodeAdminState::STATE_ENABLED
                                         : LbNodeAdminState::STATE_DRAIN_SOFT;

  if (state == LbNodeState::STATE_UP && new_state == LbNodeState::STATE_DOWN) {
    state_changed = true;
    max_nodes_kept = false;
    state = LbNodeState::STATE_DOWN;
    if (admin_state == LbNodeAdminState::STATE_ENABLED)
      admin_state = new_admin_state;
    log(MessageType::MSG_STATE_DOWN, this,
        fmt::sprintf("message: %d of %d checks failed",
                     num_healthchecks - ok_healthchecks, num_healthchecks));
  } else if (state == LbNodeState::STATE_DOWN &&
             new_state == LbNodeState::STATE_UP) {
    // Change from any state to UP
    state_changed = true;
    min_nodes_kept = false;
    state = LbNodeState::STATE_UP;
    if (admin_state == LbNodeAdminState::STATE_DRAIN_SOFT)
      admin_state = LbNodeAdminState::STATE_ENABLED;
    log(MessageType::MSG_STATE_DOWN, this,
        fmt::sprintf("message: all of %d checks passed", num_healthchecks));
  }

  // Notify parent pool. Do it always, no matter if there was change or not.
  // The pool might not be synced if pfctl was busy.
  parent_lbpool->pool_logic(this);
}

/// Starts or ends a downtime.
void LbNode::change_downtime(string s) {
  LbNodeAdminState new_admin_state = admin_state_from_config(s);

  // Do nothing if there is no change
  if (new_admin_state == admin_state)
    return;

  if (new_admin_state < LbNodeAdminState::STATE_ENABLED) {
    kill_states = !(new_admin_state <= LbNodeAdminState::STATE_DRAIN_SOFT);
    log(MessageType::MSG_STATE_DOWN, this,
        fmt::sprintf("downtime: starting, drain: %s",
                     kill_states ? "no" : "yes"));
    max_nodes_kept = false;
    // Enable pool logic. It will consider a downtimed node down and remove it.
    this->state_changed = true;
  } else {
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
  admin_state = new_admin_state;
  parent_lbpool->pool_logic(this);
}
