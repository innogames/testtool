//
// Testtool - Load Balancing Pool
//
// Copyright (c) 2018 InnoGames GmbH
//

#ifndef _LB_POOL_H_
#define _LB_POOL_H_

#include <exception>
#include <iostream>
#include <list>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "lb_node.h"

using namespace std;

class NotLbPoolException {
public:
  NotLbPoolException(const string &msg) : msg_(msg){};
  ~NotLbPoolException(){};
  const char *what() { return (msg_.c_str()); }

private:
  string msg_;
};

enum class LbPoolState { STATE_DOWN, STATE_UP };

// Fault policy infers the pool state from the set of node states.
enum class FaultPolicy {
  FORCE_DOWN,  // Pool fails if up_nodes < min_nodes.
  FORCE_UP,    // Last min_nodes nodes will be treated as online, even if
               // they seem down
  BACKUP_POOL, // Switch to backup pool.
};

static const char *FaultPolicyNames[] = {"force_down", "force_up",
                                         "backup_pool"};

FaultPolicy fault_policy_from_string(string s);

class LbPool {
  // Methods
public:
  LbPool(string name, nlohmann::json &config,
         map<std::string, LbPool *> *all_lb_pools);
  void schedule_healthchecks(struct timespec *now);
  void pool_logic(LbNode *last_node);
  void finalize_healthchecks();
  void update_pfctl();
  string get_state_text();
  size_t count_up_nodes();
  string get_backup_pool_state();
  string fault_policy_to_string();

  // Members
public:
  string name;
  string route_network;
  string ipv4_address;
  string ipv6_address;
  string pf_name;
  LbPoolState state;
  vector<class LbNode *> nodes;
  set<class LbNode *> up_nodes;

private:
  string backup_pool_name;
  bool backup_pool_active;
  size_t min_nodes; // Minimum number of UP hosts (inclusive) before the fault
                    // policy kicks in.
  size_t max_nodes; // Maximum number of UP hosts (inclusive) for security.
                    // 0 disables the check.
  FaultPolicy fault_policy;
  map<std::string, LbPool *> *all_lb_pools;
  bool pf_synced;
};

#endif
