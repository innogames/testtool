//
// Tests for LbPool->pool_logic()
//

#include <boost/exception/diagnostic_information.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <fstream>
#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include "cmake_dirs.h"
#include "healthcheck_dummy.h"
#include "lb_node.h"
#include "lb_pool.h"
#include "testtool_test.h"

using namespace std;
using namespace boost::interprocess;

using json = nlohmann::json;

// Global variables required by some classes' methods.
struct event_base *eventBase = NULL;
SSL_CTX *sctx = NULL;
int verbose = 0;
boost::interprocess::message_queue *pfctl_mq;

extern bool _pf_is_in_table;
extern set<string> sent_up_lb_nodes;

class LbPoolTest : public TesttoolTest {};

TEST_F(LbPoolTest, InitDown) {
  // On startup LB Nodes are read as down from pfctl.
  SetUp(false);

  // And they are still down once testtool starts.
  EXPECT_EQ(UpNodesNames(), set<string>({}));
}

TEST_F(LbPoolTest, InitDownNoHCs) {
  // The LB Pool has no HCs.
  base_config["lbpool.example.com"].erase("health_checks");

  // On startup LB Nodes are read as down from pfctl.
  SetUp(false);

  // But they all become active since there are no HCs.
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));
}

TEST_F(LbPoolTest, InitUp) {
  SetUp(true);
  // On startup LB Nodes are read as up from pfctl.
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));
}

TEST_F(LbPoolTest, NodeGoesUp) {
  SetUp(false);

  EXPECT_EQ(UpNodesNames(), set<string>({}));

  // Just one HC is not enough to have the node go up.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, false);
  EXPECT_EQ(UpNodesNames(), set<string>({}));

  // All passed checks make the node go up.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));
}

// Detect a single LB Node go down with hc_max_failed =1
//
TEST_F(LbPoolTest, NodeGoesDown1) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 1;
  SetUp(true);

  // Fail a LB Node just once.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL, false);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));
}

// Detect a single LB Node go down with hc_max_failed > 1
//
TEST_F(LbPoolTest, NodeGoesDown3) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 3;
  SetUp(true);

  // Fail LB Node for the 1st time.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL, false);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));

  // Fail LB Node for the 2nd time.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL, false);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));

  // Fail LB Node for the 3rd time.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL, false);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));
}

// Test min_nodes initialization.
//
TEST_F(LbPoolTest, MinNodes1ForceUpInit) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 1;
  base_config["lbpool.example.com"]["min_nodes"] = 1;
  base_config["lbpool.example.com"]["min_nodes_action"] = "force_up";
  SetUp(false);

  // LB Pool will force up a random LB Node on startup with all LB Nodes
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
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, true);
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_PASS, true);
  EndDummyHC(test_lb_pool, "lbnode3", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));

  // Fail one LB Node.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL, false);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));

  // Fail one more LB Node, the whole LB Pool will fail.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_FAIL, true);
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
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // Make the same LB Node down.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL, false);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // Have a different LB Node pass tests, LB Pool will switch to it.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2"}));

  // Make the same LB Node down, it will be kept.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_FAIL, false);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2"}));
}

// Test max_nodes behaviour.
//
TEST_F(LbPoolTest, MaxNodes1) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 1;
  base_config["lbpool.example.com"]["max_nodes"] = 1;
  SetUp(false);

  // Have one LB Node pass tests.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // Add one more LB Node, it will not change anything.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // Fail the 1st LB Node, it will switch to the 2nd.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL, false);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2"}));

  // Pass the 3st LB Node, it will not change anything.
  EndDummyHC(test_lb_pool, "lbnode3", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2"}));

  // Fail the 2nd LB Node, it will switch to the 3nd.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_FAIL, false);
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
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // Add one more, there will be no change.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // The 1st one dies, switch to the 2nd live one.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL, false);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2"}));

  // The 2st one dies, no change.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_FAIL, false);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2"}));
}

// Test downtime functionality, it will override max_hc_failed and fail the
// LB Node immediately.
//
TEST_F(LbPoolTest, DowntimeFromAdminState) {

  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 3;
  SetUp(true);

  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));

  // Downtime a LB Node.
  GetLbNode(test_lb_pool, "lbnode1")->change_downtime("maintenance");
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));

  // Finished HC changes nothing.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));

  // End downtime, LB Node will go up only once it passes a HC.
  GetLbNode(test_lb_pool, "lbnode1")->change_downtime("online");
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));
  // Pretend the next the check passes, as it would in testtool.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));
}

// Test downtime from HC functionality. it will override max_hc_failed and fail
// the LB Node immediately.
//
TEST_F(LbPoolTest, DowntimeFromHealthcheckDrain) {

  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 3;
  SetUp(true);

  // Downtime a LB Node from its Healthcheck.
  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_DRAIN, false);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode3"}));

  EndDummyHC(test_lb_pool, "lbnode2", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));
}

// Test that admin_state downtime overrides HC downtime.
//
TEST_F(LbPoolTest, DowntimeFromHealthcheckAndAdminState) {
  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 3;
  SetUp(true);

  // Downtime a LB Node from its Healthcheck.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_DRAIN, false);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));

  // Downtime it again from admin_state.
  GetLbNode(test_lb_pool, "lbnode1")->change_downtime("maintenance");
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));

  // Pass a HC.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));

  // Remove admin_state downtime.
  GetLbNode(test_lb_pool, "lbnode1")->change_downtime("online");
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));
  // Pretend the next the check passes, as it would in testtool.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1", "lbnode2", "lbnode3"}));
}

// Initial config loading in maintenance state.
//
TEST_F(LbPoolTest, InitUpDowntimedMaintenance) {
  base_config["lbpool.example.com"]["nodes"]["lbnode1"]["state"] =
      "maintenance";
  SetUp(true);

  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));
}

// Initial config loading in draining state.
//
TEST_F(LbPoolTest, InitUpDowntimedDrain) {
  base_config["lbpool.example.com"]["nodes"]["lbnode1"]["state"] =
      "deploy_offline";
  SetUp(true);

  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode2", "lbnode3"}));
}

// NDCO-4445: Force-up LB Nodes not added after downtime ends
// if they change state.
//
TEST_F(LbPoolTest, ForceUpDowntimedDrain) {
  // Only one LB Node in this test.
  base_config["lbpool.example.com"]["nodes"].erase("lbnode2");
  base_config["lbpool.example.com"]["nodes"].erase("lbnode3");

  base_config["lbpool.example.com"]["health_checks"][0]["hc_max_failed"] = 1;
  base_config["lbpool.example.com"]["min_nodes"] = 1;
  base_config["lbpool.example.com"]["min_nodes_action"] = "force_up";

  SetUp(true);

  // Start with an online, up LB Node.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // Start a deployment.
  GetLbNode(test_lb_pool, "lbnode1")->change_downtime("deploy_offline");
  EXPECT_EQ(UpNodesNames(), set<string>());

  // Web server is stopped while the deployment is in progress.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_FAIL, true);
  EXPECT_EQ(UpNodesNames(), set<string>());

  // The LB Node is set back to online before the deployment is finished.
  // min_nodes_action is obeyed, the LB Node is added even though it's down.
  GetLbNode(test_lb_pool, "lbnode1")->change_downtime("online");
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));

  // The deployment is finished.
  EndDummyHC(test_lb_pool, "lbnode1", HealthcheckResult::HC_PASS, true);
  EXPECT_EQ(UpNodesNames(), set<string>({"lbnode1"}));
}
