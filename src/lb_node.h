//
// Testtool - Load Balancing Node
//
// Copyright (c) 2018 InnoGames GmbH

#ifndef _LB_NODE_H_
#define _LB_NODE_H_

#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>

#include "healthcheck.h"

using namespace std;

enum class LbNodeState { STATE_DOWN, STATE_UP };

class LbNode {
  // Methods
public:
  LbNode(string name, const nlohmann::json &config,
         class LbPool *parent_lbpool);
  void schedule_healthchecks(struct timespec *now);
  void finalize_healthchecks();
  void node_logic();

  void start_downtime();
  void end_downtime();

  LbNodeState get_state(); // getter for private member
  string state_to_string();

  // Members
public:
  string name;
  string route_network;
  // libevent wants the address passed as char[] so stick to some string-like.
  string ipv4_address;
  string ipv6_address;
  class LbPool *parent_lbpool;
  LbNodeState state;
  LbNodeState admin_state; // Keeps information about downtimes.
  vector<class Healthcheck *> healthchecks;
  bool min_nodes_kept; // Node was kept to meet min_nodes requirement.
  bool max_nodes_kept; // Node was kept because it met max_nodes requirement.
  bool checked;        // This node has all of its checks ran at least once.
  bool state_changed;  // This node hs changed its state since last check.

private:
  bool backup;
};

#endif
