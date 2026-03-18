//
// Tests for pfctl.cpp functions
//

#include <boost/interprocess/ipc/message_queue.hpp>
#include <gtest/gtest.h>
#include <iostream>
#include <openssl/ssl.h>
#include <set>
#include <string>

#include "msg.h"
#include "pfctl.h"
#include "pfctl_test.h"
#include "pfctl_worker.h"

using namespace std;
using namespace boost::interprocess;

// Global variables required by pfctl.cpp
bool pf_action = true;
int verbose_pfctl = 0;

// Mock log functions (same pattern as testtool_test.cpp)
void log(MessageType loglevel, string msg) {
  cerr << msg << endl;
}

void log(MessageType loglevel, LbPool *lbpool, string msg) {
  cerr << "lbpool: " << msg << endl;
}

void log(MessageType loglevel, LbNode *lbnode, string msg) {
  cerr << "lbnode: " << msg << endl;
}

void log(MessageType loglevel, Healthcheck *hc, string msg) {
  cerr << "healthcheck: " << msg << endl;
}

// Global variables required by lb_pool.cpp / lb_node.cpp if linked
struct event_base *eventBase = NULL;
SSL_CTX *sctx = NULL;
int verbose = 0;
message_queue *pfctl_mq;

// Mock send_message (lb_pool.cpp calls this via pfctl_worker.cpp which is excluded)
bool send_message(message_queue *mq, string pool_name, string table_name,
                  set<LbNode *> all_lb_nodes, set<LbNode *> up_lb_nodes) {
  (void)(mq);
  (void)(pool_name);
  (void)(table_name);
  (void)(all_lb_nodes);
  (void)(up_lb_nodes);
  return true;
}

// === pfctl_run_command tests ===

TEST_F(PfctlTest, RunCommandBasicAdd) {
  // Test that pfctl_run_command correctly spawns the mock script
  vector<string> args = {"-t", "test_table", "-T", "add", "2001:db8::1"};
  bool ret = pfctl_run_command(&args, NULL);
  EXPECT_TRUE(ret);
  EXPECT_EQ(GetTable("test_table"), set<string>({"2001:db8::1"}));
}

TEST_F(PfctlTest, RunCommandWithOutput) {
  SetTable("test_table", {"2001:db8::1", "2001:db8::2"});

  vector<string> args = {"-t", "test_table", "-T", "show"};
  vector<string> lines;
  bool ret = pfctl_run_command(&args, &lines);
  EXPECT_TRUE(ret);
  EXPECT_EQ(lines.size(), 2);
}

TEST_F(PfctlTest, RunCommandPfActionFalse) {
  pf_action = false;
  vector<string> args = {"-t", "test_table", "-T", "add", "2001:db8::1"};
  bool ret = pfctl_run_command(&args, NULL);
  EXPECT_TRUE(ret);
  // Table should be empty since command was not executed
  EXPECT_EQ(GetTable("test_table"), set<string>({}));
  pf_action = true;
}

TEST_F(PfctlTest, RunCommandFailure) {
  // Show on non-existent table should fail
  vector<string> args = {"-t", "nonexistent", "-T", "show"};
  vector<string> lines;
  bool ret = pfctl_run_command(&args, &lines);
  EXPECT_FALSE(ret);
}

// === pf_table_add tests ===

TEST_F(PfctlTest, TableAddSingle) {
  string table = "pool_0";
  set<string> addrs = {"2001:db8::1"};
  EXPECT_TRUE(pf_table_add(&table, &addrs));
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1"}));
}

TEST_F(PfctlTest, TableAddMultiple) {
  string table = "pool_0";
  set<string> addrs = {"2001:db8::1", "2001:db8::2", "2001:db8::3"};
  EXPECT_TRUE(pf_table_add(&table, &addrs));
  EXPECT_EQ(GetTable("pool_0"),
            set<string>({"2001:db8::1", "2001:db8::2", "2001:db8::3"}));
}

TEST_F(PfctlTest, TableAddEmptySet) {
  string table = "pool_0";
  set<string> addrs = {};
  // Empty set returns true without calling pfctl
  EXPECT_TRUE(pf_table_add(&table, &addrs));
  EXPECT_EQ(GetTable("pool_0"), set<string>({}));
}

TEST_F(PfctlTest, TableAddIdempotent) {
  string table = "pool_0";
  set<string> addrs = {"2001:db8::1"};
  EXPECT_TRUE(pf_table_add(&table, &addrs));
  EXPECT_TRUE(pf_table_add(&table, &addrs));
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1"}));
}

// === pf_table_del tests ===

TEST_F(PfctlTest, TableDelSingle) {
  string table = "pool_0";
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2"});
  set<string> addrs = {"2001:db8::1"};
  EXPECT_TRUE(pf_table_del(&table, &addrs));
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::2"}));
}

TEST_F(PfctlTest, TableDelAll) {
  string table = "pool_0";
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2"});
  set<string> addrs = {"2001:db8::1", "2001:db8::2"};
  EXPECT_TRUE(pf_table_del(&table, &addrs));
  EXPECT_EQ(GetTable("pool_0"), set<string>({}));
}

TEST_F(PfctlTest, TableDelEmptySet) {
  string table = "pool_0";
  set<string> addrs = {};
  EXPECT_TRUE(pf_table_del(&table, &addrs));
}

TEST_F(PfctlTest, TableDelNonexistent) {
  string table = "pool_0";
  SetTable("pool_0", {"2001:db8::1"});
  set<string> addrs = {"2001:db8::99"};
  EXPECT_TRUE(pf_table_del(&table, &addrs));
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1"}));
}

// === pf_kill_src_nodes_to tests ===

TEST_F(PfctlTest, KillSrcNodesWithoutStates) {
  string table = "pool_0";
  string addr = "2001:db8::1";
  EXPECT_TRUE(pf_kill_src_nodes_to(&table, &addr, false));

  json kill_log = GetKillLog();
  ASSERT_EQ(kill_log.size(), 1);
  EXPECT_EQ(kill_log[0]["type"], "src_nodes");
  EXPECT_EQ(kill_log[0]["table"], "pool_0");
  EXPECT_EQ(kill_log[0]["address"], "2001:db8::1");
  EXPECT_EQ(kill_log[0]["with_states"], false);
}

TEST_F(PfctlTest, KillSrcNodesWithStates) {
  string table = "pool_0";
  string addr = "2001:db8::1";
  EXPECT_TRUE(pf_kill_src_nodes_to(&table, &addr, true));

  json kill_log = GetKillLog();
  ASSERT_EQ(kill_log.size(), 1);
  EXPECT_EQ(kill_log[0]["type"], "src_nodes");
  EXPECT_EQ(kill_log[0]["with_states"], true);
}

// === pf_kill_states_to_rdr tests ===

TEST_F(PfctlTest, KillStatesToRdr) {
  string table = "pool_0";
  string addr = "2001:db8::1";
  EXPECT_TRUE(pf_kill_states_to_rdr(&table, &addr));

  json kill_log = GetKillLog();
  ASSERT_EQ(kill_log.size(), 1);
  EXPECT_EQ(kill_log[0]["type"], "states_rdr");
  EXPECT_EQ(kill_log[0]["table"], "pool_0");
  EXPECT_EQ(kill_log[0]["address"], "2001:db8::1");
}

// === pf_get_table tests ===

TEST_F(PfctlTest, GetTableExisting) {
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2"});
  string table = "pool_0";
  set<string> result;
  EXPECT_TRUE(pf_get_table(&table, &result));
  EXPECT_EQ(result, set<string>({"2001:db8::1", "2001:db8::2"}));
}

TEST_F(PfctlTest, GetTableEmpty) {
  SetTable("pool_0", {});
  string table = "pool_0";
  set<string> result;
  EXPECT_TRUE(pf_get_table(&table, &result));
  EXPECT_EQ(result, set<string>({}));
}

TEST_F(PfctlTest, GetTableNonexistentCreatesIt) {
  // pf_get_table creates the table if it doesn't exist
  string table = "new_table";
  set<string> result;
  EXPECT_TRUE(pf_get_table(&table, &result));
  EXPECT_EQ(result, set<string>({}));
  // Table should now exist in state
  EXPECT_TRUE(ReadState()["tables"].contains("new_table"));
}

TEST_F(PfctlTest, GetTableMultipleAddresses) {
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2", "2001:db8::3"});
  string table = "pool_0";
  set<string> result;
  EXPECT_TRUE(pf_get_table(&table, &result));
  EXPECT_EQ(result,
            set<string>({"2001:db8::1", "2001:db8::2", "2001:db8::3"}));
}

// === pf_is_in_table tests ===

TEST_F(PfctlTest, IsInTablePresent) {
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2"});
  string table = "pool_0";
  string addr = "2001:db8::1";
  bool answer = false;
  EXPECT_TRUE(pf_is_in_table(&table, &addr, &answer));
  EXPECT_TRUE(answer);
}

TEST_F(PfctlTest, IsInTableAbsent) {
  SetTable("pool_0", {"2001:db8::1"});
  string table = "pool_0";
  string addr = "2001:db8::99";
  bool answer = true;
  EXPECT_TRUE(pf_is_in_table(&table, &addr, &answer));
  EXPECT_FALSE(answer);
}

TEST_F(PfctlTest, IsInTableEmptyTable) {
  SetTable("pool_0", {});
  string table = "pool_0";
  string addr = "2001:db8::1";
  bool answer = true;
  EXPECT_TRUE(pf_is_in_table(&table, &addr, &answer));
  EXPECT_FALSE(answer);
}

// === pf_table_rebalance tests ===

TEST_F(PfctlTest, RebalanceKillsOldEntries) {
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2", "2001:db8::3"});
  string table = "pool_0";
  set<string> skip = {"2001:db8::3"}; // newly added
  EXPECT_TRUE(pf_table_rebalance(&table, &skip));

  json kill_log = GetKillLog();
  // Should kill src_nodes for ::1 and ::2 (not ::3)
  ASSERT_EQ(kill_log.size(), 2);
  set<string> killed;
  for (const auto &entry : kill_log) {
    EXPECT_EQ(entry["type"], "src_nodes");
    EXPECT_EQ(entry["with_states"], false);
    killed.insert(entry["address"].get<string>());
  }
  EXPECT_EQ(killed, set<string>({"2001:db8::1", "2001:db8::2"}));
}

TEST_F(PfctlTest, RebalanceNonexistentTable) {
  // pf_get_table will create it, then no entries to kill
  string table = "new_table";
  set<string> skip = {"2001:db8::1"};
  EXPECT_TRUE(pf_table_rebalance(&table, &skip));
  EXPECT_EQ(GetKillLog().size(), 0);
}

// === pf_sync_table tests ===

// Helper to build SyncedLbNode array
static void InitSyncedNodes(SyncedLbNode *nodes) {
  memset(nodes, 0, sizeof(SyncedLbNode) * MAX_NODES);
}

static void SetSyncedNode(SyncedLbNode *nodes, int index,
                          const char *address, LbNodeState wanted,
                          LbNodeAdminState admin) {
  if (address)
    strncpy(nodes[index].ip_address[1], address, ADDR_LEN);
  nodes[index].wanted_state = wanted;
  nodes[index].admin_state = admin;
}

TEST_F(PfctlTest, SyncTableAddNodes) {
  // Empty table, add two wanted-up nodes
  SetTable("pool_0", {});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);
  SetSyncedNode(nodes, 1, "2001:db8::2", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);

  EXPECT_TRUE(pf_sync_table("pool_0", nodes));
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1", "2001:db8::2"}));
}

TEST_F(PfctlTest, SyncTableRemoveNodes) {
  // Table has 3 addresses, only 1 node is wanted
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2", "2001:db8::3"});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);
  SetSyncedNode(nodes, 1, "2001:db8::2", LbNodeState::STATE_DOWN,
                LbNodeAdminState::STATE_ENABLED);
  SetSyncedNode(nodes, 2, "2001:db8::3", LbNodeState::STATE_DOWN,
                LbNodeAdminState::STATE_ENABLED);

  EXPECT_TRUE(pf_sync_table("pool_0", nodes));
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1"}));
}

TEST_F(PfctlTest, SyncTableNoChange) {
  // Table matches desired state exactly
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2"});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);
  SetSyncedNode(nodes, 1, "2001:db8::2", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);

  EXPECT_TRUE(pf_sync_table("pool_0", nodes));
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1", "2001:db8::2"}));
  // No kills should have happened
  EXPECT_EQ(GetKillLog().size(), 0);
}

TEST_F(PfctlTest, SyncTableDrainSoftNoKillStates) {
  // DRAIN_SOFT nodes should NOT kill states (with_states = false)
  SetTable("pool_0", {"2001:db8::1"});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_DOWN,
                LbNodeAdminState::STATE_DRAIN_SOFT);

  EXPECT_TRUE(pf_sync_table("pool_0", nodes));
  EXPECT_EQ(GetTable("pool_0"), set<string>({}));

  // Check kill log: should have only src_nodes kill, no states kill
  json kill_log = GetKillLog();
  ASSERT_GE(kill_log.size(), 1);
  for (const auto &entry : kill_log) {
    if (entry["type"] == "src_nodes") {
      EXPECT_EQ(entry["with_states"], false);
    }
    // Should NOT have states_rdr entry
    EXPECT_NE(entry["type"], "states_rdr");
  }
}

TEST_F(PfctlTest, SyncTableDrainHardNoKillStates) {
  // DRAIN_HARD nodes should NOT kill states (with_states = false)
  // Both DRAIN_HARD and DRAIN_SOFT are <= STATE_DRAIN_SOFT in the enum,
  // so neither kills states. Only DOWNTIME and above do.
  SetTable("pool_0", {"2001:db8::1"});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_DOWN,
                LbNodeAdminState::STATE_DRAIN_HARD);

  EXPECT_TRUE(pf_sync_table("pool_0", nodes));
  EXPECT_EQ(GetTable("pool_0"), set<string>({}));

  // Check kill log: should have src_nodes without states, no states_rdr
  json kill_log = GetKillLog();
  ASSERT_GE(kill_log.size(), 1);
  for (const auto &entry : kill_log) {
    if (entry["type"] == "src_nodes") {
      EXPECT_EQ(entry["with_states"], false);
    }
    EXPECT_NE(entry["type"], "states_rdr");
  }
}

TEST_F(PfctlTest, SyncTableDowntimeKillsStates) {
  // DOWNTIME nodes should kill states (admin_state > DRAIN_SOFT)
  SetTable("pool_0", {"2001:db8::1"});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_DOWN,
                LbNodeAdminState::STATE_DOWNTIME);

  EXPECT_TRUE(pf_sync_table("pool_0", nodes));

  json kill_log = GetKillLog();
  bool has_states_rdr = false;
  for (const auto &entry : kill_log) {
    if (entry["type"] == "states_rdr")
      has_states_rdr = true;
  }
  EXPECT_TRUE(has_states_rdr);
}

TEST_F(PfctlTest, SyncTableRebalancesOnAdd) {
  // When new nodes are added, existing entries should be rebalanced
  SetTable("pool_0", {"2001:db8::1"});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);
  SetSyncedNode(nodes, 1, "2001:db8::2", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);

  EXPECT_TRUE(pf_sync_table("pool_0", nodes));
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1", "2001:db8::2"}));

  // Should have rebalance kills (src_nodes for ::1, the old entry)
  json kill_log = GetKillLog();
  bool rebalanced = false;
  for (const auto &entry : kill_log) {
    if (entry["type"] == "src_nodes" && entry["address"] == "2001:db8::1") {
      rebalanced = true;
    }
  }
  EXPECT_TRUE(rebalanced);
}

TEST_F(PfctlTest, SyncTableNonEnabledNodesNotAdded) {
  // Nodes that are UP but not ENABLED should NOT be added to the table
  SetTable("pool_0", {});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_DRAIN_HARD);
  SetSyncedNode(nodes, 1, "2001:db8::2", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_DOWNTIME);

  EXPECT_TRUE(pf_sync_table("pool_0", nodes));
  EXPECT_EQ(GetTable("pool_0"), set<string>({}));
}

// === main ===

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
