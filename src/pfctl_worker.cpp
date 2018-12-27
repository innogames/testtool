//
// Testtool - PF Control Worker
//
// Copyright (c) 2018 InnoGames GmbH
//

#include <boost/interprocess/ipc/message_queue.hpp>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <iostream>
#include <signal.h>
#include <string.h>
#include <sys/types.h>

#include "msg.h"
#include "pfctl.h"
#include "pfctl_worker.h"

using namespace std;
using namespace boost::interprocess;
using namespace boost::posix_time;

extern pid_t parent_pid;
extern pid_t worker_pid;
bool running;

void worker_signal_handler(int signum) {
  switch (signum) {
  case SIGTERM:
    // Gracefully terminate worker loop
    running = false;
    break;
  }
}

bool pfctl_worker_loop(message_queue *mq) {
  pfctl_msg msg;
  message_queue::size_type recvd_size;
  unsigned int priority;
  bool mq_success;

  log(MSG_INFO, "pfctl_worker: entering worker loop");

  running = true;
  while (running) {
    // Normal receive won't stop blocking when the queue
    // is removed on master process.
    mq_success = false;
    try {
      // A timed_receive will timeout after a moment so that
      // this loop can run again and check if running was changed.
      // That would be done by kill signal. Then this worker
      // can gracefully terminate.
      ptime delay = microsec_clock::universal_time() + millisec(1000);
      mq_success = mq->timed_receive(&msg, sizeof(pfctl_msg), recvd_size,
                                     priority, delay);
    } catch (const exception &ex) {
      log(MSG_CRIT,
          fmt::sprintf("pfctl_worker: exception while receiving from queue %s",
                       ex.what()));
      return false;
    }

    // Check if master process is still alive.
    if (getppid() != parent_pid) {
      log(MSG_CRIT, fmt::sprintf("pfctl_worker: parent died"));
      return false;
    }

    // If no message was received, try to receive another one.
    if (!mq_success) {
      continue;
    }

    assert(recvd_size == sizeof(pfctl_msg));

    log(MSG_INFO, fmt::sprintf("lbpool: %s sync: start pf_table: %s",
                               msg.pool_name, msg.table_name));

    // Decode the message
    string table_name(msg.table_name);
    set<string> wanted_addresses;

    for (int i = 0; i < MAX_NODES * 2; i++) {
      string wanted_address(msg.wanted_addresses[i]);
      if (wanted_address.length())
        wanted_addresses.insert(wanted_address);
    }

    // Measure total time of all pfctl operations.
    // Don't use std::chrono, there is a conflict with boost.
    chrono::high_resolution_clock::time_point t1 =
        chrono::high_resolution_clock::now();
    pf_sync_table(table_name, wanted_addresses);
    chrono::high_resolution_clock::time_point t2 =
        chrono::high_resolution_clock::now();
    auto duration =
        chrono::duration_cast<chrono::milliseconds>(t2 - t1).count();
    log(MSG_INFO,
        fmt::sprintf("lbpool: %s sync: finish pf_table: %s time: %dms",
                     msg.pool_name, msg.table_name, duration));
  }
  return true;
}

message_queue *new_pfctl_queue() {
  message_queue *rmq = NULL;

  try {
    static message_queue mq(create_only, "pfctl", QUEUE_LEN, sizeof(pfctl_msg));
    rmq = &mq;
  } catch (const runtime_error &ex) {
    // speciffic handling for runtime_error
    log(MSG_INFO,
        fmt::sprintf("testtool: unable to create queue %s", ex.what()));
  } catch (const exception &ex) {
    // std::runtime_error which is handled explicitly
    log(MSG_INFO,
        fmt::sprintf("testtool: unable to create queue %s", ex.what()));
  }
  return rmq;
}

message_queue *attach_pfctl_queue() {
  message_queue *rmq = NULL;
  while (true) {
    try {
      static message_queue mq(open_only, "pfctl");
      rmq = &mq;
    } catch (const interprocess_exception &ex) {
      if (ex.get_error_code() == 7) {
        // Awaiting master process to create the queue
        usleep(100000);
        continue;
      } else {
        log(MSG_INFO, fmt::sprintf("pfctl_worker: Interprocess Exception while "
                                   "waiting for queue %d",
                                   ex.get_error_code()));
      }
    } catch (const runtime_error &ex) {
      // speciffic handling for runtime_error
      log(MSG_INFO,
          fmt::sprintf("pfctl_worker: Exception while waiting for queue %s",
                       ex.what()));
    } catch (const exception &ex) {
      // std::runtime_error which is handled explicitly
      log(MSG_INFO,
          fmt::sprintf("pfctl_worker: Exception while waiting for queue %s",
                       ex.what()));
    }
    break;
  }

  return rmq;
}

bool send_message(message_queue *mq, string pool_name, string table_name,
                  set<LbNode *> lb_nodes) {
  pfctl_msg msg;

  memset(&msg, 0, sizeof(msg));
  strncpy(msg.pool_name, pool_name.c_str(), sizeof(msg.pool_name));
  strncpy(msg.table_name, table_name.c_str(), sizeof(msg.table_name));
  int node_index = 0;
  assert(lb_nodes.size() < MAX_NODES);
  for (auto node : lb_nodes) {
    if (!node->ipv4_address.empty()) {
      strncpy(msg.wanted_addresses[node_index], node->ipv4_address.c_str(),
              ADDR_LEN);
      node_index++;
    }
    if (!node->ipv4_address.empty()) {
      strncpy(msg.wanted_addresses[node_index], node->ipv6_address.c_str(),
              ADDR_LEN);
      node_index++;
    }
  }
  return mq->try_send(&msg, sizeof(pfctl_msg), 0);
}

message_queue *start_pfctl_worker() {
  pid_t pid;

  // The queue must be opened in both processes and accessed by name.
  // It must not be created before forking!
  message_queue::remove("pfctl");
  pid = fork();
  if (pid == 0) {
    // Child process
    signal(SIGTERM, worker_signal_handler);
    setproctitle("%s", "pfctl worker");
    message_queue *mq = attach_pfctl_queue();
    if (mq == NULL) {
      log(MSG_CRIT, "pfctl_worker: unable to attach to message queue");
      exit(EXIT_FAILURE);
    }
    if (pfctl_worker_loop(mq)) {
      log(MSG_INFO, fmt::sprintf("pfctl_worker: worker loop finished"));
      message_queue::remove("pfctl");
      exit(EXIT_SUCCESS);
    } else {
      // Detailed error message was printed in worker loop
      message_queue::remove("pfctl");
      exit(EXIT_FAILURE);
    }
  } else if (pid == -1) {
    log(MSG_CRIT, "testtool: unable to fork, terminating!");
    exit(EXIT_FAILURE);
  } else {
    // Parent process
    worker_pid = pid;
    message_queue *mq = new_pfctl_queue();
    if (mq == NULL) {
      log(MSG_CRIT, "testtool: unable to create message queue");
      return NULL;
    }
    return mq;
  }
}

void stop_pfctl_worker() {
  log(MSG_INFO, "testtool: stopping pfctl worker");
  kill(worker_pid, 15);
  message_queue::remove("pfctl");
}
