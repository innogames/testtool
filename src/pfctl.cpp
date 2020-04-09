//
// Testtool - PF Controls
//
// Copyright (c) 2018 InnoGames GmbH
//

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <iostream>
#include <set>
#include <sstream>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <sys/wait.h>

#include "msg.h"
#include "pfctl.h"
#include "pfctl_worker.h"

using namespace std;
using namespace std::chrono;

extern bool pf_action;
extern int verbose_pfctl;

bool pfctl_run_command(vector<string> *args, vector<string> *lines) {
  int ret = 0;
  FILE *fp;
  char buffer[1024];

  string cmd = "/sbin/pfctl -q";

  for (auto arg : *args) {
    cmd += " " + arg;
  }

  if (!pf_action)
    return true;

  if (verbose_pfctl) {
    log(MessageType::MSG_INFO, cmd);
  }

  high_resolution_clock::time_point t1 = high_resolution_clock::now();
  fp = popen(cmd.c_str(), "r");
  if (!fp) {
    log(MessageType::MSG_CRIT,
        fmt::sprintf("pfctl: '%s' message: can't spawn process", cmd));
    return false;
  }

  if (lines != NULL) {
    string strbuffer;
    while (fgets(buffer, 1024, fp) != NULL) {
      strbuffer.append(buffer);
    }
    istringstream istrbuffer(strbuffer);
    for (string line; getline(istrbuffer, line);) {
      lines->push_back(line);
    }
  }
  int status = pclose(fp);
  ret = WEXITSTATUS(status);
  high_resolution_clock::time_point t2 = high_resolution_clock::now();
  auto duration = duration_cast<milliseconds>(t2 - t1).count();
  log(MessageType::MSG_DEBUG,
      fmt::sprintf("pfctl: '%s' exit_code: %d time: %dms", cmd, ret, duration));

  if (ret != 0)
    return false;
  return true;
}

bool pf_table_add(string *table, set<string> *addresses) {
  if (addresses->size() == 0)
    return true;
  vector<string> cmd;
  cmd.push_back("-t");
  cmd.push_back(*table);
  cmd.push_back("-T");
  cmd.push_back("add");
  for (auto address : *addresses) {
    cmd.push_back(address);
  }
  return pfctl_run_command(&cmd, NULL);
}

bool pf_table_del(string *table, set<string> *addresses) {
  if (addresses->size() == 0)
    return true;
  vector<string> cmd;
  cmd.push_back("-t");
  cmd.push_back(*table);
  cmd.push_back("-T");
  cmd.push_back("del");
  for (auto address : *addresses) {
    cmd.push_back(address);
  }
  return pfctl_run_command(&cmd, NULL);
}

bool pf_kill_src_nodes_to(string *table, string *address, bool with_states) {
  vector<string> cmd;
  cmd.push_back("-K");
  cmd.push_back("table");
  cmd.push_back("-K");
  cmd.push_back(*table);
  cmd.push_back("-K");
  cmd.push_back("dsthost");
  cmd.push_back("-K");
  cmd.push_back(*address);
  if (with_states) {
    cmd.push_back("-K");
    cmd.push_back("kill");
    cmd.push_back("-K");
    cmd.push_back("rststates");
  }

  return pfctl_run_command(&cmd, NULL);
}

bool pf_kill_states_to_rdr(string *table, string *address) {
  vector<string> cmd;
  cmd.push_back("-k");
  cmd.push_back("table");
  cmd.push_back("-k");
  cmd.push_back(*table);
  cmd.push_back("-k");
  cmd.push_back("rdrhost");
  cmd.push_back("-k");
  cmd.push_back(*address);
  cmd.push_back("-k");
  cmd.push_back("kill");
  cmd.push_back("-k");
  cmd.push_back("rststates");

  return pfctl_run_command(&cmd, NULL);
}

bool pf_get_table(string *table, set<string> *result) {
  vector<string> cmd;
  cmd.push_back("-t");
  cmd.push_back(*table);
  cmd.push_back("-T");
  cmd.push_back("show");

  vector<string> out;
  bool ret = pfctl_run_command(&cmd, &out);
  if (!ret) {
    // The first operation on testtool startup is checking if LB Node is already
    // in LB Pool's pf table. If the table does not exist, all subsequent
    // operations will fail until something is added to table. Therefore create
    // a table* and fail getting the table only if that creation fails.
    vector<string> create_cmd;
    create_cmd.push_back("-t");
    create_cmd.push_back(*table);
    create_cmd.push_back("-T");
    create_cmd.push_back("add");
    bool create_ret = pfctl_run_command(&create_cmd, &out);
    if (!create_ret) {
      return false;
    }
  }
  boost::system::error_code ec;
  if (verbose_pfctl) {
    log(MessageType::MSG_INFO, "pfctl: IP addresses in table");
  }
  for (auto line : out) {
    boost::trim(line);
    boost::asio::ip::address::from_string(line, ec);
    if (ec)
      log(MessageType::MSG_CRIT,
          fmt::sprintf("pfctl: Not an IP Address '%s'", line));
    else {
      if (verbose_pfctl) {
        log(MessageType::MSG_INFO, line);
      }
      result->insert(line);
    }
  }
  return true;
}

// Check if an IP address is in the given table.
bool pf_is_in_table(string *table, string *address, bool *answer) {
  set<string> lines;

  *answer = false;
  bool ret = pf_get_table(table, &lines);
  if (!ret) {
    return false;
  }

  for (auto line : lines) {
    if (line == *address)
      *answer = true;
  }

  return true;
}

/// Equalizes traffic to all Nodes of LB Pool.
///
/// Remove src_nodes to all IPs in the given table apart from the specified
/// ones. States of existing connections will not be killed, only src_nodes.
bool pf_table_rebalance(string *table, set<string> *skip_addresses) {
  bool ret;
  set<string> addresses;
  ret = pf_get_table(table, &addresses);
  if (!ret) {
    return false;
  }

  for (auto address : addresses) {
    if (skip_addresses->find(address) == skip_addresses->end()) {
      ret = pf_kill_src_nodes_to(table, &address, false);
      if (!ret) {
        return false;
      }
    }
  }
  return true;
}

bool pf_sync_table(string table, SyncedLbNode *synced_lb_nodes) {
  set<string> cur_set;
  set<string> want_set;

  if (!pf_get_table(&table, &cur_set))
    return false;

  for (int i = 0; i < MAX_NODES; i++) {
    for (int proto = 0; proto < 2; proto++) {
      if (strlen(synced_lb_nodes[i].ip_address[proto]) &&
          synced_lb_nodes[i].state == LbNodeState::STATE_UP) {
        want_set.insert(string(synced_lb_nodes[i].ip_address[proto]));
      }
    }
  }

  // Prepare nodes to add and remove
  std::set<string> to_add;
  std::set_difference(want_set.begin(), want_set.end(), cur_set.begin(),
                      cur_set.end(), std::inserter(to_add, to_add.end()));

  std::set<string> to_del;
  std::set_difference(cur_set.begin(), cur_set.end(), want_set.begin(),
                      want_set.end(), std::inserter(to_del, to_del.end()));

  // Remove unwanted LB Nodes from table. This prevents any future connections
  // to be balanced to them but does not affect existing connections.
  if (!pf_table_del(&table, &to_del))
    return false;

  // This used to be done with iteration over std::set_difference but we need to
  // keep information about killing states, so iterate manually instead and
  // check against to_del.
  for (int i = 0; i < MAX_NODES; i++) {
    bool with_states =
        !(synced_lb_nodes[i].admin_state == LbNodeAdminState::STATE_DRAIN);

    for (int proto = 0; proto < 2; proto++) {
      if (strlen(synced_lb_nodes[i].ip_address[proto]) == 0)
        continue;

      string lb_node_ip_address(synced_lb_nodes[i].ip_address[proto]);

      // Don't delete things not include in to_del.
      if (to_del.count(lb_node_ip_address) == 0)
        continue;

      // Kill src_nodes. And linked states if necessary.
      pf_kill_src_nodes_to(&table, &lb_node_ip_address, with_states);

      if (with_states) {
        // Kill unlinked states if necessary.
        pf_kill_states_to_rdr(&table, &lb_node_ip_address);

        // Kill nodes again, there might be some which were created after last
        // kill due to belonging to states with deferred src_nodes. See
        // TECH-6711 and around.
        pf_kill_src_nodes_to(&table, &lb_node_ip_address, true);
      }
    }
  }

  // Add wanted nodes to table
  if (!pf_table_add(&table, &to_add))
    return false;

  // Rebalance table if new hosts are added. Kill src_nodes to old entries
  if (to_add.size())
    pf_table_rebalance(&table, &to_add);

  return true;
}
