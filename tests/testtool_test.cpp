//
// Tests for testtool
//

#include <fstream>
#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <string>
#include <cstring>

#include "healthcheck_dummy.h"
#include "lb_node.h"
#include "msg.h"
#include "pfctl.h"
#include "pfctl_async.h"
#include "testtool_test.h"

using namespace std;

// Check if an IP address is in the given table. Global variable used for
// faking state input for tests.
bool _pf_is_in_table = false;
bool pf_is_in_table(string *table, string *address, bool *answer) {
  (void)(table);
  (void)(address);
  *answer = _pf_is_in_table;
  return true;
}

void log(MessageType loglevel, string msg) {
  cerr << msg << endl;
}

void log(MessageType loglevel, LbPool *lbpool, string msg) {
  string out = "lbpool: " + lbpool->name + " " + msg;
  log(loglevel, out);
}

void log(MessageType loglevel, LbNode *lbnode, string msg) {
  string out = "lbnode: " + lbnode->name + " " + msg;
  log(loglevel, lbnode->parent_lbpool, out);
}

void log(MessageType loglevel, Healthcheck *hc, string msg) {
  string out = "healthcheck: " + hc->type + " " + hc->log_prefix + " " + msg;
  log(loglevel, hc->parent_lbnode, out);
}

set<string> sent_up_lb_nodes;

// Mock pf_sync_table_async for pool logic tests
void pf_sync_table_async(PfctlAsync *pfctl_async, std::string table,
                          SyncedLbNode *synced_lb_nodes,
                          PfSyncDoneCallback done_callback) {
  (void)(pfctl_async);
  (void)(table);

  sent_up_lb_nodes.clear();
  for (int i = 0; i < MAX_NODES; i++) {
    for (int proto = 0; proto < 2; proto++) {
      if (strlen(synced_lb_nodes[i].ip_address[proto]) == 0)
        continue;
      if (synced_lb_nodes[i].wanted_state == LbNodeState::STATE_UP &&
          synced_lb_nodes[i].admin_state == LbNodeAdminState::STATE_ENABLED) {
        sent_up_lb_nodes.insert(
            std::string(synced_lb_nodes[i].ip_address[proto]));
      }
    }
  }

  done_callback(true);
}

void TesttoolTest::SetUp() {
  string path = string(CMAKE_SOURCE_DIR) + "/tests/lb_pool_test.json";
  ifstream config_file(path);
  config_file >> base_config;
  config_file.close();
}

void TesttoolTest::SetUp(bool init_state) {
  _pf_is_in_table = init_state;
  for (const auto &lb_pool : base_config.items()) {
    string name = lb_pool.key();
    LbPool *new_lbpool = NULL;
    new_lbpool = new LbPool(name, lb_pool.value(), &lb_pools);
    lb_pools[new_lbpool->name] = new_lbpool;
  }
  // Run dummy HCs for each created LB Node
  for (const auto &lb_pool : lb_pools) {
    for (LbNode *node : lb_pool.second->nodes) {
      EndDummyHC(lb_pool.first, node->name,
                 init_state ? HealthcheckResult::HC_PASS
                            : HealthcheckResult::HC_FAIL,
                 true);
    }
  }
}

void TesttoolTest::TearDown() {
  lb_pools.clear();
  up_nodes_test.clear();
}

void TesttoolTest::EndDummyHC(string lb_pool_name, string lb_node_name,
                              HealthcheckResult result, bool all_hcs) {
  Healthcheck_dummy *hcd = NULL;

  LbNode *lbn = GetLbNode(lb_pool_name, lb_node_name);

  string message;
  switch (result) {
  case HealthcheckResult::HC_PASS:
    message = "dummy_pass";
    break;
  case HealthcheckResult::HC_FAIL:
    message = "dummy_fail";
    break;
  case HealthcheckResult::HC_DRAIN:
    message = "dummy_drain";
    break;
  case HealthcheckResult::HC_PANIC:
    message = "dummy_panic";
    break;
  }

  try {
    for (unsigned int i = 0; i < (all_hcs ? lbn->healthchecks.size() : 1);
         i++) {
      hcd = (Healthcheck_dummy *)(lbn->healthchecks.at(i));
      hcd->dummy_end_check(result, message);
    }
  } catch (out_of_range) {
    throw LbPoolTestException("Could't find HC for LB Pool " + lb_pool_name +
                              " LB Node " + lb_node_name);
  }
}

LbNode *TesttoolTest::GetLbNode(string lb_pool_name, string lb_node_name) {
  LbNode *ret = NULL;
  for (LbNode *node : lb_pools[lb_pool_name]->nodes) {
    if (node->name == lb_node_name) {
      ret = node;
      break;
    }
  }
  if (ret == NULL)
    throw LbPoolTestException("Could't find LB Pool " + lb_pool_name +
                              " LB Node " + lb_node_name);

  return ret;
}

LbNodeState TesttoolTest::GetLbNodeState(string lb_pool_name,
                                         string lb_node_name) {
  return GetLbNode(lb_pool_name, lb_node_name)->state;
}

set<string> TesttoolTest::UpNodesNames() {
  return lb_pools[test_lb_pool]->get_up_nodes_names();
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
