#ifndef PFCTL_TEST_H
#define PFCTL_TEST_H

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <event2/event.h>

#include "cmake_dirs.h"
#include "pfctl.h"
#include "pfctl_async.h"

using namespace std;
using json = nlohmann::json;

extern bool pf_action;
extern int verbose_pfctl;

class PfctlTest : public ::testing::Test {
protected:
  string state_file;
  string mock_pfctl_path;
  struct event_base *base_;
  PfctlAsync *pfctl_;
  bool done_;
  bool last_success_;
  vector<string> last_output_;

  virtual void SetUp() override {
    // Point pfctl_command to mock script
    mock_pfctl_path = string(CMAKE_SOURCE_DIR) + "/tests/mock_pfctl.py";
    pfctl_command = mock_pfctl_path;

    // Create temp state file
    state_file = string(MAKE_BINARY_DIR) + "/pfctl_test_state_" +
                 ::testing::UnitTest::GetInstance()
                     ->current_test_info()
                     ->name() +
                 ".json";

    // Set env var for mock script
    setenv("MOCK_PFCTL_STATE", state_file.c_str(), 1);

    // Ensure pf_action is true so commands actually run
    pf_action = true;
    verbose_pfctl = 0;

    // Start with empty state
    ofstream f(state_file);
    f << R"({"tables": {}, "kill_log": []})";
    f.close();

    // Initialize libevent base and PfctlAsync
    base_ = event_base_new();
    pfctl_ = new PfctlAsync(base_);
    done_ = false;
    last_success_ = false;
  }

  virtual void TearDown() override {
    delete pfctl_;
    event_base_free(base_);
    filesystem::remove(state_file);
  }

  // Run the event loop until done_ flag is set by a callback
  void RunUntilDone() {
    while (!done_) {
      event_base_loop(base_, EVLOOP_ONCE);
    }
  }

  // Run a single pfctl command asynchronously and wait for completion
  bool RunCommand(vector<string> args, vector<string> *output = nullptr) {
    done_ = false;
    last_success_ = false;
    last_output_.clear();
    pfctl_->run_command(std::move(args), [this](bool success, vector<string> out) {
      last_success_ = success;
      last_output_ = std::move(out);
      done_ = true;
    });
    RunUntilDone();
    if (output) *output = last_output_;
    return last_success_;
  }

  // Run pf_sync_table_async and wait for completion
  bool RunSyncTable(const string &table, SyncedLbNode *nodes) {
    done_ = false;
    last_success_ = false;
    pf_sync_table_async(pfctl_, table, nodes, [this](bool success) {
      last_success_ = success;
      done_ = true;
    });
    RunUntilDone();
    return last_success_;
  }

  // Helper to read mock state
  json ReadState() {
    ifstream f(state_file);
    json state;
    f >> state;
    return state;
  }

  // Helper to set initial table state
  void SetTable(string table, set<string> addresses) {
    json state = ReadState();
    state["tables"][table] = json::array();
    for (const auto &addr : addresses) {
      state["tables"][table].push_back(addr);
    }
    ofstream f(state_file);
    f << state.dump(2);
    f.close();
  }

  // Helper to get addresses in table from state file
  set<string> GetTable(string table) {
    json state = ReadState();
    set<string> result;
    if (state["tables"].contains(table)) {
      for (const auto &addr : state["tables"][table]) {
        result.insert(addr.get<string>());
      }
    }
    return result;
  }

  // Helper to get kill log
  json GetKillLog() {
    json state = ReadState();
    return state["kill_log"];
  }
};

#endif
