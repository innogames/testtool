#include <iostream>
#include <fstream>
#include <list>
#include <sstream>
#include <map>
#include <vector>
#include <set>
#include <fmt/format.h>
#include <yaml-cpp/yaml.h>
#include <boost/interprocess/ipc/message_queue.hpp>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#include <event2/event.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "config.h"
#include "msg.h"

#include "lb_pool.h"
#include "lb_node.h"
#include "healthcheck.h"
#include "healthcheck_ping.h"
#include "testtool.h"
#include "pfctl_worker.h"

using namespace std;
using namespace boost::interprocess;

/* Global variables, some are exported to other modules. */
struct event_base	*eventBase = NULL;
SSL_CTX			*sctx = NULL;
int			 verbose = 0;
int			 verbose_pfctl = 0;
bool			 pf_action = true;
bool			 check_downtimes = false; /* Whether downtimes should be reloaded. */
message_queue		*pfctl_mq;
pid_t			 parent_pid;
pid_t			 worker_pid;

void signal_handler(int signum) {
	switch (signum) {
		case SIGPIPE:
			/* This will happen on failed SSL checks. */
			break;
		case SIGTERM:
		case SIGINT:
		/*
		   Config reloading will be added later. For now terminating
		   the program is fine as watchdog script will re-launch it.
		*/
		case SIGHUP:
			event_base_loopbreak(eventBase);
			break;
		case SIGUSR1:
			check_downtimes = true;
	}
}


/*
   Load downtime list and dowtime specified nodes in a specified lbpool.
   A downtime means that:
   - The specified nodes are immediately marked as "down" with all the consequences.
   - They will not be checked anymore until the downtime is removed.
   - After the downtime is removed, they will be subject to normal healthchecks before getting traffic again.
*/
void TestTool::load_downtimes() {
	log(MSG_INFO, "Reloading downtime list.");

	string line;

	downtimes.clear();

	/* Read all the lbpool-node pairs and store them. */
	ifstream downtime_file("/etc/iglb/testtool_downtimes.conf");
	if (downtime_file) {
		while (getline(downtime_file, line)) {
			downtimes.insert(line);
		}
		downtime_file.close();
	} else {
		log(MSG_INFO, "Could not load downtime list file.");
	}

	/* Iterate over all lbpools and nodes, start downtime for the loaded
	 * ones, end for the ones not in the set. On testtool startup the list
	 * of pools is empty, this loop should just ignore them.
	 */
	for (auto& lbpool : lb_pools) {
		for (auto node : lbpool.second->nodes) {
			if ( downtimes.count(lbpool.second->pf_name + " " + node->address) ) {
				node->start_downtime();
			} else {
				node->end_downtime();
			}
		}
	}
}


/*
   Load pools from given configuration file.
*/
void TestTool::load_config(string config_file) {
	YAML::Node config;
	log(MSG_INFO, "Loading configration file  " + config_file);

        config = YAML::LoadFile(config_file)["lbpools"];

	/*
	 * Load downtimes before loading pools and nodes so that they can
	 * start their operation in desired state.
	 */
	load_downtimes();

	for (
		YAML::const_iterator pool_it = config.begin();
		pool_it != config.end();
		pool_it++
	) {
		// Duplicate LB Pool into IPv4 and IPv6 versions.
		std::vector<string> protos = {"4", "6"};
		for (auto proto : protos ) {
			string name = pool_it->first.as<std::string>() + "_" + proto;
			/*
			 * Ignore LBPools which have no IP address in given address
			 * family or have no ports forwarded - they might be SNAT rules.
			 * Serveradmin gives us evertythng he has seen.
			 */
			try {
				LbPool *new_lbpool = NULL;
				new_lbpool = new LbPool(name, pool_it->second, proto, &downtimes, &lb_pools);
				lb_pools[new_lbpool->name] = new_lbpool;
			}
			catch (NotLbPoolException ex) {
				/* Nothing to do, just ignore it */
				log(MSG_INFO, fmt::sprintf("lbpool: %s %s", name, ex.what()));
			}
		}
	}
}


/*
   This function schedules healthchecks on all lbnodes.
*/
void healthcheck_scheduler_callback(evutil_socket_t fd, short what, void *arg) {
	/* Make compiler happy. */
	(void)(fd);
	(void)(what);

	((TestTool*)arg)->schedule_healthchecks();
}

void TestTool::schedule_healthchecks() {
	struct timespec now;

	if (check_downtimes) {
		load_downtimes();
		check_downtimes = false;
	}

	/* Get time once and assume all checks started at this time. */
	clock_gettime(CLOCK_MONOTONIC, &now);

	/* Iterate over all lbpools and schedule healthchecks. */
	for (auto& lbpool : lb_pools) {
		lbpool.second->schedule_healthchecks(&now);
	}
}

/*
   This function parses the results of healthchecks for all lbpools.
*/
void healthcheck_parser_callback(evutil_socket_t fd, short what, void *arg) {
	/* Make compiler happy. */
	(void)(fd);
	(void)(what);

	((TestTool*)arg)->parse_healthchecks_results();
}

void TestTool::parse_healthchecks_results() {
	/* Iterate over all lbpools parse healthcheck results. */
	for (auto& lbpool: lb_pools) {
		lbpool.second->parse_healthchecks_results();
	}
}


/*
   Check if pfctl worker is still alive.
*/
void worker_check_callback(evutil_socket_t fd, short what, void *arg) {
	/* Make compiler happy. */
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

/*
   Dump status to file. The status consists of:
   - Pools with no nodes to serve the traffic.
*/
void dump_status_callback(evutil_socket_t fd, short what, void *arg) {
	/* Make compiler happy. */
	(void)(fd);
	(void)(what);

	((TestTool*)arg)->dump_status();
}

void TestTool::dump_status() {
	char buf[128];
	struct timeval  tv;
	struct tm      *tm;

	ofstream status_file("/var/log/testtool.status.new",  ios_base::out |  ios_base::trunc);

	gettimeofday(&tv, NULL);
	tm = localtime(&tv.tv_sec);
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S\0", tm);

	status_file << "Testtool's log, stardate " << buf << ". Our mission is lbpools' high availability." << endl;

	/* Iterate over all VIPs and write status of each one to the file.
	   NOTE: Syntax uses old data model due to Nagios parsing. */
	for (auto& lb_pool : lb_pools) {
		status_file << "lbpool: " << lb_pool.second->pf_name;
		status_file << " nodes_alive: " << lb_pool.second->count_up_nodes();
		status_file << " backup_pool: "  << lb_pool.second->get_backup_pool_state();
		status_file << endl;
	}

	status_file.close();
	rename ("/var/log/testtool.status.new", "/var/log/testtool.status");
}

void configure_bgp_callback(evutil_socket_t fd, short what, void *arg) {
	/* Make compiler happy. */
	(void)(fd);
	(void)(what);

	((TestTool*)arg)->configure_bgp();
}

void TestTool::configure_bgp() {

	std::set<string*> ips4_alive_tmp;
	std::set<string*> ips6_alive_tmp;

	for (auto& lb_pool : lb_pools) {
		if (lb_pool.second->state == LbPool::STATE_UP) {
			if (lb_pool.second->proto == "4") {
				ips4_alive_tmp.insert(&lb_pool.second->ip_address);
			}

			if (lb_pool.second->proto == "6") {
				ips6_alive_tmp.insert(&lb_pool.second->ip_address);
			}
		}
	}

	string conffile = "/usr/local/etc/bird_testtool_nets.conf";
	string tmpfile  = conffile + ".new";
	if (ips4_alive_tmp != ips4_alive) {
		ofstream status_file(tmpfile,  ios_base::out |  ios_base::trunc);
		status_file << "testtool_pools = [ 0.0.0.0/32+";
		for (auto ip_alive : ips4_alive_tmp) {
			status_file << ", " << *ip_alive << "/32+ ";
		}
		status_file << " ];" << endl;
		status_file.close();
		// Create real file in "atomic" way, in case BIRD reloads it in the meantime.
		std::rename(tmpfile.c_str(), conffile.c_str());
		ips4_alive = ips4_alive_tmp;
		system("/usr/local/sbin/birdcl configure");
	}

	conffile = "/usr/local/etc/bird6_testtool_nets.conf";
	tmpfile  = conffile + ".new";
	if (ips6_alive_tmp != ips6_alive) {
		ofstream status_file(tmpfile,  ios_base::out |  ios_base::trunc);
		status_file << "testtool_pools = [ ::/128+";
		for (auto ip_alive : ips6_alive_tmp) {
			status_file << ", " << *ip_alive << "/128+ ";
		}
		status_file << " ];" << endl;
		status_file.close();
		// Create real file in "atomic" way, in case BIRD reloads it in the meantime.
		std::rename(tmpfile.c_str(), conffile.c_str());
		ips6_alive = ips6_alive_tmp;
		system("/usr/local/sbin/birdcl6 configure");
	}

}

void TestTool::setup_events() {
	/*
	   Sleep time for the main loop (by the way, using unicode micro sign breaks my vim).
	    1 000 us =   1ms = 1000/s
	   10 000 us =  10ms =  100/s
	  100 000 us = 100ms =   10/s
	 */

	/* Run the healthcheck scheduler multiple times per second. */
	struct timeval healthcheck_scheduler_interval;
	healthcheck_scheduler_interval.tv_sec  = 0;
	healthcheck_scheduler_interval.tv_usec = 100000; // 0.1s
	struct event *healthcheck_scheduler_event = event_new(eventBase, -1, EV_PERSIST, healthcheck_scheduler_callback, this);
	event_add(healthcheck_scheduler_event, &healthcheck_scheduler_interval);

	/* Run the healthcheck result parser multiple times per second. */
	struct timeval healthcheck_parser_interval;
	healthcheck_parser_interval.tv_sec  = 0;
	healthcheck_parser_interval.tv_usec = 100000; // 0.1s
	struct event *healthcheck_parser_event = event_new(eventBase, -1, EV_PERSIST, healthcheck_parser_callback, this);
	event_add(healthcheck_parser_event, &healthcheck_parser_interval);

	/* Check if pfctl worker thread is still alive multiple times per second. */
	struct timeval worker_check_interval;
	worker_check_interval.tv_sec  = 0;
	worker_check_interval.tv_usec = 100000; // 0.1s
	struct event *worker_check_event = event_new(eventBase, -1, EV_PERSIST, worker_check_callback, this);
	event_add(worker_check_event, &worker_check_interval);

	/* Dump the status to a file every 5 seconds */
	struct timeval dump_status_interval;
	dump_status_interval.tv_sec  = 5;
	dump_status_interval.tv_usec = 0;
	struct event *dump_status_event = event_new(eventBase, -1, EV_PERSIST, dump_status_callback, this);
	event_add(dump_status_event, &dump_status_interval);

	/* Configure BGP every 10 seconds */
	struct timeval configure_bgp_interval;
	configure_bgp_interval.tv_sec  = 10;
	configure_bgp_interval.tv_usec = 0;
	struct event *configure_bgp_event = event_new(eventBase, -1, EV_PERSIST, configure_bgp_callback, this);
	event_add(configure_bgp_event, &configure_bgp_interval);

}


void init_libevent() {
	eventBase = event_base_new();
	log(MSG_INFO, fmt::sprintf("libevent method: %s", event_base_get_method(eventBase)));
}


void finish_libevent() {
	event_base_free(eventBase);
}


int init_libssl() {
	SSL_library_init ();
	SSL_load_error_strings ();
	OpenSSL_add_all_algorithms ();

	log(MSG_INFO, fmt::sprintf("OpenSSL version: %s", SSLeay_version (SSLEAY_VERSION)));

	sctx = SSL_CTX_new (SSLv23_client_method ());
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
	cout << " -p  - display pfctl commands even if skipping pfctl actions" << endl;
	cout << " -v  - be verbose - display loaded lbpools list" << endl;
	cout << " -vv - be more verbose - display every scheduling of a test and test result" << endl;
}


int main (int argc, char *argv[]) {
	start_logging();

	srand(time(NULL));;


	string config_file_name = "/etc/iglb/iglb.json";

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

	auto tool = new TestTool();
	tool->load_config(config_file_name);

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
