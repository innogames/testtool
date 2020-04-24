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

#include "pfctl_worker.h"

using namespace std;

bool pfctl_run_command(vector<string> *args, vector<string> *lines);
bool pf_table_add(string *table, set<string> *addresses);
bool pf_table_del(string *table, set<string> *addresses);
bool pf_kill_src_nodes_to(string *table, string *address, bool with_states);
bool pf_kill_states_to_rdr(string *table, string *address);
bool pf_get_table(string table, set<string> *result);
bool pf_is_in_table(string *table, string *address, bool *answer);
bool pf_table_rebalance(string *table, set<string> *skip_addresses);
bool pf_sync_table(string table, SyncedLbNode *synced_lb_nodes);

#endif
