#include <iostream>
#include <fstream>
#include <list>
#include <sstream>
#include <map>
#include <vector>
#include <set>

#include <unistd.h>
#include <sys/types.h>
#include <signal.h>

#include <event2/event.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "config.h"
#include "msg.h"

#include "lb_vip.h"
#include "lb_pool.h"
#include "lb_node.h"
#include "healthcheck.h"
#include "healthcheck_ping.h"
#include "testtool.h"

using namespace std;

/* Global variables, some are exported to other modules. */
struct event_base	*eventBase = NULL;
SSL_CTX			*sctx = NULL;
int			 verbose = 0;
int			 verbose_pfctl = 0;
bool			 pf_action = true;
bool			 check_downtimes = false; /* Whether downtimes should be reloaded. */

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
	log_txt(MSG_TYPE_DEBUG, "Reloading downtime list.");

	string line;

	set<string> downtimes;

	/* Read all the lbpool-node pairs and store them. */
	ifstream downtime_file("/etc/iglb/testtool_downtimes.conf");
	if (downtime_file) {
		while (getline(downtime_file, line)) {
			downtimes.insert(line);
		}
		downtime_file.close();
	} else {
		log_txt(MSG_TYPE_DEBUG, "Could not load downtime list file.");
	}

	/* Iterate over all lbpools and nodes, start downtime for the loaded ones, end for the ones not in the set. */
	for (auto lbpool : m_pools) {
		for (auto node : lbpool->nodes) {

			if ( downtimes.count(lbpool->name + " " + node->address) ) {
				node->start_downtime();
			} else {
				node->end_downtime();
			}
		}
	}
}


/*
   Load pools from given configuration file.
   Return a list of loaded VIPs.
*/
void TestTool::load_config(ifstream &config_file) {
	log_txt(MSG_TYPE_DEBUG, "Loading configration file...");

	string line;

	/* Build a mapping between lbpool names and objects.
	   This is requried to create lbpool->backup_pool link. */
	map<string, LbPool*> lbpool_name_to_lbpool_obj;

	/* Temporary mapping of backup pools, which get populated in a second run. */
	map<LbVip*, string> lbvip_backup_names;

	LbPool		*new_lbpool = NULL;
	LbNode		*new_lbnode = NULL;

	while (getline(config_file, line)) {

		if (line.empty())
			continue;

		string command;
		istringstream istr_line(line);

		istr_line >> command;

		/* For all the types of objects created, pass the istr_line to them.
		   They can read next parameters from it after we have read the first word. */
		if (command=="pool_params") {
			string name;
			istr_line >> name;

			auto config = ConfigLine(istr_line);

			int min_nodes;
			config.load("min_nodes", min_nodes, 1);

			LbPool::FaultPolicy fault_policy;
			string min_nodes_action;
			config.load("min_nodes_action", min_nodes_action, "force_down");
			fault_policy = LbPool::fault_policy_by_name(min_nodes_action);

			string hwlb;
			config.load("hwlb", hwlb, "");
			if (hwlb == "") {
				log_txt(MSG_TYPE_CRITICAL, "VIP %s has empty hwlb!", name.c_str());
			}

			/* Implicitly create both VIP and primary pool. */
			auto vip = new LbVip(name, hwlb);
			new_lbpool = new LbPool(name, hwlb, min_nodes, fault_policy);

			vip->attach_pool(new_lbpool, POOL_PRIMARY);
			m_pools.push_back(new_lbpool);
			m_vips.push_back(vip);

			/* Track backup pool names. */
			config.load("backup_pools", lbvip_backup_names[vip], "");

			/* Insert mapping of name (string) to lbpool (object). */
			lbpool_name_to_lbpool_obj[name] = new_lbpool;
		}
		else if (command=="node") {
			if (new_lbpool) {
				new_lbnode = new LbNode(istr_line, new_lbpool);
			}
		}
		else if (command=="healthcheck_params") {
			if (new_lbnode) {
				Healthcheck::healthcheck_factory(istr_line, new_lbnode);
			}
		}
	}

	/* Fill in backup pools. */
	for (auto& kv : lbvip_backup_names) {
		auto vip = kv.first;
		auto& backup_pool_names = kv.second;

		/* ... iterate over all possible backup_lb_pools proposed for this lb_pool.
		   There can be multiple of them and some might be on other HWLBs!  */
		stringstream ss_backup_pools_names(backup_pool_names);
		string s_backup_pool_name;
		while(getline(ss_backup_pools_names, s_backup_pool_name, ',')) {
			/* Get the object from name-to-object map. */
			LbPool *proposed_backup_pool = lbpool_name_to_lbpool_obj[s_backup_pool_name];

			/* Pick the first one located on proper HWLB. */
			if (proposed_backup_pool && proposed_backup_pool->hwlb == vip->hwlb) {
				log_txt(MSG_TYPE_DEBUG, "Mapping backup_pool %s to VIP %s", proposed_backup_pool->name.c_str(), vip->name.c_str());
				vip->attach_pool(proposed_backup_pool, POOL_BACKUP);
				break;
			}
		}
	}

	load_downtimes();

	for (auto vip : m_vips) {
		vip->start();
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
	for (auto lbpool : m_pools) {
		lbpool->schedule_healthchecks(&now);
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
	for (auto lbpool : m_pools) {
		lbpool->parse_healthchecks_results();
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

	ofstream status_file("/var/log/testtool.status",  ios_base::out |  ios_base::trunc);

	gettimeofday(&tv, NULL);
	tm = localtime(&tv.tv_sec);
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S\0", tm);

	status_file << "Testtool's log, stardate " << buf << ". Our mission is lbpools' high availability." << endl;

	/* Iterate over all VIPs and write status of each one to the file.
	   NOTE: Syntax uses old data model due to Nagios parsing. */
	for (auto vip : m_vips) {
		status_file << "lbpool: " << vip->name;
		status_file << " nodes_alive: " << vip->count_live_nodes();

		auto backup_link = vip->get_backup_pool();
		status_file << " backup_pool: "  << (backup_link ? (backup_link->active ? "active" : "configured") : "none" );
		status_file << endl;
	}

	status_file.close();
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
	healthcheck_scheduler_interval.tv_usec = 100000;
	struct event *healthcheck_scheduler_event = event_new(eventBase, -1, EV_PERSIST, healthcheck_scheduler_callback, this);
	event_add(healthcheck_scheduler_event, &healthcheck_scheduler_interval);

	/* Run the healthcheck result parser multiple times per second. */
	struct timeval healthcheck_parser_interval;
	healthcheck_parser_interval.tv_sec  = 0;
	healthcheck_parser_interval.tv_usec = 100000;
	struct event *healthcheck_parser_event = event_new(eventBase, -1, EV_PERSIST, healthcheck_parser_callback, this);
	event_add(healthcheck_parser_event, &healthcheck_parser_interval);

	/* Dump the status to a file every 45 seconds */
	struct timeval dump_status_interval;
	dump_status_interval.tv_sec  = 45;
	dump_status_interval.tv_usec = 0;
	struct event *dump_status_event = event_new(eventBase, -1, EV_PERSIST, dump_status_callback, this);
	event_add(dump_status_event, &dump_status_interval);
}


void init_libevent() {
	eventBase = event_base_new();
	log_txt(MSG_TYPE_DEBUG, "libevent method: %s", event_base_get_method(eventBase));
}


void finish_libevent() {
	event_base_free(eventBase);
}


int init_libssl() {
	SSL_library_init ();
	SSL_load_error_strings ();
	OpenSSL_add_all_algorithms ();

	log_txt(MSG_TYPE_DEBUG, "OpenSSL version: %s", SSLeay_version (SSLEAY_VERSION) );

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
	log_txt(MSG_TYPE_DEBUG, "Starting testtool, built on %s %s @ %s",  __DATE__, __TIME__, __HOSTNAME__);
	log_txt(MSG_TYPE_DEBUG, "Built on branch %s, last commit %s", __GIT_BRANCH__, __GIT_LAST_COMMIT__ );

	srand(time(NULL));;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGPIPE, signal_handler);
	signal(SIGUSR1, signal_handler);

	string config_file_name = "/etc/iglb/testtool.conf";

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
				exit(0);
				break;
		}
	}

	log_txt(MSG_TYPE_DEBUG, "Initializing various stuff...");
	if (!init_libssl()) {
		log_txt(MSG_TYPE_CRITICAL, "Unable to initialise OpenSSL!");
		exit(-1);
	}
	init_libevent();

	if (!Healthcheck_ping::initialize()) {
		log_txt(MSG_TYPE_CRITICAL, "Unable to initialize Healthcheck_ping!");
		exit(-1);
	}

	/* Load lbpools and healthchecks. */
	ifstream config_file(config_file_name.c_str());
	if (!config_file) {
		log_txt(MSG_TYPE_CRITICAL, "Unable to load configuration file!");
		exit(-1);
	}

	auto tool = new TestTool();
	tool->load_config(config_file);
	config_file.close();

	tool->setup_events();
	log_txt(MSG_TYPE_DEBUG, "Entering the main loop...");
	event_base_dispatch(eventBase);
	log_txt(MSG_TYPE_DEBUG, "Left the main loop.");

	delete tool;
	log_txt(MSG_TYPE_DEBUG, "Ending testtool, bye!");

	Healthcheck_ping::destroy();

	finish_libevent();
	finish_libssl();
}

