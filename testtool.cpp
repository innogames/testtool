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

#include "msg.h"

#include "lb_pool.h"
#include "lb_node.h"
#include "healthcheck.h"
#include "healthcheck_ping.h"

using namespace std;

/* Global variables, some are exported to other modules. */
struct event_base	*eventBase = NULL;
SSL_CTX			*sctx = NULL;
int			 verbose = 0;
int			 verbose_pfctl = 0;
bool			 pf_action = true;
bool			 check_downtimes = true; /* Start with true, so downtimes will be loaded on startup. */

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
void load_downtimes(list<LbPool *> * lbpools) {

	show_message(MSG_TYPE_NOTICE, "Reloading downtime list.");

	string line;

	set<string> downtimes;

	/* Read all the lbpool-node pairs and store them. */
	ifstream downtime_file("/tmp/testtool_downtimes");
	if (downtime_file) {
		while (getline(downtime_file, line)) {
			downtimes.insert(line);
		}
		downtime_file.close();
	} else {
		show_message(MSG_TYPE_WARNING, "Could not load downtime list file.");
	}

	/* Iterate over all lbpools and nodes, start downtime for the loaded ones, end for the ones not in the set. */
	for(list<LbPool*>::iterator lbpool = lbpools->begin(); lbpool != lbpools->end(); lbpool++) {
		for(unsigned int i=0; i<(*lbpool)->nodes.size(); i++) {

			if ( downtimes.count((*lbpool)->name + " " + (*lbpool)->nodes[i]->address) ) {
				(*lbpool)->nodes[i]->start_downtime();
			} else {
				(*lbpool)->nodes[i]->end_downtime();
			}
		}
	}
}


/*
   Load pools from given configuration file.
   Return a list of loaded lbpools.
*/
list<LbPool*> * load_lbpools(ifstream &config_file) {
	show_message(MSG_TYPE_NOTICE, "Loading configration file...");

	string line;

	/* Allocate an empty list of lbpools. */
	list<LbPool*>	*lbpools = new list<LbPool*>;

	/* Build a mapping between lbpool names and objects.
	   This is requried to create lbpool->backup_pool link. */
	map<string, LbPool*> lbpool_name_to_lbpool_obj;

	LbPool		*new_lbpool = NULL;
	LbNode		*new_lbnode = NULL;
	Healthcheck	*new_healthcheck = NULL;

	while (getline(config_file, line)) {

		if (line.empty())
			continue;

		string command;
		istringstream istr_line(line);

		istr_line >> command;

		/* For all the types of objects created, pass the istr_line to them.
		   They can read next parameters from it after we have read the first word. */
		if (command=="pool") {
			new_lbpool = new LbPool(istr_line);
			lbpools->push_back(new_lbpool);
			/* Insert mapping of name (string) to lbpool (object). */
			lbpool_name_to_lbpool_obj.insert(pair<string, LbPool*>(new_lbpool->name,new_lbpool));
		}
		else if (command=="node") {
			if (new_lbpool) {
				new_lbnode = new LbNode(istr_line, new_lbpool);
			}
		}
		else if (command=="healthcheck") {
			if (new_lbnode) {
				new_healthcheck = Healthcheck::healthcheck_factory(istr_line, new_lbnode);
			}
		}
	}

	/* Fill in lbpool->backup_pool and lbpool->used_as_backup.
	   For all lbpools... */
	for(list<LbPool*>::iterator lbpool_it = lbpools->begin(); lbpool_it != lbpools->end(); lbpool_it++) {
		
		/* ... iterate over all possible backup_lb_pools proposed for this lb_pool.
		   There can be multiple of them and some might be on other HWLBs!  */
		stringstream ss_backup_pools_names((*lbpool_it)->backup_pool_names);
		string s_backup_pool_name;
		while(getline(ss_backup_pools_names, s_backup_pool_name, ',')) {
			/* Get the object from name-to-object map. */
			LbPool *proposed_backup_pool = lbpool_name_to_lbpool_obj[s_backup_pool_name];

			/* Pick the first one located on proper HWLB. */
			if (proposed_backup_pool && proposed_backup_pool->default_hwlb == (*lbpool_it)->default_hwlb) {
				show_message(MSG_TYPE_DEBUG, " Mapping backup_pool %s to %s", proposed_backup_pool->name.c_str(), (*lbpool_it)->name.c_str());
				(*lbpool_it)->backup_pool = proposed_backup_pool;
				proposed_backup_pool->used_as_backup.push_back((*lbpool_it));
				break;
			}
		}
	}

	return lbpools;
}


/*
   This function schedules healthchecks on all lbnodes.
*/
void healthcheck_scheduler_callback(evutil_socket_t fd, short what, void *arg) {
	/* Make compiler happy. */
	(void)(fd);
	(void)(what);

	struct timespec now;

	list<LbPool *> *lbpools = (list<LbPool *> *)arg;

	if (check_downtimes) {
		load_downtimes(lbpools);
		check_downtimes = false;
	}

	/* Get time once and assume all checks started at this time. */
	clock_gettime(CLOCK_MONOTONIC, &now);

	/* Iterate over all lbpools and schedule healthchecks. */
	for(list<LbPool*>::iterator lbpool = lbpools->begin(); lbpool != lbpools->end(); lbpool++) {
		(*lbpool)->schedule_healthchecks(&now);
	}
}


/*
   This function parses the results of healthchecks for all lbpools.
*/
void healthcheck_parser_callback(evutil_socket_t fd, short what, void *arg) {
	/* Make compiler happy. */
	(void)(fd);
	(void)(what);

	list<LbPool *> *lbpools = (list<LbPool *> *)arg;

	/* Iterate over all lbpools parse healthcheck results. */
	for(list<LbPool*>::iterator lbpool = lbpools->begin(); lbpool != lbpools->end(); lbpool++) {
		(*lbpool)->parse_healthchecks_results();
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

	char buf[128];
	struct timeval  tv;
	struct tm      *tm;

	list<LbPool *> * lbpools = (list<LbPool *> *)arg;

	ofstream status_file("/var/log/testtool.status",  ios_base::out |  ios_base::trunc);

	gettimeofday(&tv, NULL);
	tm = localtime(&tv.tv_sec);
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S\0", tm);

	status_file << "Testtool's log, stardate " << buf << ". Our mission is lbpools' high availability." << endl;

	/* Iterate over all lbpools and write status of each one to the file. */
	for(list<LbPool*>::iterator lbpool = lbpools->begin(); lbpool != lbpools->end(); lbpool++) {
		status_file << "lbpool: " << (*lbpool)->name;
		status_file << " nodes_alive: " << (*lbpool)->count_live_nodes();
		status_file << " backup_pool: "  << ( (*lbpool)->backup_pool ? ((*lbpool)->switched_to_backup ? "active" : "configured") : "none" );
		status_file << endl;
	}

	 status_file.close();
}


void setup_events(list<LbPool *> * lbpools) {
	/*
	   Sleep time for the main loop (by the way, using unicode micro sign breaks my vim).
	    1 000 us =   1ms = 1000/s
	   10 000 us =  10ms =  100/s
	  100 000 us = 100ms =   10/s
	 */

	/* Run the healthcheck scheduler multiple times per second. */
	struct timeval healthcheck_scheduler_interval;
	healthcheck_scheduler_interval.tv_sec  = 0;
	healthcheck_scheduler_interval.tv_usec = 10000;
	struct event *healthcheck_scheduler_event = event_new(eventBase, -1, EV_PERSIST, healthcheck_scheduler_callback, lbpools);
	event_add(healthcheck_scheduler_event, &healthcheck_scheduler_interval);

	/* Run the healthcheck result parser multiple times per second. */
	struct timeval healthcheck_parser_interval;
	healthcheck_parser_interval.tv_sec  = 0;
	healthcheck_parser_interval.tv_usec = 100000;
	struct event *healthcheck_parser_event = event_new(eventBase, -1, EV_PERSIST, healthcheck_parser_callback, lbpools);
	event_add(healthcheck_parser_event, &healthcheck_parser_interval);

	/* Dump the status to a file every 45 seconds */
	struct timeval dump_status_interval;
	dump_status_interval.tv_sec  = 45;
	dump_status_interval.tv_usec = 0;
	struct event *dump_status_event = event_new(eventBase, -1, EV_PERSIST, dump_status_callback, lbpools);
	event_add(dump_status_event, &dump_status_interval);

	event_base_dispatch(eventBase);
}


void init_libevent() {
	eventBase = event_base_new();
	show_message(MSG_TYPE_DEBUG, " * libevent method: %s", event_base_get_method(eventBase));
}


void finish_libevent() {
	event_base_free(eventBase);
}


int init_libssl() {
	SSL_library_init (); 
	SSL_load_error_strings (); 
	OpenSSL_add_all_algorithms (); 

	show_message(MSG_TYPE_DEBUG, " * OpenSSL version: %s", SSLeay_version (SSLEAY_VERSION) );

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
	show_message(MSG_TYPE_NOTICE, "Starting testtool, built on %s %s",  __DATE__, __TIME__);

	list<LbPool *> * lbpools = NULL;
	srand(time(NULL));;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGPIPE, signal_handler);
	signal(SIGUSR1, signal_handler);

	string config_file_name = "/root/lb/testtool.conf";

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

	show_message(MSG_TYPE_DEBUG, "Initializing various stuff...");
	if (!init_libssl()) {
		show_message(MSG_TYPE_ERROR, "Unable to initialise OpenSSL!");
		exit(-1);
	}
	init_libevent();
	
	if (!Healthcheck_ping::initialize()) {
		show_message(MSG_TYPE_ERROR, "Unable to initialize Healthcheck_ping!");
		exit(-1);
	}

	/* Load lbpools and healthchecks. */
	ifstream config_file(config_file_name.c_str());
	if (!config_file) {
		show_message(MSG_TYPE_ERROR, "Unable to load configuration file!");
		exit(-1);
	}
	lbpools = load_lbpools(config_file);
	config_file.close();

	show_message(MSG_TYPE_DEBUG, "Entering the main loop...");
	setup_events(lbpools);
	show_message(MSG_TYPE_DEBUG, "Left the main loop.");

	show_message(MSG_TYPE_NOTICE, "Ending testtool, bye!");

	finish_libevent();
	finish_libssl();
	Healthcheck_ping::destroy();
}

