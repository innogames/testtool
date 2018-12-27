//
// Testtool - Generals
//
// Copyright (c) 2018 InnoGames GmbH
//

#include <boost/interprocess/ipc/message_queue.hpp>
#include <event2/event.h>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "config.h"
#include "healthcheck.h"
#include "healthcheck_ping.h"
#include "lb_node.h"
#include "lb_pool.h"
#include "msg.h"
#include "pfctl_worker.h"
#include "testtool.h"

using namespace std;
using namespace boost::interprocess;

// Global variables, some are exported to other modules.
struct event_base *eventBase = NULL;
SSL_CTX *sctx = NULL;
int verbose = 0;
int verbose_pfctl = 0;
bool pf_action = true;
bool check_downtimes = false; // Whether downtimes should be reloaded.
message_queue *pfctl_mq;
pid_t parent_pid;
pid_t worker_pid;

void signal_handler(int signum) {
  switch (signum) {
  case SIGPIPE:
    // This will happen on failed SSL checks.
    break;
  case SIGTERM:
  case SIGINT:
  // Config reloading will be added later. For now terminating
  // the program is fine as watchdog script will re-launch it.
  case SIGHUP:
    event_base_loopbreak(eventBase);
    break;
  case SIGUSR1:
    check_downtimes = true;
  }
}

/// Loads downtime list and dowtime specified nodes in a specified lbpool.
///
/// A downtime means that:
/// - The specified nodes are immediately marked as "down" with all the
/// consequences.
/// - They will not be checked anymore until the downtime is removed.
/// - After the downtime is removed, they will be subject to normal healthchecks
/// before getting traffic again.
void TestTool::load_downtimes() {
  log(MSG_INFO, "Reloading downtime list.");

  YAML::Node downtimes;
  downtimes = YAML::LoadFile(config_file_name);

  // Compare new config against the old one, start downtimes if necessary.
  for (auto lb_pool : lb_pools) {
    for (auto lb_node : lb_pool.second->nodes) {

      // Check the whole path, elements might be missing
      // if somebody changed things in Serveradmin.
      if (downtimes[lb_pool.first] &&
          downtimes[lb_pool.first]["nodes"][lb_node->name] &&
          downtimes[lb_pool.first]["nodes"][lb_node->name]["downtime"]
              .as<bool>()) {
        lb_node->start_downtime();
      } else {
        lb_node->end_downtime();
      }
    }
  }
}

/// Loads pools from given configuration file.
void TestTool::load_config() {
  YAML::Node config;
  log(MSG_INFO, "Loading configration file  " + config_file_name);

  config = YAML::LoadFile(config_file_name);

  for (auto lb_pool : config) {
    string name = lb_pool.first.as<std::string>();
    try {
      LbPool *new_lbpool = NULL;
      new_lbpool = new LbPool(name, lb_pool.second, &lb_pools);
      lb_pools[new_lbpool->name] = new_lbpool;
    } catch (NotLbPoolException ex) {
      // Nothing to do, just ignore it
      log(MSG_INFO, fmt::sprintf("lbpool: %s state: not created message: %s",
                                 name, ex.what()));
    }
  }
}

/// Schedules healthchecks on all lbnodes.
void healthcheck_scheduler_callback(evutil_socket_t fd, short what, void *arg) {
  // Make compiler happy.
  (void)(fd);
  (void)(what);

  ((TestTool *)arg)->schedule_healthchecks();
}

void TestTool::schedule_healthchecks() {
  struct timespec now;

  if (check_downtimes) {
    load_downtimes();
    check_downtimes = false;
  }

  // Get time once and assume all checks started at this time.
  clock_gettime(CLOCK_MONOTONIC, &now);

  // Iterate over all lbpools and schedule healthchecks.
  for (auto &lbpool : lb_pools) {
    lbpool.second->schedule_healthchecks(&now);
  }
}

/// Parses the results of healthchecks for all lbpools.
void healthcheck_finalizer_callback(evutil_socket_t fd, short what, void *arg) {
  // Make compiler happy.
  (void)(fd);
  (void)(what);

  ((TestTool *)arg)->finalize_healthchecks();
}

void TestTool::finalize_healthchecks() {
  // Iterate over all lbpools parse healthcheck results.
  for (auto &lbpool : lb_pools) {
    lbpool.second->finalize_healthchecks();
  }
}

/// Checks if pfctl worker is still alive.
void worker_check_callback(evutil_socket_t fd, short what, void *arg) {
  // Make compiler happy.
  (void)(fd);
  (void)(what);

  int status;
  pid_t result = waitpid(worker_pid, &status, WNOHANG);
  if (result == 0) {
    // Worker still working.
  } else if (result == -1) {
    // Unable to get worker status
    log(MSG_CRIT, "testtool: pfctl worker died");
    event_base_loopbreak(eventBase);
  } else {
    // Worker exited normally, status is its exit code
    switch (status) {
    case EXIT_FAILURE:
      log(MSG_CRIT, "testtool: pfctl worked died with error code");
      event_base_loopbreak(eventBase);
      break;
    case EXIT_SUCCESS:
      log(MSG_INFO, "testtool: pfclt worker terminated normally");
      break;
    }
  }
}

void dump_status_callback(evutil_socket_t fd, short what, void *arg) {
  // Make compiler happy.
  (void)(fd);
  (void)(what);

  ((TestTool *)arg)->dump_status();
}

TestTool::TestTool(string config_file_name) {
  this->config_file_name = config_file_name;
}

/// Dumps pools with no nodes to serve the traffic.
void TestTool::dump_status() {
  char buf[128];
  struct timeval tv;
  struct tm *tm;

  ofstream status_file("/var/log/testtool.status.new",
                       ios_base::out | ios_base::trunc);

  gettimeofday(&tv, NULL);
  tm = localtime(&tv.tv_sec);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S\0", tm);

  status_file << "Testtool's log, stardate " << buf
              << ". Our mission is lbpools' high availability." << endl;

  // Iterate over all VIPs and write status of each one to the file.
  // NOTE: Syntax uses old data model due to Nagios parsing.
  for (auto &lb_pool : lb_pools) {
    status_file << "lbpool: " << lb_pool.second->pf_name;
    status_file << " nodes_alive: " << lb_pool.second->count_up_nodes();
    status_file << " backup_pool: " << lb_pool.second->get_backup_pool_state();
    status_file << endl;
  }

  status_file.close();
  if (status_file.good()) {
    rename("/var/log/testtool.status.new", "/var/log/testtool.status");
  } else {
    log(MSG_CRIT,
        fmt::sprintf("Could not write status file, will retry next time."));
  }
}

void TestTool::setup_events() {
  // Sleep time for the main loop
  //   1 000 μs =   1ms = 1000/s
  //  10 000 μs =  10ms =  100/s
  // 100 000 μs = 100ms =   10/s

  // Run the healthcheck scheduler multiple times per second.
  struct timeval healthcheck_scheduler_interval;
  healthcheck_scheduler_interval.tv_sec = 0;
  healthcheck_scheduler_interval.tv_usec = 100000; // 0.1s
  struct event *healthcheck_scheduler_event = event_new(
      eventBase, -1, EV_PERSIST, healthcheck_scheduler_callback, this);
  event_add(healthcheck_scheduler_event, &healthcheck_scheduler_interval);

  // Run the healthcheck finalizer multiple times per second.
  // This is for special healthchecks like ping which can't handle
  // its own timeouts.
  struct timeval healthcheck_finalizer_interval;
  healthcheck_finalizer_interval.tv_sec = 0;
  healthcheck_finalizer_interval.tv_usec = 100000; // 0.1s
  struct event *healthcheck_finalizer_event = event_new(
      eventBase, -1, EV_PERSIST, healthcheck_finalizer_callback, this);
  event_add(healthcheck_finalizer_event, &healthcheck_finalizer_interval);

  // Check if pfctl worker thread is still alive.
  struct timeval worker_check_interval;
  worker_check_interval.tv_sec = 1;
  worker_check_interval.tv_usec = 0; // Just once a second.
  struct event *worker_check_event =
      event_new(eventBase, -1, EV_PERSIST, worker_check_callback, this);
  event_add(worker_check_event, &worker_check_interval);

  // Dump the status to a file every 5 seconds
  struct timeval dump_status_interval;
  dump_status_interval.tv_sec = 5;
  dump_status_interval.tv_usec = 0;
  struct event *dump_status_event =
      event_new(eventBase, -1, EV_PERSIST, dump_status_callback, this);
  event_add(dump_status_event, &dump_status_interval);
}

void init_libevent() {
  eventBase = event_base_new();
  log(MSG_INFO,
      fmt::sprintf("libevent method: %s", event_base_get_method(eventBase)));
}

void finish_libevent() { event_base_free(eventBase); }

int init_libssl() {
  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_all_algorithms();

  log(MSG_INFO,
      fmt::sprintf("OpenSSL version: %s", SSLeay_version(SSLEAY_VERSION)));

  sctx = SSL_CTX_new(TLSv1_2_client_method());
  if (!sctx) {
    return false;
  }

  SSL_CTX_set_verify(sctx, SSL_VERIFY_NONE, NULL);

  return true;
}

void finish_libssl() {
  EVP_cleanup();
  ERR_free_strings();
  SSL_CTX_free(sctx);
}

void usage() {
  cout << "Hi, I'm testtool-ng and my arguments are:" << endl;
  cout << " -f  - specify an alternate configuration file to load" << endl;
  cout << " -h  - helps you with this helpful help message" << endl;
  cout << " -n  - do not perform any pfctl actions" << endl;
  cout << " -p  - display pfctl commands even if skipping pfctl actions"
       << endl;
  cout << " -v  - be verbose - display loaded lbpools list" << endl;
  cout << " -vv - be more verbose - display every scheduling of a test and "
          "test result"
       << endl;
}

int main(int argc, char *argv[]) {
  start_logging();

  srand(time(NULL));
  ;

  string config_file_name = "/etc/iglb/lbpools.json";

  int opt;
  while ((opt = getopt(argc, argv, "hnpvf:")) != -1) {
    switch (opt) {
    case 'f':
      config_file_name = optarg;
      break;
    case 'n':
      pf_action = false;
      break;
    case 'p':
      verbose_pfctl++;
      break;
    case 'v':
      verbose++;
      break;
    case 'h':
      usage();
      exit(EXIT_SUCCESS);
      break;
    }
  }

  log(MSG_INFO, "Initializing various stuff...");

  parent_pid = getpid();
  pfctl_mq = start_pfctl_worker();
  setproctitle("%s", "main process");

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGHUP, signal_handler);
  signal(SIGPIPE, signal_handler);
  signal(SIGUSR1, signal_handler);

  if (!init_libssl()) {
    log(MSG_CRIT, "Unable to initialise OpenSSL, terminating!");
    exit(EXIT_FAILURE);
  }
  init_libevent();

  if (!Healthcheck_ping::initialize()) {
    log(MSG_CRIT, "Unable to initialize Healthcheck_ping, terminating!");
    exit(EXIT_FAILURE);
  }

  auto tool = new TestTool(config_file_name);
  tool->load_config();

  tool->setup_events();
  log(MSG_INFO, "Entering the main loop...");
  event_base_dispatch(eventBase);
  log(MSG_INFO, "Left the main loop.");

  delete tool;
  log(MSG_INFO, "Stopping testtool");

  Healthcheck_ping::destroy();

  finish_libevent();
  finish_libssl();
  stop_pfctl_worker();
  log(MSG_INFO, "Waiting for pfctl worker");
  wait(NULL);
  log(MSG_INFO, "Testtool finished, bye!");
}
