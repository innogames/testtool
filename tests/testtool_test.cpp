//
// Tests for testtool
//

#include <boost/interprocess/ipc/message_queue.hpp>
#include <fstream>
#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <string>

#include "healthcheck_dummy.h"
#include "lb_node.h"
#include "msg.h"
#include "testtool_test.h"

using namespace std;
using namespace boost::interprocess;

// Check if an IP address is in the given table. Global variable used for
// faking state input for tests.
bool _pf_is_in_table = false;
bool pf_is_in_table(string *table, string *address, bool *answer) {
  *answer = _pf_is_in_table;
  return true;
}

void log(MessageType loglevel, string msg){};
void log(MessageType loglevel, LbPool *lbpool, string msg){};
void log(MessageType loglevel, LbNode *lbnode, string msg){};
void log(MessageType loglevel, Healthcheck *hc, string msg){};

set<string> sent_up_lb_nodes;
bool send_message(message_queue *mq, string pool_name, string table_name,
                  set<LbNode *> all_lb_nodes, set<LbNode *> up_lb_nodes) {

  for (LbNode *up_lb_node : up_lb_nodes)
    sent_up_lb_nodes.insert(up_lb_node->name);

  return true;
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
}

void TesttoolTest::TearDown() {
  lb_pools.clear();
  up_nodes_test.clear();
}

void TesttoolTest::EndDummyHC(string lb_pool_name, string lb_node_name,
                              HealthcheckResult result) {
  Healthcheck_dummy *hcd = NULL;

  LbNode *lbn = GetLbNode(lb_pool_name, lb_node_name);

  try {
    hcd = (Healthcheck_dummy *)(lbn->healthchecks.at(0));
  } catch (out_of_range) {
    throw LbPoolTestException("Could't find HC for LB Pool " + lb_pool_name +
                              " LB Node " + lb_node_name);
  }

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
  hcd->dummy_end_check(result, message);
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
