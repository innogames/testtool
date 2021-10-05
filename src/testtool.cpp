//
// Testtool - Generals
//
// Copyright (c) 2018 InnoGames GmbH
//

#define FMT_HEADER_ONLY

#include <boost/interprocess/ipc/message_queue.hpp>
#include <event2/event-config.h>
#include <event2/util.h>
#include <event2/visibility.h>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <nlohmann/json.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

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
using json = nlohmann::json;

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

static void signal_handler(evutil_socket_t fd, short event, void *arg) {
  int signal = event_get_signal((struct event *)arg);

  switch (signal) {
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
  log(MessageType::MSG_INFO, "Reloading downtime list.");

  json config;
  std::ifstream config_file(config_file_name);
  config_file >> config;
  config_file.close();

  // Compare new config against the old one, start downtimes if necessary.
  // Check the whole path, elements might be missing if somebody changed
  // things in Serveradmin. Don't crash if keys are missing.
  for (const auto &lb_pool : lb_pools) {
    auto pool_config = config.value(lb_pool.first, nlohmann::json{});
    if (!pool_config.empty()) {
      for (const auto &lb_node : lb_pool.second->nodes) {
        auto pool_nodes = pool_config.value("nodes", nlohmann::json{});
        auto node_config = pool_nodes.value(lb_node->name, nlohmann::json{});
        if (!node_config.empty()) {
          lb_node->change_downtime(
              safe_get<string>(node_config, "state", "online"));
        } else {
          log(MessageType::MSG_INFO,
              fmt::sprintf("Can't find LB Node '%s' in new config",
                           lb_node->name));
        }
      }
    } else {
      log(MessageType::MSG_INFO,
          fmt::sprintf("Can't find LB Pool '%s' in new config", lb_pool.first));
    }
  }
}

/// Loads pools from given configuration file.
void TestTool::load_config() {
  log(MessageType::MSG_INFO, "Loading configration file  " + config_file_name);

  json config;
  std::ifstream config_file(config_file_name);
  config_file >> config;
  config_file.close();

  for (const auto &lb_pool : config.items()) {
    string name = lb_pool.key();
    try {
      LbPool *new_lbpool = NULL;
      new_lbpool = new LbPool(name, lb_pool.value(), &lb_pools);
      lb_pools[new_lbpool->name] = new_lbpool;
    } catch (NotLbPoolException ex) {
      // Nothing to do, just ignore it
      log(MessageType::MSG_INFO,
          fmt::sprintf("lbpool: %s state: not created message: %s", name,
                       ex.what()));
    }
  }
}

/// Schedules healthchecks on all lbnodes.
void healthcheck_scheduler_callback(evutil_socket_t fd, short what, void *arg) {
  // Make compiler happy.
  (void)(fd);
  (void)(what);

  try {
    ((TestTool *)arg)->schedule_healthchecks();
  } catch (const HealthcheckSchedulingException &e) {
    log(MessageType::MSG_CRIT,
        fmt::sprintf("testtool: scheduling a check failed: %s, terminating",
                     e.what()));
    event_base_loopbreak(eventBase);
  }
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
    log(MessageType::MSG_CRIT, "testtool: pfctl worker died");
    event_base_loopbreak(eventBase);
  } else {
    // Worker exited normally, status is its exit code
    switch (status) {
    case EXIT_FAILURE:
      log(MessageType::MSG_CRIT, "testtool: pfctl worked died with error code");
      event_base_loopbreak(eventBase);
      break;
    case EXIT_SUCCESS:
      log(MessageType::MSG_INFO, "testtool: pfclt worker terminated normally");
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

  ofstream status_file("/var/run/iglb/lbpools_state.json.new",
                       ios_base::out | ios_base::trunc);

  gettimeofday(&tv, NULL);
  tm = localtime(&tv.tv_sec);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S\0", tm);

  nlohmann::json lb_pools_status;

  // Iterate over all VIPs and write status of each one to the file.
  // NOTE: Syntax uses old data model due to Nagios parsing.
  for (const auto &lb_pool : lb_pools) {
    nlohmann::json lb_nodes_status;

    for (const auto &lb_node : lb_pool.second->nodes) {
      lb_nodes_status[lb_node->name] = {
          {"state", lb_node->is_up_string()},
          {"route_network", lb_node->route_network},
          {"ipv4_address", lb_node->ipv4_address},
          {"ipv6_address", lb_node->ipv6_address},
      };
    }

    lb_pools_status[lb_pool.first] = {
        {"route_network", lb_pool.second->route_network},
        {"nodes_alive", lb_pool.second->count_up_nodes()},
        {"nodes", lb_nodes_status},
        {"state", lb_pool.second->get_state_string()},
        {"ipv4_address", lb_pool.second->ipv4_address},
        {"ipv6_address", lb_pool.second->ipv6_address},
    };
  }

  // Make the file marginally readable for humans.
  status_file << setw(4) << lb_pools_status << std::endl;

  if (status_file.good()) {
    rename("/var/run/iglb/lbpools_state.json.new",
           "/var/run/iglb/lbpools_state.json");
  } else {
    log(MessageType::MSG_CRIT,
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

  // Dump the status to a file every 1 seconds.
  // We could also do it every time something changes via LbPool::pool_logic
  // but that could be way too often if a lot of things change.
  struct timeval dump_status_interval;
  dump_status_interval.tv_sec = 1;
  dump_status_interval.tv_usec = 0;
  struct event *dump_status_event =
      event_new(eventBase, -1, EV_PERSIST, dump_status_callback, this);
  event_add(dump_status_event, &dump_status_interval);
}

void init_libevent() {
  eventBase = event_base_new();
  log(MessageType::MSG_INFO,
      fmt::sprintf("libevent method: %s", event_base_get_method(eventBase)));
}

void finish_libevent() { event_base_free(eventBase); }

int init_libssl() {
  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_all_algorithms();

  log(MessageType::MSG_INFO,
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

  log(MessageType::MSG_INFO, "Initializing various stuff...");

  parent_pid = getpid();
  pfctl_mq = start_pfctl_worker();
#ifdef __FreeBSD__
  setproctitle("%s", "main process");
#endif

  if (!init_libssl()) {
    log(MessageType::MSG_CRIT, "Unable to initialise OpenSSL, terminating!");
    exit(EXIT_FAILURE);
  }
  init_libevent();

  struct event *ev_sigint =
      evsignal_new(eventBase, SIGINT, signal_handler, event_self_cbarg());
  evsignal_add(ev_sigint, NULL);

  struct event *ev_sigterm =
      evsignal_new(eventBase, SIGTERM, signal_handler, event_self_cbarg());
  evsignal_add(ev_sigterm, NULL);

  struct event *ev_sighup =
      evsignal_new(eventBase, SIGHUP, signal_handler, event_self_cbarg());
  evsignal_add(ev_sighup, NULL);

  struct event *ev_sigpipe =
      evsignal_new(eventBase, SIGPIPE, signal_handler, event_self_cbarg());
  evsignal_add(ev_sigpipe, NULL);

  struct event *ev_sigusr1 =
      evsignal_new(eventBase, SIGUSR1, signal_handler, event_self_cbarg());
  evsignal_add(ev_sigusr1, NULL);

  if (!Healthcheck_ping::initialize()) {
    log(MessageType::MSG_CRIT,
        "Unable to initialize Healthcheck_ping, terminating!");
    exit(EXIT_FAILURE);
  }

  auto tool = new TestTool(config_file_name);
  tool->load_config();

  tool->setup_events();
  log(MessageType::MSG_INFO, "Entering the main loop...");
  event_base_dispatch(eventBase);
  log(MessageType::MSG_INFO, "Left the main loop.");

  delete tool;
  log(MessageType::MSG_INFO, "Stopping testtool");

  Healthcheck_ping::destroy();

  finish_libevent();
  finish_libssl();
  stop_pfctl_worker();
  log(MessageType::MSG_INFO, "Waiting for pfctl worker");
  wait(NULL);
  log(MessageType::MSG_INFO, "Testtool finished, bye!");
}
