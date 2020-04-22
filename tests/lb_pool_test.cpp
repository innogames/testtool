//
// Tests for LbPool->pool_logic()
//

#include <boost/exception/diagnostic_information.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <fstream>
#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include "cmake.h"
#include "healthcheck_dummy.h"
#include "lb_pool.h"

using namespace std;
using namespace boost::interprocess;

using json = nlohmann::json;

// Global variables required by some classes' methods.
struct event_base *eventBase = NULL;
SSL_CTX *sctx = NULL;
int verbose = 0;
boost::interprocess::message_queue *pfctl_mq;

extern bool _pf_is_in_table;

string test_lb_pool = "lbpool.example.com";

namespace {
class LbPoolTest : public ::testing::Test {

protected:
  std::map<std::string, LbPool *> lb_pools;
  json base_config;

  virtual void SetUp() {
    std::string path = CMAKE_SOURCE_DIR + "/tests/lb_pool_test.json";
    std::ifstream config_file(path);
    config_file >> base_config;
    config_file.close();
  }

  virtual void SetUp(bool init_state) {
    _pf_is_in_table = init_state;
    for (const auto &lb_pool : base_config.items()) {
      string name = lb_pool.key();
      LbPool *new_lbpool = NULL;
      new_lbpool = new LbPool(name, lb_pool.value(), &lb_pools);
      lb_pools[new_lbpool->name] = new_lbpool;
    }
  }

  virtual void TearDown() { lb_pools.clear(); }

  void EndDummyHC(string lb_pool_name, string lb_node_name,
                  HealthcheckResult result) {
    Healthcheck_dummy *d = NULL;
    for (LbNode *node : lb_pools[lb_pool_name]->nodes) {
      if (node->name == lb_node_name) {
        d = (Healthcheck_dummy *)(node->healthchecks.at(0));
        break;
      }
    }

    if (d == NULL)
      return;

    string message;
    switch (result) {
    case HealthcheckResult::HC_FAIL:
      message = "dummy_fail";
      break;
    case HealthcheckResult::HC_PASS:
      message = "dummy_pass";
      break;
    case HealthcheckResult::HC_PANIC:
      message = "dummy_panic";
      break;
    }
    d->dummy_end_check(result, message);
  }

  int GetLbNodeState(string lb_pool_name, string lb_node_name) {
    for (LbNode *node : lb_pools[lb_pool_name]->nodes) {
      if (node->name == lb_node_name) {
        return node->state;
      }
    }
    return -1;
  }

  LbNode *GetLbNode(string lb_pool_name, string lb_node_name) {
    for (LbNode *node : lb_pools[lb_pool_name]->nodes) {
      if (node->name == lb_node_name) {
        return node;
      }
    }
    return NULL;
  }
};

} // namespace

TEST_F(LbPoolTest, InitDown) {
  SetUp(false);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode1.local"), STATE_DOWN);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode2.local"), STATE_DOWN);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode2.local"), STATE_DOWN);
}

TEST_F(LbPoolTest, InitUp) {
  SetUp(true);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode1.local"), STATE_UP);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode2.local"), STATE_UP);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode2.local"), STATE_UP);
}

TEST_F(LbPoolTest, NodeGoesUp) {
  SetUp(false);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode1.local"), STATE_DOWN);
  EndDummyHC(test_lb_pool, "lbnode1.local", HC_PASS);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode1.local"), STATE_UP);
}

TEST_F(LbPoolTest, NodeGoesDown1) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 1;
  SetUp(true);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode1.local"), STATE_UP);
  EndDummyHC(test_lb_pool, "lbnode1.local", HC_FAIL);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode1.local"), STATE_DOWN);
}

TEST_F(LbPoolTest, NodeGoesDown3) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 3;
  SetUp(true);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode1.local"), STATE_UP);

  EndDummyHC(test_lb_pool, "lbnode1.local", HC_FAIL);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode1.local"), STATE_UP);

  EndDummyHC(test_lb_pool, "lbnode1.local", HC_FAIL);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode1.local"), STATE_UP);

  EndDummyHC(test_lb_pool, "lbnode1.local", HC_FAIL);
  EXPECT_EQ(GetLbNodeState(test_lb_pool, "lbnode1.local"), STATE_DOWN);
}

TEST_F(LbPoolTest, MinNodes1ForceUp) {
  set<LbNode *> up_nodes_test;

  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 1;
  base_config["lbpool.example.com"]["min_nodes"] = 1;
  base_config["lbpool.example.com"]["min_nodes_action"] = "force_up";
  SetUp(false);

  // Testtool should pick one random node on startup with all nodes failed.
  EXPECT_EQ(lb_pools[test_lb_pool]->up_nodes.size(), 1);

  // Have one node pass tests.
  EndDummyHC(test_lb_pool, "lbnode1.local", HC_PASS);
  up_nodes_test.clear();
  up_nodes_test.insert(GetLbNode(test_lb_pool, "lbnode1.local"));
  EXPECT_EQ(up_nodes_test, lb_pools[test_lb_pool]->up_nodes);

  // Make the same node down.
  EndDummyHC(test_lb_pool, "lbnode1.local", HC_FAIL);
  EXPECT_EQ(up_nodes_test, lb_pools[test_lb_pool]->up_nodes);

  // Have a different node pass tests, testtool should switch to it.
  EndDummyHC(test_lb_pool, "lbnode2.local", HC_PASS);
  up_nodes_test.clear();
  up_nodes_test.insert(GetLbNode(test_lb_pool, "lbnode2.local"));
  EXPECT_EQ(up_nodes_test, lb_pools[test_lb_pool]->up_nodes);

  // Make the same node down, it should be kept.
  EndDummyHC(test_lb_pool, "lbnode2.local", HC_FAIL);
  EXPECT_EQ(up_nodes_test, lb_pools[test_lb_pool]->up_nodes);

  // Fail all nodes
  // for (LbNode *node : lb_pools[test_lb_pool]->nodes) {
  //  EndDummyHC(test_lb_pool, node->name, HC_FAIL);
  // }

  //
  // up_nodes_test.insert(GetLbNode(test_lb_pool, "lbnode1.local"));
  // up_nodes_test.insert(GetLbNode(test_lb_pool, "lbnode2.local"));
  // up_nodes_test.insert(GetLbNode(test_lb_pool, "lbnode3.local"));
  // EXPECT_EQ(up_nodes_test, lb_pools[test_lb_pool]->up_nodes);
}
