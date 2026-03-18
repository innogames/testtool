//
// Tests for pfctl async API
//

#include <gtest/gtest.h>
#include <iostream>
#include <openssl/ssl.h>
#include <set>
#include <string>
#include <vector>

#include "msg.h"
#include "pfctl.h"
#include "pfctl_async.h"
#include "pfctl_test.h"

using namespace std;

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
PfctlAsync *pfctl_async = nullptr;

// === PfctlAsync::run_command tests ===

TEST_F(PfctlTest, RunCommandBasicAdd) {
  bool ret = RunCommand({"-t", "test_table", "-T", "add", "2001:db8::1"});
  EXPECT_TRUE(ret);
  EXPECT_EQ(GetTable("test_table"), set<string>({"2001:db8::1"}));
}

TEST_F(PfctlTest, RunCommandWithOutput) {
  SetTable("test_table", {"2001:db8::1", "2001:db8::2"});

  vector<string> lines;
  bool ret = RunCommand({"-t", "test_table", "-T", "show"}, &lines);
  EXPECT_TRUE(ret);
  EXPECT_EQ(lines.size(), 2);
}

TEST_F(PfctlTest, RunCommandPfActionFalse) {
  pf_action = false;
  bool ret = RunCommand({"-t", "test_table", "-T", "add", "2001:db8::1"});
  EXPECT_TRUE(ret);
  // Table should be empty since command was not executed
  EXPECT_EQ(GetTable("test_table"), set<string>({}));
  pf_action = true;
}

TEST_F(PfctlTest, RunCommandFailure) {
  // Show on non-existent table should fail
  vector<string> lines;
  bool ret = RunCommand({"-t", "nonexistent", "-T", "show"}, &lines);
  EXPECT_FALSE(ret);
}

// === Table add tests ===

TEST_F(PfctlTest, TableAddSingle) {
  bool ret = RunCommand({"-t", "pool_0", "-T", "add", "2001:db8::1"});
  EXPECT_TRUE(ret);
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1"}));
}

TEST_F(PfctlTest, TableAddMultiple) {
  bool ret = RunCommand({"-t", "pool_0", "-T", "add", "2001:db8::1", "2001:db8::2", "2001:db8::3"});
  EXPECT_TRUE(ret);
  EXPECT_EQ(GetTable("pool_0"),
            set<string>({"2001:db8::1", "2001:db8::2", "2001:db8::3"}));
}

TEST_F(PfctlTest, TableAddEmptySet) {
  // Empty add creates the table but adds no addresses
  bool ret = RunCommand({"-t", "pool_0", "-T", "add"});
  EXPECT_TRUE(ret);
  EXPECT_EQ(GetTable("pool_0"), set<string>({}));
}

TEST_F(PfctlTest, TableAddIdempotent) {
  bool ret = RunCommand({"-t", "pool_0", "-T", "add", "2001:db8::1"});
  EXPECT_TRUE(ret);
  ret = RunCommand({"-t", "pool_0", "-T", "add", "2001:db8::1"});
  EXPECT_TRUE(ret);
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1"}));
}

// === Table del tests ===

TEST_F(PfctlTest, TableDelSingle) {
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2"});
  bool ret = RunCommand({"-t", "pool_0", "-T", "del", "2001:db8::1"});
  EXPECT_TRUE(ret);
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::2"}));
}

TEST_F(PfctlTest, TableDelAll) {
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2"});
  bool ret = RunCommand({"-t", "pool_0", "-T", "del", "2001:db8::1", "2001:db8::2"});
  EXPECT_TRUE(ret);
  EXPECT_EQ(GetTable("pool_0"), set<string>({}));
}

TEST_F(PfctlTest, TableDelEmptySet) {
  // Del with no addresses is a no-op
  bool ret = RunCommand({"-t", "pool_0", "-T", "del"});
  EXPECT_TRUE(ret);
}

TEST_F(PfctlTest, TableDelNonexistent) {
  SetTable("pool_0", {"2001:db8::1"});
  bool ret = RunCommand({"-t", "pool_0", "-T", "del", "2001:db8::99"});
  EXPECT_TRUE(ret);
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1"}));
}

// === Kill src_nodes tests ===

TEST_F(PfctlTest, KillSrcNodesWithoutStates) {
  bool ret = RunCommand({"-K", "table", "-K", "pool_0", "-K", "dsthost", "-K", "2001:db8::1"});
  EXPECT_TRUE(ret);

  json kill_log = GetKillLog();
  ASSERT_EQ(kill_log.size(), 1);
  EXPECT_EQ(kill_log[0]["type"], "src_nodes");
  EXPECT_EQ(kill_log[0]["table"], "pool_0");
  EXPECT_EQ(kill_log[0]["address"], "2001:db8::1");
  EXPECT_EQ(kill_log[0]["with_states"], false);
}

TEST_F(PfctlTest, KillSrcNodesWithStates) {
  bool ret = RunCommand({"-K", "table", "-K", "pool_0", "-K", "dsthost", "-K", "2001:db8::1",
                          "-K", "kill", "-K", "rststates"});
  EXPECT_TRUE(ret);

  json kill_log = GetKillLog();
  ASSERT_EQ(kill_log.size(), 1);
  EXPECT_EQ(kill_log[0]["type"], "src_nodes");
  EXPECT_EQ(kill_log[0]["with_states"], true);
}

// === Kill states tests ===

TEST_F(PfctlTest, KillStatesToRdr) {
  bool ret = RunCommand({"-k", "table", "-k", "pool_0", "-k", "rdrhost", "-k", "2001:db8::1",
                          "-k", "kill", "-k", "rststates"});
  EXPECT_TRUE(ret);

  json kill_log = GetKillLog();
  ASSERT_EQ(kill_log.size(), 1);
  EXPECT_EQ(kill_log[0]["type"], "states_rdr");
  EXPECT_EQ(kill_log[0]["table"], "pool_0");
  EXPECT_EQ(kill_log[0]["address"], "2001:db8::1");
}

// === Get table tests (via show command + output parsing) ===

TEST_F(PfctlTest, GetTableExisting) {
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2"});

  vector<string> lines;
  bool ret = RunCommand({"-t", "pool_0", "-T", "show"}, &lines);
  EXPECT_TRUE(ret);

  // Parse output (mock produces "   addr" format lines)
  set<string> result;
  for (auto &line : lines) {
    // Trim whitespace
    size_t start = line.find_first_not_of(" \t");
    if (start != string::npos)
      result.insert(line.substr(start));
  }
  EXPECT_EQ(result, set<string>({"2001:db8::1", "2001:db8::2"}));
}

TEST_F(PfctlTest, GetTableEmpty) {
  SetTable("pool_0", {});

  vector<string> lines;
  bool ret = RunCommand({"-t", "pool_0", "-T", "show"}, &lines);
  EXPECT_TRUE(ret);
  EXPECT_EQ(lines.size(), 0);
}

TEST_F(PfctlTest, GetTableNonexistentCreatesIt) {
  // Show on nonexistent table fails, then add creates it
  vector<string> lines;
  bool ret = RunCommand({"-t", "new_table", "-T", "show"}, &lines);
  EXPECT_FALSE(ret);

  // Create the table via add
  ret = RunCommand({"-t", "new_table", "-T", "add"});
  EXPECT_TRUE(ret);

  // Table should now exist in state
  EXPECT_TRUE(ReadState()["tables"].contains("new_table"));
}

TEST_F(PfctlTest, GetTableMultipleAddresses) {
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2", "2001:db8::3"});

  vector<string> lines;
  bool ret = RunCommand({"-t", "pool_0", "-T", "show"}, &lines);
  EXPECT_TRUE(ret);
  EXPECT_EQ(lines.size(), 3);
}

// === Is-in-table tests (show + check output) ===

TEST_F(PfctlTest, IsInTablePresent) {
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2"});

  vector<string> lines;
  bool ret = RunCommand({"-t", "pool_0", "-T", "show"}, &lines);
  EXPECT_TRUE(ret);

  // Parse and check
  set<string> result;
  for (auto &line : lines) {
    size_t start = line.find_first_not_of(" \t");
    if (start != string::npos)
      result.insert(line.substr(start));
  }
  EXPECT_TRUE(result.count("2001:db8::1") > 0);
}

TEST_F(PfctlTest, IsInTableAbsent) {
  SetTable("pool_0", {"2001:db8::1"});

  vector<string> lines;
  bool ret = RunCommand({"-t", "pool_0", "-T", "show"}, &lines);
  EXPECT_TRUE(ret);

  set<string> result;
  for (auto &line : lines) {
    size_t start = line.find_first_not_of(" \t");
    if (start != string::npos)
      result.insert(line.substr(start));
  }
  EXPECT_FALSE(result.count("2001:db8::99") > 0);
}

TEST_F(PfctlTest, IsInTableEmptyTable) {
  SetTable("pool_0", {});

  vector<string> lines;
  bool ret = RunCommand({"-t", "pool_0", "-T", "show"}, &lines);
  EXPECT_TRUE(ret);

  set<string> result;
  for (auto &line : lines) {
    size_t start = line.find_first_not_of(" \t");
    if (start != string::npos)
      result.insert(line.substr(start));
  }
  EXPECT_FALSE(result.count("2001:db8::1") > 0);
}

// === Rebalance tests (via sync_table) ===

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

TEST_F(PfctlTest, RebalanceKillsOldEntries) {
  // Start with existing entries, add a new one via sync, verify rebalance kills
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2", "2001:db8::3"});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  // All 3 wanted + a new one (::4) to trigger rebalance
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);
  SetSyncedNode(nodes, 1, "2001:db8::2", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);
  SetSyncedNode(nodes, 2, "2001:db8::3", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);
  SetSyncedNode(nodes, 3, "2001:db8::4", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);

  EXPECT_TRUE(RunSyncTable("pool_0", nodes));
  EXPECT_EQ(GetTable("pool_0"),
            set<string>({"2001:db8::1", "2001:db8::2", "2001:db8::3", "2001:db8::4"}));

  // Should have rebalance kills for the old entries (::1, ::2, ::3)
  json kill_log = GetKillLog();
  set<string> killed;
  for (const auto &entry : kill_log) {
    if (entry["type"] == "src_nodes") {
      killed.insert(entry["address"].get<string>());
    }
  }
  EXPECT_TRUE(killed.count("2001:db8::1") > 0);
  EXPECT_TRUE(killed.count("2001:db8::2") > 0);
  EXPECT_TRUE(killed.count("2001:db8::3") > 0);
}

TEST_F(PfctlTest, RebalanceNonexistentTable) {
  // Sync on nonexistent table should create it and add nodes
  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);

  EXPECT_TRUE(RunSyncTable("new_table", nodes));
  EXPECT_EQ(GetTable("new_table"), set<string>({"2001:db8::1"}));
}

// === pf_sync_table_async tests ===

TEST_F(PfctlTest, SyncTableAddNodes) {
  SetTable("pool_0", {});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);
  SetSyncedNode(nodes, 1, "2001:db8::2", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);

  EXPECT_TRUE(RunSyncTable("pool_0", nodes));
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1", "2001:db8::2"}));
}

TEST_F(PfctlTest, SyncTableRemoveNodes) {
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2", "2001:db8::3"});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);
  SetSyncedNode(nodes, 1, "2001:db8::2", LbNodeState::STATE_DOWN,
                LbNodeAdminState::STATE_ENABLED);
  SetSyncedNode(nodes, 2, "2001:db8::3", LbNodeState::STATE_DOWN,
                LbNodeAdminState::STATE_ENABLED);

  EXPECT_TRUE(RunSyncTable("pool_0", nodes));
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1"}));
}

TEST_F(PfctlTest, SyncTableNoChange) {
  SetTable("pool_0", {"2001:db8::1", "2001:db8::2"});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);
  SetSyncedNode(nodes, 1, "2001:db8::2", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);

  EXPECT_TRUE(RunSyncTable("pool_0", nodes));
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1", "2001:db8::2"}));
  // No kills should have happened
  EXPECT_EQ(GetKillLog().size(), 0);
}

TEST_F(PfctlTest, SyncTableDrainSoftNoKillStates) {
  SetTable("pool_0", {"2001:db8::1"});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_DOWN,
                LbNodeAdminState::STATE_DRAIN_SOFT);

  EXPECT_TRUE(RunSyncTable("pool_0", nodes));
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
  SetTable("pool_0", {"2001:db8::1"});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_DOWN,
                LbNodeAdminState::STATE_DRAIN_HARD);

  EXPECT_TRUE(RunSyncTable("pool_0", nodes));
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
  SetTable("pool_0", {"2001:db8::1"});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_DOWN,
                LbNodeAdminState::STATE_DOWNTIME);

  EXPECT_TRUE(RunSyncTable("pool_0", nodes));

  json kill_log = GetKillLog();
  bool has_states_rdr = false;
  for (const auto &entry : kill_log) {
    if (entry["type"] == "states_rdr")
      has_states_rdr = true;
  }
  EXPECT_TRUE(has_states_rdr);
}

TEST_F(PfctlTest, SyncTableRebalancesOnAdd) {
  SetTable("pool_0", {"2001:db8::1"});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);
  SetSyncedNode(nodes, 1, "2001:db8::2", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_ENABLED);

  EXPECT_TRUE(RunSyncTable("pool_0", nodes));
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
  SetTable("pool_0", {});

  SyncedLbNode nodes[MAX_NODES];
  InitSyncedNodes(nodes);
  SetSyncedNode(nodes, 0, "2001:db8::1", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_DRAIN_HARD);
  SetSyncedNode(nodes, 1, "2001:db8::2", LbNodeState::STATE_UP,
                LbNodeAdminState::STATE_DOWNTIME);

  EXPECT_TRUE(RunSyncTable("pool_0", nodes));
  EXPECT_EQ(GetTable("pool_0"), set<string>({}));
}

// === New PfctlAsync-specific tests ===

TEST_F(PfctlTest, AsyncQueueSerializes) {
  // Queue 2 commands and verify they execute in order
  vector<string> order;
  done_ = false;
  int completed = 0;

  pfctl_->run_command({"-t", "pool_0", "-T", "add", "2001:db8::1"},
    [&](bool success, vector<string> out) {
      order.push_back("first");
      completed++;
      if (completed == 2) done_ = true;
    });

  pfctl_->run_command({"-t", "pool_0", "-T", "add", "2001:db8::2"},
    [&](bool success, vector<string> out) {
      order.push_back("second");
      completed++;
      if (completed == 2) done_ = true;
    });

  RunUntilDone();

  ASSERT_EQ(order.size(), 2);
  EXPECT_EQ(order[0], "first");
  EXPECT_EQ(order[1], "second");
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1", "2001:db8::2"}));
}

TEST_F(PfctlTest, AsyncQueueFullReturnsFalse) {
  // Fill queue beyond MAX_QUEUE_LEN while a command is running
  // First, start a command to make the instance busy
  bool first_done = false;
  pfctl_->run_command({"-t", "pool_0", "-T", "add", "2001:db8::1"},
    [&](bool success, vector<string> out) {
      first_done = true;
    });

  // Now queue MAX_QUEUE_LEN more commands (filling the queue)
  for (size_t i = 0; i < 10; i++) {
    bool queued = pfctl_->run_command(
      {"-t", "pool_0", "-T", "add", "2001:db8::" + to_string(i + 10)},
      [](bool, vector<string>) {});
    // First 10 should succeed (queue has room)
    EXPECT_TRUE(queued) << "Command " << i << " should have been queued";
  }

  // This should fail - queue is full and a child is running
  bool queued = pfctl_->run_command(
    {"-t", "pool_0", "-T", "add", "2001:db8::99"},
    [](bool, vector<string>) {});
  EXPECT_FALSE(queued);

  // Drain to clean up
  pfctl_->drain();
}

TEST_F(PfctlTest, AsyncDrainCompletes) {
  // Queue commands, call drain(), verify all callbacks fired
  int completed = 0;

  pfctl_->run_command({"-t", "pool_0", "-T", "add", "2001:db8::1"},
    [&](bool success, vector<string> out) {
      EXPECT_TRUE(success);
      completed++;
    });

  pfctl_->run_command({"-t", "pool_0", "-T", "add", "2001:db8::2"},
    [&](bool success, vector<string> out) {
      EXPECT_TRUE(success);
      completed++;
    });

  pfctl_->drain();

  EXPECT_EQ(completed, 2);
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1", "2001:db8::2"}));
}

TEST_F(PfctlTest, AsyncCommandFailureContinues) {
  // Queue a failing command followed by a succeeding one
  // Both should execute
  done_ = false;
  int completed = 0;
  bool first_success = true;
  bool second_success = false;

  pfctl_->run_command({"-t", "nonexistent", "-T", "show"},
    [&](bool success, vector<string> out) {
      first_success = success;
      completed++;
      if (completed == 2) done_ = true;
    });

  pfctl_->run_command({"-t", "pool_0", "-T", "add", "2001:db8::1"},
    [&](bool success, vector<string> out) {
      second_success = success;
      completed++;
      if (completed == 2) done_ = true;
    });

  RunUntilDone();

  EXPECT_FALSE(first_success);
  EXPECT_TRUE(second_success);
  EXPECT_EQ(GetTable("pool_0"), set<string>({"2001:db8::1"}));
}

// === main ===

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
