//
// Testtool - PF Controls
//
// Copyright (c) 2018 InnoGames GmbH
//

#define FMT_HEADER_ONLY

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include "msg.h"
#include "pfctl.h"
#include "pfctl_async.h"

using namespace std;

extern bool pf_action;
extern int verbose_pfctl;

string pfctl_command = "/sbin/pfctl";

// ---------------------------------------------------------------------------
// Synchronous helpers (used at startup before event loop)
// ---------------------------------------------------------------------------

// Run a pfctl command synchronously via fork/exec. Used only at startup.
static bool pfctl_run_command_sync(vector<string> *args, vector<string> *lines) {
  if (!pf_action)
    return true;

  int pipefd[2];
  if (pipe(pipefd) < 0)
    return false;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    vector<const char *> argv;
    argv.push_back(pfctl_command.c_str());
    argv.push_back("-q");
    for (const auto &arg : *args) {
      argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);
    execvp(pfctl_command.c_str(), const_cast<char *const *>(argv.data()));
    _exit(127);
  }

  close(pipefd[1]);
  string out_buf;
  char buf[4096];
  ssize_t n;
  while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
    out_buf.append(buf, n);
  }
  close(pipefd[0]);

  int status;
  waitpid(pid, &status, 0);

  if (lines != nullptr) {
    istringstream stream(out_buf);
    string line;
    while (getline(stream, line)) {
      lines->push_back(line);
    }
  }

  return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// Synchronous pf_is_in_table — called at startup from lb_node.cpp
bool pf_is_in_table(string *table, string *address, bool *answer) {
  *answer = false;

  vector<string> cmd = {"-t", *table, "-T", "show"};
  vector<string> out;
  bool ret = pfctl_run_command_sync(&cmd, &out);
  if (!ret) {
    // Table might not exist yet — create it
    vector<string> create_cmd = {"-t", *table, "-T", "add"};
    vector<string> create_out;
    if (!pfctl_run_command_sync(&create_cmd, &create_out))
      return false;
    return true; // empty table, address not in it
  }

  boost::system::error_code ec;
  for (auto line : out) {
    boost::trim(line);
    boost::asio::ip::make_address(line, ec);
    if (!ec && line == *address) {
      *answer = true;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Async implementation
// ---------------------------------------------------------------------------

static void parse_table_output(const std::vector<std::string> &output,
                               std::set<std::string> &result) {
  boost::system::error_code ec;
  for (auto line : output) {
    boost::trim(line);
    if (line.empty())
      continue;
    boost::asio::ip::make_address(line, ec);
    if (ec) {
      log(MessageType::MSG_CRIT,
          fmt::sprintf("pfctl: Not an IP Address '%s'", line));
    } else {
      if (verbose_pfctl) {
        log(MessageType::MSG_INFO, line);
      }
      result.insert(line);
    }
  }
}

void pf_sync_table_async(PfctlAsync *pfctl_async, std::string table,
                          SyncedLbNode *synced_lb_nodes,
                          PfSyncDoneCallback done_callback) {
  if (!pf_action) {
    done_callback(true);
    return;
  }

  auto *ctx = new PfSyncContext();
  ctx->pfctl_async = pfctl_async;
  ctx->table = std::move(table);
  memcpy(ctx->synced_lb_nodes, synced_lb_nodes, sizeof(ctx->synced_lb_nodes));
  ctx->done_callback = std::move(done_callback);
  ctx->start();
}

void PfSyncContext::start() {
  std::vector<std::string> cmd = {"-t", table, "-T", "show"};
  pfctl_async->run_command(std::move(cmd),
      [this](bool success, std::vector<std::string> output) {
        on_get_table(success, std::move(output));
      });
}

void PfSyncContext::on_get_table(bool success, std::vector<std::string> output) {
  if (!success) {
    // Table may not exist yet — try to create it.
    std::vector<std::string> cmd = {"-t", table, "-T", "add"};
    pfctl_async->run_command(std::move(cmd),
        [this](bool s, std::vector<std::string> o) {
          on_table_created(s, std::move(o));
        });
    return;
  }

  if (verbose_pfctl) {
    log(MessageType::MSG_INFO, "pfctl: IP addresses in table");
  }
  parse_table_output(output, cur_set);
  compute_diff();
}

void PfSyncContext::on_table_created(bool success, std::vector<std::string> output) {
  if (!success) {
    finish(false);
    return;
  }
  // Fresh table — cur_set stays empty.
  compute_diff();
}

void PfSyncContext::compute_diff() {
  // Build want_set from synced_lb_nodes.
  for (int i = 0; i < MAX_NODES; i++) {
    for (int proto = 0; proto < 2; proto++) {
      if (strlen(synced_lb_nodes[i].ip_address[proto]) &&
          synced_lb_nodes[i].wanted_state == LbNodeState::STATE_UP &&
          synced_lb_nodes[i].admin_state == LbNodeAdminState::STATE_ENABLED) {
        want_set.insert(std::string(synced_lb_nodes[i].ip_address[proto]));
        log(MessageType::MSG_INFO,
            fmt::sprintf("pfctl: Wanted node %s",
                         synced_lb_nodes[i].ip_address[proto]));
      }
    }
  }

  // Compute to_add = want_set - cur_set
  std::set_difference(want_set.begin(), want_set.end(),
                      cur_set.begin(), cur_set.end(),
                      std::inserter(to_add, to_add.end()));

  // Compute to_del = cur_set - want_set
  std::set_difference(cur_set.begin(), cur_set.end(),
                      want_set.begin(), want_set.end(),
                      std::inserter(to_del, to_del.end()));

  // Build kill_list from synced_lb_nodes for IPs in to_del.
  for (int i = 0; i < MAX_NODES; i++) {
    bool with_states =
        !(synced_lb_nodes[i].admin_state <= LbNodeAdminState::STATE_DRAIN_SOFT);
    for (int proto = 0; proto < 2; proto++) {
      if (strlen(synced_lb_nodes[i].ip_address[proto]) == 0)
        continue;
      std::string ip(synced_lb_nodes[i].ip_address[proto]);
      if (to_del.count(ip) > 0) {
        kill_list.push_back(KillEntry{ip, with_states});
      }
    }
  }

  if (!to_del.empty()) {
    // Delete unwanted nodes from table.
    std::vector<std::string> cmd = {"-t", table, "-T", "del"};
    for (const auto &addr : to_del) {
      cmd.push_back(addr);
    }
    pfctl_async->run_command(std::move(cmd),
        [this](bool success, std::vector<std::string> output) {
          on_del(success, std::move(output));
        });
  } else {
    do_add();
  }
}

void PfSyncContext::on_del(bool success, std::vector<std::string> output) {
  if (!success) {
    finish(false);
    return;
  }
  kill_index = 0;
  kill_phase = 0;
  kill_next_node();
}

void PfSyncContext::kill_next_node() {
  if (kill_index >= kill_list.size()) {
    do_add();
    return;
  }

  const auto &entry = kill_list[kill_index];
  std::vector<std::string> cmd;

  if (kill_phase == 0) {
    // kill_src_nodes_to
    cmd = {"-K", "table", "-K", table, "-K", "dsthost", "-K", entry.ip};
    if (entry.with_states) {
      cmd.push_back("-K");
      cmd.push_back("kill");
      cmd.push_back("-K");
      cmd.push_back("rststates");
    }
  } else if (kill_phase == 1) {
    // kill_states_to_rdr
    cmd = {"-k", "table", "-k", table, "-k", "rdrhost", "-k", entry.ip,
           "-k", "kill", "-k", "rststates"};
  } else if (kill_phase == 2) {
    // kill_src_nodes_to again with with_states=true (TECH-6711)
    cmd = {"-K", "table", "-K", table, "-K", "dsthost", "-K", entry.ip,
           "-K", "kill", "-K", "rststates"};
  }

  pfctl_async->run_command(std::move(cmd),
      [this](bool success, std::vector<std::string> output) {
        on_kill_step(success, std::move(output));
      });
}

void PfSyncContext::on_kill_step(bool success, std::vector<std::string> output) {
  // Don't fail on kill errors — current code ignores return value in the loop.
  const auto &entry = kill_list[kill_index];

  if (entry.with_states) {
    if (kill_phase == 0) {
      kill_phase = 1;
      kill_next_node();
    } else if (kill_phase == 1) {
      kill_phase = 2;
      kill_next_node();
    } else {
      // kill_phase == 2, done with this entry
      kill_index++;
      kill_phase = 0;
      kill_next_node();
    }
  } else {
    kill_index++;
    kill_phase = 0;
    kill_next_node();
  }
}

void PfSyncContext::do_add() {
  if (!to_add.empty()) {
    std::vector<std::string> cmd = {"-t", table, "-T", "add"};
    for (const auto &addr : to_add) {
      cmd.push_back(addr);
    }
    pfctl_async->run_command(std::move(cmd),
        [this](bool success, std::vector<std::string> output) {
          on_add(success, std::move(output));
        });
  } else {
    finish(true);
  }
}

void PfSyncContext::on_add(bool success, std::vector<std::string> output) {
  if (!success) {
    finish(false);
    return;
  }
  // If we added nodes, rebalance.
  if (!to_add.empty()) {
    start_rebalance();
  } else {
    finish(true);
  }
}

void PfSyncContext::start_rebalance() {
  std::vector<std::string> cmd = {"-t", table, "-T", "show"};
  pfctl_async->run_command(std::move(cmd),
      [this](bool success, std::vector<std::string> output) {
        on_rebalance_get_table(success, std::move(output));
      });
}

void PfSyncContext::on_rebalance_get_table(bool success, std::vector<std::string> output) {
  if (!success) {
    finish(false);
    return;
  }

  std::set<std::string> all_addresses;
  parse_table_output(output, all_addresses);

  // Build rebalance_list: addresses NOT in to_add.
  for (const auto &addr : all_addresses) {
    if (to_add.find(addr) == to_add.end()) {
      rebalance_list.push_back(addr);
    }
  }

  rebalance_index = 0;
  kill_next_rebalance_entry();
}

void PfSyncContext::kill_next_rebalance_entry() {
  if (rebalance_index >= rebalance_list.size()) {
    finish(true);
    return;
  }

  const auto &addr = rebalance_list[rebalance_index];
  std::vector<std::string> cmd = {"-K", "table", "-K", table,
                                   "-K", "dsthost", "-K", addr};
  pfctl_async->run_command(std::move(cmd),
      [this](bool success, std::vector<std::string> output) {
        on_rebalance_kill(success, std::move(output));
      });
}

void PfSyncContext::on_rebalance_kill(bool success, std::vector<std::string> output) {
  rebalance_index++;
  kill_next_rebalance_entry();
}

void PfSyncContext::finish(bool success) {
  auto cb = std::move(done_callback);
  delete this;
  cb(success);
}
