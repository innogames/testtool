#ifndef _PFCTL_H_
#define _PFCTL_H_

#include <string>

using namespace std;

void pf_kill_src_nodes_to(string &ip, bool with_states);
void pf_table_add(string &table, string &ip);
void pf_table_del(string &table, string &ip);
int  pf_is_in_table(string &table, string &ip);
void pf_table_rebalance(string &table, string &ip);

#endif

