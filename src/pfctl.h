//
// Testtool - PF Controls
//
// Copyright (c) 2018 InnoGames GmbH
//

#ifndef _PFCTL_H_
#define _PFCTL_H_

#include <set>
#include <string>
#include <vector>

#include "lb_node.h"

using namespace std;

#define NAME_LEN 256 // 64 in Serveradmin
// Operations which got into queue are installed as soon as possible.
// Operations which did not fit will be re-done after given HC finishes its
// next run. Usually checks run each 2000ms and each operation takes around
// 120ms. Optimal lenght would be 16. Keep it a bit shorter in case operations
// take way longer, for example when HWLB is under a DDoS.
#define QUEUE_LEN 10
#define MAX_NODES 100 // I hope 100 LB Nodes is reasonable enough
#define ADDR_LEN sizeof("FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:255.255.255.255") + 1

typedef struct {
  LbNodeState wanted_state;     // To add or remove LB Node from table.
  LbNodeAdminState admin_state; // How to remove LB Node from table.
  // IPv4 and IPv6 addresses.
  char ip_address[2][ADDR_LEN];
} SyncedLbNode;

typedef struct {
  // This struct is sent over a queue, complex datatypes won't work here.
  char pool_name[NAME_LEN];
  char table_name[NAME_LEN];
  SyncedLbNode synced_lb_nodes[MAX_NODES];
} pfctl_msg;

extern string pfctl_command;

// Synchronous function — used at startup to check initial node state
bool pf_is_in_table(string *table, string *address, bool *answer);

#include "pfctl_async.h"
#include <functional>

using PfSyncDoneCallback = std::function<void(bool success)>;

struct PfSyncContext {
    PfctlAsync *pfctl_async;
    std::string table;
    SyncedLbNode synced_lb_nodes[MAX_NODES];
    PfSyncDoneCallback done_callback;

    // Computed during sync
    std::set<std::string> cur_set;
    std::set<std::string> want_set;
    std::set<std::string> to_add;
    std::set<std::string> to_del;

    // Kill chain iteration state
    struct KillEntry {
        std::string ip;
        bool with_states;
    };
    std::vector<KillEntry> kill_list;
    size_t kill_index = 0;
    int kill_phase = 0;  // 0=src_nodes, 1=states_rdr, 2=src_nodes_again

    // Rebalance iteration state
    std::vector<std::string> rebalance_list;
    size_t rebalance_index = 0;

    void start();
    void on_get_table(bool success, std::vector<std::string> output);
    void on_table_created(bool success, std::vector<std::string> output);
    void compute_diff();
    void on_del(bool success, std::vector<std::string> output);
    void kill_next_node();
    void on_kill_step(bool success, std::vector<std::string> output);
    void do_add();
    void on_add(bool success, std::vector<std::string> output);
    void start_rebalance();
    void on_rebalance_get_table(bool success, std::vector<std::string> output);
    void kill_next_rebalance_entry();
    void on_rebalance_kill(bool success, std::vector<std::string> output);
    void finish(bool success);
};

void pf_sync_table_async(PfctlAsync *pfctl_async, std::string table,
                          SyncedLbNode *synced_lb_nodes,
                          PfSyncDoneCallback done_callback);

#endif
