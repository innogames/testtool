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
#include "lb_node.h"
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

class LbPoolTestException : virtual public runtime_error {
public:
  explicit LbPoolTestException(const string &msg) : runtime_error(msg) {}
  virtual ~LbPoolTestException() throw() {}
};

namespace {
class LbPoolTest : public ::testing::Test {

protected:
  map<string, LbPool *> lb_pools;
  json base_config;
  set<LbNode *> up_nodes_test;

  virtual void SetUp() {
    string path = CMAKE_SOURCE_DIR + "/tests/lb_pool_test.json";
    ifstream config_file(path);
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

  virtual void TearDown() {
    lb_pools.clear();
    up_nodes_test.clear();
  }

  LbNode *GetLbNode(string lb_pool_name, string lb_node_name) {
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

  LbNodeState GetLbNodeState(string lb_pool_name, string lb_node_name) {
    return GetLbNode(lb_pool_name, lb_node_name)->state;
  }

  void EndDummyHC(string lb_pool_name, string lb_node_name,
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
    case HealthcheckResult::HC_PANIC:
      message = "dummy_panic";
      break;
    }
    hcd->dummy_end_check(result, message);
  }

  set<string> UpNodesNames() {
    return lb_pools[test_lb_pool]->get_up_nodes_names();
  }
};

} // namespace

TEST_F(LbPoolTest, InitDown) {
  SetUp(false);

  // On startup LB Nodes are read as down from pfctl.
  EXPECT_EQ(UpNodesNames(), set<string>({}));
}

TEST_F(LbPoolTest, InitUp) {
  SetUp(true);
  // On startup LB Nodes are read as up from pfctl.
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));
}

TEST_F(LbPoolTest, NodeGoesUp) {
  SetUp(false);

  EXPECT_EQ(UpNodesNames(), set<string>({}));

  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));
}

// Detect a single LB Node go down with hc_max_failed =1
//
TEST_F(LbPoolTest, NodeGoesDown1) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 1;
  SetUp(true);

  // Fail a LB Node just once.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));
}

// Detect a single LB Node go down with hc_max_failed > 1
//
TEST_F(LbPoolTest, NodeGoesDown3) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 3;
  SetUp(true);

  // Fail LB Node for the 1st time/
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));

  // Fail LB Node for the 2nd time/
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));

  SCOPED_TRACE("Fail LB Node for the 3rd time");
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));
}

// Test min_nodes initialization.
//
TEST_F(LbPoolTest, MinNodes1ForceUpInit) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 1;
  base_config["lbpool.example.com"]["min_nodes"] = 1;
  base_config["lbpool.example.com"]["min_nodes_action"] = "force_up";
  SetUp(false);

  // LB Pool will force up a random LB Node on startup with all    LB Nodes
  // failed.
  EXPECT_EQ(lb_pools[test_lb_pool]->get_up_nodes().size(), 1);
}

// Test min_nodes with force_down policy.
//
TEST_F(LbPoolTest, MinNodes1ForceDown) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 1;
  base_config["lbpool.example.com"]["min_nodes"] = 2;
  base_config["lbpool.example.com"]["min_nodes_action"] = "force_down";
  SetUp(false);

  // Set all LB Nodes up.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS);
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_PASS);
  EndDummyHC(test_lb_pool, "lbnode3", HealthcheckResult::HC_PASS);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));

  // Fail one LB Node.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));

  // Fail one more LB Node, the whole LB Pool will fail.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_FAIL);
  EXPECT_EQ(UpNodesNames(), set<string>());
}

// Test min_nodes with force_up policy with no up LB Nodes during test.
//
TEST_F(LbPoolTest, MinNodes1ForceUpAllDead) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 1;
  base_config["lbpool.example.com"]["min_nodes"] = 1;
  base_config["lbpool.example.com"]["min_nodes_action"] = "force_up";
  SetUp(false);

  // Have one LB Node pass tests.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // Make the same LB Node down.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // Have a different LB Node pass tests, LB Pool will switch to it.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_PASS);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2"}));

  // Make the same LB Node down, it will be kept.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_FAIL);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2"}));
}

// Test max_nodes behaviour.
//
TEST_F(LbPoolTest, MaxNodes1) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 1;
  base_config["lbpool.example.com"]["max_nodes"] = 1;
  SetUp(false);

  // Have one LB Node pass tests.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // Add one more LB Node, it will not change anything.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_PASS);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // Fail the 1st LB Node, it will switch to the 2nd.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2"}));

  // Pass the 3st LB Node, it will not change anything.
  EndDummyHC(test_lb_pool, "lbnode3", HealthcheckResult::HC_PASS);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2"}));

  // Fail the 2nd LB Node, it will switch to the 3nd.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_FAIL);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode3"}));
}

// Combined min_ and max_nodes behaviour used for PostgreSQL databases. There is
// only one writable master and we keep it active to reduce flapping.
//
TEST_F(LbPoolTest, MinNodes1MaxNodes1) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 1;
  base_config["lbpool.example.com"]["max_nodes"] = 1;
  base_config["lbpool.example.com"]["min_nodes"] = 1;
  base_config["lbpool.example.com"]["min_nodes_action"] = "force_up";
  SetUp(false);

  // Have one LB Node pass tests.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // Add one more, there will be no change.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_PASS);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // The 1st one dies, switch to the 2nd live one.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2"}));

  // The 2st one dies, no change.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_FAIL);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2"}));
}

// Test downtime functionality, it will override max_hc_failed and fail the
// LB Node immediately.
//
TEST_F(LbPoolTest, Downtime) {

  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 3;
  SetUp(true);

  // Downtime a LB Node.
  GetLbNode(test_lb_pool, "lbnode1")->change_downtime("maintenance");
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));

  // Finished HC changes nothing.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));

  // End downtime, LB Node will go up only once it passes a HC.
  GetLbNode(test_lb_pool, "lbnode1")->change_downtime("online");
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));
}
