//
// Testtool - Load Balancing Pool
//
// Copyright (c) 2018 InnoGames GmbH
//

#define FMT_HEADER_ONLY

#include <fmt/format.h>
#include <fmt/printf.h>
#include <nlohmann/json.hpp>
#include <string.h>

#include "config.h"
#include "lb_node.h"
#include "lb_pool.h"
#include "msg.h"
#include "pfctl.h"
#include "pfctl_worker.h"

using namespace std;

// Linked from testtool.cpp
extern message_queue *pfctl_mq;

FaultPolicy fault_policy_from_string(string s) {
  if (s == "force_down")
    return FaultPolicy::FORCE_DOWN;
  if (s == "force_up")
    return FaultPolicy::FORCE_UP;
  if (s == "backup_pool")
    return FaultPolicy::BACKUP_POOL;

  log(MessageType::MSG_CRIT,
      "Unknown min_nodes_action " + s + ", falling back to force_down!");

  return FaultPolicy::FORCE_DOWN;
}

/// Constructor of LB Pool class
///
/// The constructor has not much work to do, init some variables
/// and display the LbPool name if verbose.
LbPool::LbPool(string name, nlohmann::json &config,
               map<std::string, LbPool *> *all_lb_pools) {

  this->all_lb_pools = all_lb_pools;
  this->name = name;

  // Read parameters from json, read them safe and then verify if they make
  // sense.
  this->ipv4_address = safe_get<string>(config, "ip4", "");
  this->ipv6_address = safe_get<string>(config, "ip6", "");
  this->pf_name = safe_get<string>(config, "pf_name", "");
  this->route_network = safe_get<string>(config, "route_network", "");
  this->min_nodes = safe_get<int>(config, "min_nodes", 0);
  this->max_nodes = safe_get<int>(config, "max_nodes", 0);
  this->fault_policy = fault_policy_from_string(
      safe_get<string>(config, "min_nodes_action", "force_down"));
  this->backup_pool_name = safe_get<string>(config, "backup_pool", "");
  if (this->backup_pool_name != "") {
    this->fault_policy = FaultPolicy::BACKUP_POOL;
  }

  // Perform some checks to verify if this is really an LB Pool and not
  // something else like a SNAT rule.
  if (this->ipv4_address.empty() && this->ipv6_address.empty())
    throw(NotLbPoolException("No IP address of any family specified!"));

  if (this->pf_name.empty())
    throw(NotLbPoolException("No pf_name configured!"));

  auto protocol_port = config.find("protocol_port");
  if (protocol_port == config.end() || protocol_port->empty())
    throw(NotLbPoolException("No protocol_port configured!"));

  this->state = LbPoolState::STATE_DOWN;

  // If this Pool has no healthchecks then force nodes to be always up.
  // This is required to have testool manage all LB Pools.
  auto health_checks = config.find("health_checks");
  if (health_checks == config.end() || health_checks->empty()) {
    this->fault_policy = FaultPolicy::FORCE_UP;
    this->min_nodes = config["nodes"].size();
  }

  // Ensure that min_ and max_nodes make sense. But only if max_nodes
  // was given. Value of 0 means that the feature is disabled.
  if (max_nodes > 0 && max_nodes < min_nodes) {
    max_nodes = min_nodes;
  }

  log(MessageType::MSG_INFO, this,
      fmt::sprintf("min_nodes: %d max_nodes: %d policy: %s state: created",
                   min_nodes, max_nodes, this->get_fault_policy_string()));

  // Glue things together. Please note that children append themselves
  // to property of parent in their own code.
  int node_index = 0;
  for (const auto &lbnode_it : config["nodes"].items()) {
    // Silently drop node if there is more than MAX_NODES.
    // Only this many nodes can be sent to pfctl worker.
    // We have no way of notifying Serveradmin about push failure
    // so silently dropping is good enough for now.
    // TODO: inform Serveradmin and fail pushing.
    if (node_index < MAX_NODES) {
      new LbNode(lbnode_it.key(), lbnode_it.value(), this);
      node_index++;
    }
  }

  // Healthchecks are defined per LB Pool but in fact must be attached
  // to each LB Node. Hence they are created here, in LB Pool constructor.
  for (auto hc_it : config["health_checks"]) {
    for (auto node : this->nodes) {
      if (!node->ipv4_address.empty())
        Healthcheck::healthcheck_factory(hc_it, node, &node->ipv4_address);
      if (!node->ipv6_address.empty())
        Healthcheck::healthcheck_factory(hc_it, node, &node->ipv6_address);
    }
  }

  // State of nodes loaded from pf must be verified. Maybe it contains
  // entries which are not in config file anymore? Maybe it has no checks
  // assigned?
  pf_synced = false;
  pool_logic(NULL);
}

string LbPool::get_fault_policy_string() {
  return FaultPolicyNames[static_cast<int>(this->fault_policy)];
}

/// Gos over all healthchecks and schedules them to run.
void LbPool::schedule_healthchecks(struct timespec *now) {
  for (auto node : this->nodes) {
    node->schedule_healthchecks(now);
  }
}

size_t LbPool::count_up_nodes() { return up_nodes.size(); }

string LbPool::get_backup_pool_state() {
  if (backup_pool_name == "")
    return "none";
  if (backup_pool_active)
    return "active";
  return "configured";
}

// Check state of all nodes in this pool
void LbPool::finalize_healthchecks() {

  // Go over all nodes in this pool, each node will gather state
  // from its healthchecks and update its own state.
  for (auto node : this->nodes)
    node->finalize_healthchecks();
}

/// Performs operations on LB Pool when LB Nodes' and their checks' statuses
/// change
void LbPool::pool_logic(LbNode *last_node) {
  // Now that checks results are known, operations can be performed on pf.
  // I tried to come multiple times with a differential algorithm.
  // I always failed because iteration was necessary in some cases anyway:
  // - To restore nodes after min_nodes with force_down is satisfied.
  // - To fill in a pool without healthchecks.
  // - To restore pool after switch from backup_pool.
  set<LbNode *> wanted_nodes;
  set<LbNode *> force_up_candidates;

  // The algorithm for calculating wanted nodes is quite big.
  // Don't run it unless we are called from a node with changed state
  // or right at start of testtool.
  if (last_node == NULL || last_node->state_changed) {
    // Try running without backup pool. Enable it only if not enough up
    // nodes are found.
    backup_pool_active = false;

    // Add nodes while satisfying max_nodes if it is set.

    // First add nodes which were added on previous change in order to avoid
    // rebalancing.
    for (LbNode *node : nodes) {
      if (!(max_nodes == 0 || wanted_nodes.size() < max_nodes))
        break; // Above max_nodes limit.
      if (!node->is_up())
        continue; // Don't add down nodes.
      if (!node->max_nodes_kept)
        continue; // Nodes which did not violate max_nodes before.
      log(MessageType::MSG_INFO, this,
          fmt::sprintf("Adding previously wanted up LB Node %s", node->name));
      wanted_nodes.insert(node);
    }

    // Then add other active nodes
    for (LbNode *node : nodes) {
      if (!(max_nodes == 0 || wanted_nodes.size() < max_nodes))
        break; // Above max_nodes limit.
      if (!node->is_up())
        continue; // Don't add down nodes.
      if (node->max_nodes_kept)
        continue; // Nodes which did violate max_nodes before.
      log(MessageType::MSG_INFO, this,
          fmt::sprintf("Adding any up LB Node %s", node->name));
      wanted_nodes.insert(node);
      node->max_nodes_kept = true;
    }

    // Now satisfy minNodes depending on its configuration
    if (min_nodes > 0 && wanted_nodes.size() < min_nodes) {
      switch (fault_policy) {
      case FaultPolicy::FORCE_DOWN:
        // If there is not enough nodes, bring the whole pool down.
        wanted_nodes.clear();
        log(MessageType::MSG_INFO, this,
            fmt::sprintf("Not enough nodes, forcing pool down"));
        break;

      case FaultPolicy::FORCE_UP:
        // Force some nodes up to satisfy min_nodes requirement. Only
        // non-downtimed nodes can be added.

        // First add the one which changed state recently.
        // If it was up, it is already added.
        if (last_node && last_node->state == LbNodeState::STATE_DOWN &&
            last_node->admin_state == LbNodeAdminState::STATE_ENABLED &&
            wanted_nodes.size() < min_nodes) {
          wanted_nodes.insert(last_node);
          last_node->min_nodes_kept = true;
          log(MessageType::MSG_INFO, this,
              fmt::sprintf("Force keeping recently changed LB Node %s",
                           last_node->name));
        }

        // Prepare a set of candidates to force up.
        for (LbNode *node : nodes) {
          if (last_node && node == last_node)
            continue; // Don't add recently changed LB Node.
          if (node->admin_state < LbNodeAdminState::STATE_ENABLED)
            continue; // Don't add downtimed LB Nodes.
          force_up_candidates.insert(node);
        }

        // Not enough nodes? Add those which were force-kept previously.
        for (LbNode *fuc : force_up_candidates) {
          if (wanted_nodes.size() >= min_nodes)
            break; // Enough nodes already wanted.
          if (!fuc->min_nodes_kept)
            continue; // Don't add nodes which were not added previously.
          log(MessageType::MSG_INFO, this,
              fmt::sprintf("Force keeping previously force-kept LB Node %s",
                           fuc->name));
          fuc->min_nodes_kept = true;
          wanted_nodes.insert(fuc);
        }

        // Still not enough nodes? Add any not-downtimed node.
        for (LbNode *fuc : force_up_candidates) {
          if (wanted_nodes.size() >= min_nodes)
            break; // Enough nodes already wanted.
          log(MessageType::MSG_INFO, this,
              fmt::sprintf("Force keeping any LB Node %s", fuc->name));
          fuc->min_nodes_kept = true;
          wanted_nodes.insert(fuc);
        }
        break;

      case FaultPolicy::BACKUP_POOL:
        if (all_lb_pools->find(backup_pool_name) != all_lb_pools->end()) {
          wanted_nodes = all_lb_pools->find(backup_pool_name)->second->up_nodes;
          backup_pool_active = true;
        }
        break;
      }
    }

    // Log only if state has changed.
    if (wanted_nodes != up_nodes) {
      up_nodes = wanted_nodes;

      string up_nodes_str;
      for (auto node : up_nodes) {
        up_nodes_str += node->name + ", ";
      }

      log(MessageType::MSG_INFO, this,
          fmt::sprintf("up_lbnodes: %s", up_nodes_str));

      // Pool state will be used for configuring BGP
      if (up_nodes.empty()) {
        state = LbPoolState::STATE_DOWN;
      } else {
        state = LbPoolState::STATE_UP;
      }

      pf_synced = false;
    }
  }

  // Always try to update pfctl. It might have failed previously.
  this->update_pfctl();
}

// Update pfctl to last known wanted_nodes if necessary.
void LbPool::update_pfctl(void) {
  // Update primary LB Pool
  if (!pf_synced) {
    pf_synced = send_message(pfctl_mq, name, pf_name, nodes, up_nodes);
    if (!pf_synced)
      log(MessageType::MSG_INFO, this, fmt::sprintf("sync: delayed"));
    else
      log(MessageType::MSG_INFO, this, fmt::sprintf("sync: immediate"));
  }

  // Update any other LB Pools which use this one as Backup Pool
  for (auto &lb_pool : *all_lb_pools) {
    if (lb_pool.second->backup_pool_name == name &&
        lb_pool.second->backup_pool_active) {
      lb_pool.second->pool_logic(NULL);
    }
  }
}

string LbPool::get_state_string() {
  return LbPoolStateNames[static_cast<int>(state)];
}

set<string> LbPool::get_up_nodes_names() {
  set<string> ret;
  for (LbNode *node : up_nodes) {
    ret.insert(node->name);
  }
  return ret;
}

set<LbNode *> LbPool::get_up_nodes() { return up_nodes; }
