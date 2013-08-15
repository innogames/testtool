#include <iostream>
#include <fstream>
#include <list>
#include <sstream>
#include <map>
#include <vector>
#include <set>

#include <signal.h>

#include <event2/event.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "service.h"
#include "healthcheck.h"
#include "healthcheck_ping.h"
#include "msg.h"

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
			/* This will happen on failed SSL tests. */
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
   Load downtime list and dowtime specified nodes in a specified service.
   A downtime means that:
   - The specified nodes are immediately marked as "down" with all the consequences.
   - They will not be tested anymore until the downtime is removed.
   - After the downtime is removed, they will be subject to normal healthchecks before getting traffic again.
*/
void load_downtimes(list<Service *> * services) {

	showStatus("Reloading downtime list.\n");

	string line;

	set<string> downtimes;

	/* Read all the service-node pairs and store them. */
	ifstream downtime_file("/tmp/testtool_downtimes");
	if (downtime_file) {
		while (getline(downtime_file, line)) {
			downtimes.insert(line);
		}
		downtime_file.close();
	} else {
		showWarning("Could not load downtime list file.\n");
	}

	/* Iterate over all services and nodes, start downtime for the loaded ones, end for the ones not in the set. */
	for(list<Service*>::iterator service = services->begin(); service != services->end(); service++) {
		for(unsigned int i=0; i<(*service)->healthchecks.size(); i++) {

			if ( downtimes.count((*service)->name + " " + (*service)->healthchecks[i]->address) )
				(*service)->healthchecks[i]->start_downtime();
			else
				(*service)->healthchecks[i]->end_downtime();
		}
	}
}


/*
   Load services from given configuration file.
   Return a list of loaded services.
*/
list<Service*> * load_services(ifstream &config_file) {

	string line;

	/* Allocate an empty list of services. */
	list<Service*> * services = new list<Service*>;

	map<Service*,vector<string> > services_backup_pools;

	Service * new_service = NULL;

	while (getline(config_file, line)) {
		Healthcheck * new_healthcheck;

		if (line.empty())
			continue;

		const char * c_line = line.c_str();
		string service_name;
		int default_hwlb;
		istringstream istr_service;

		switch (c_line[0]) {
			case '[':
				istr_service.str(line.substr(1, line.length()-2));
				istr_service >> service_name >> default_hwlb;
				new_service = new Service(service_name, default_hwlb);
				services->push_back(new_service);
				break;
			case '#':
			case ';':
				/* It's a comment line */
				continue;
				break;
			default:
				if (line.find("healthcheck") == 0)
					new_healthcheck = Healthcheck::healthcheck_factory(line, *new_service);

				if (line.find("backup_pool") == 0)
					if (new_service) {
						/* Read backup_pool parameters from the line. */
						string skip; /* word "backup_pool" will go here */
						int backup_pool_trigger;

						string backup_pool;
						vector<string> backup_pools;
						istringstream istr_line(line);

						/* Read most of the parameters to proper variables, then load all the backup_pools to vector<>.
						   Not all backup_pools are valid for this pool, as they might be on other HWLB.
						   This will be verified later. */
						istr_line >> skip >> backup_pool_trigger;
						while (istr_line >> backup_pool)
							backup_pools.push_back(backup_pool);

						services_backup_pools.insert(pair<Service*,vector<string> >(new_service,backup_pools));
						new_service->backup_pool_trigger = backup_pool_trigger;
					}

				break;
		}
	}

	/* Join backup pools to pools. Iterate over mappings found in the config file. Each mapping gives
	   all possible backup_pools, but not all exist on this HWLB. */
	map<Service*,vector<string> >::iterator service_it;
	for (service_it=services_backup_pools.begin(); service_it!=services_backup_pools.end(); service_it++) {

		/* Iterate over all backup_pools mapped to the current backup_pool. */
		for (unsigned int i=0; i<service_it->second.size(); i++) {
			string backup_pool_name = service_it->second[i];

			/* We have the backup_pool name, let's try to find pool object matching that name. */
			for(list<Service*>::iterator all_services_it = services->begin(); all_services_it != services->end(); all_services_it++) {
				/* Check if the found object has a matching name and ensure that its default HWLB is the same as of current pool. */
				if ((*all_services_it)->name == backup_pool_name && (*all_services_it)->default_hwlb == (*service_it->first).default_hwlb) {

					/* Append the service to "used by" field of backup_pool. */
					(*all_services_it)->used_as_backup.push_back(service_it->first);

					/* Use the servce as a backup_pool. */
					service_it->first->backup_pool = (*all_services_it);
					break;
				}
			}
			/* Was an object found for this name? The first one will do just fine, stop searching. */
			if (service_it->first->backup_pool) {
				if (verbose>0)
					cout << "Pool " << service_it->first->name << " uses backup_pool " << service_it->first->backup_pool->name << endl; 
				break;
			}
		}
	}

	return services;
}


/*
   The real main loop happens here. The function goes over all services
   and schedules the healthchecks to run.
*/
void main_loop_callback(evutil_socket_t fd, short what, void *arg) {
	/* Make compiler happy. */
	(void)(fd);
	(void)(what);

	list<Service *> * services = (list<Service *> *)arg;

	if (check_downtimes) {
		load_downtimes(services);
		check_downtimes = false;
	}
	
	/* Iterate over all services and hosts and schedule tests. */
	for(list<Service*>::iterator service = services->begin(); service != services->end(); service++) {
		(*service)->schedule_healthchecks();
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

	list<Service *> * services = (list<Service *> *)arg;

	ofstream status_file("/var/log/testtool.status",  ios_base::out |  ios_base::trunc);

	gettimeofday(&tv, NULL);
	tm = localtime(&tv.tv_sec);
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S\0", tm);

	status_file << "Testtool's log, stardate " << buf << ". Our mission is services' high availability." << endl;

	/* Iterate over all services and write status of each one to the file. */
	for(list<Service*>::iterator service = services->begin(); service != services->end(); service++) {
		status_file << "service: " << (*service)->name;
		status_file << " nodes_alive: " << (*service)->count_live_nodes();
		status_file << " backup_pool: "  << ( (*service)->backup_pool ? ((*service)->switched_to_backup ? "active" : "configured") : "none" );
		status_file << endl;
	}

	 status_file.close();
}


void setup_events(list<Service *> * services) {
	/*
	   Sleep time for the main loop (by the way, using unicode micro sign breaks my vim).
	    1 000 us =   1ms = 1000/s
	   10 000 us =  10ms =  100/s
	  100 000 us = 100ms =   10/s
	 */

	/* Run the main loop multiple times per second. */
	struct timeval main_loop_interval;
	main_loop_interval.tv_sec  = 0;
	main_loop_interval.tv_usec = 10000;
	struct event *main_loop_event = event_new(eventBase, -1, EV_PERSIST, main_loop_callback, services);
	event_add(main_loop_event, &main_loop_interval);

	/* Dump the status to a file every 45 seconds */
	struct timeval dump_status_interval;
	dump_status_interval.tv_sec  = 45;
	dump_status_interval.tv_usec = 0;
	struct event *dump_status_event = event_new(eventBase, -1, EV_PERSIST, dump_status_callback, services);
	event_add(dump_status_event, &dump_status_interval);

	event_base_dispatch(eventBase);
}


void init_libevent() {
	eventBase = event_base_new();
	printf(" * libevent method: %s\n", event_base_get_method(eventBase));
}


void finish_libevent() {
	event_base_free(eventBase);
}


int init_libssl() {
	SSL_library_init (); 
	SSL_load_error_strings (); 
	OpenSSL_add_all_algorithms (); 

	printf (" * OpenSSL version: %s\n", SSLeay_version (SSLEAY_VERSION) );

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
	cout << " -v  - be verbose - display loaded services list" << endl;
	cout << " -vv - be more verbose - display every scheduling of a test and test result" << endl;
}


int main (int argc, char *argv[]) {
	show("Built on %s - %s %s\n", __HOSTNAME__, __DATE__, __TIME__);

	list<Service *> * services = NULL;
	srand(time(NULL));;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGPIPE, signal_handler);
	signal(SIGUSR1, signal_handler);

	string config_file_name = "/root/lb/services.conf.new";

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


	if (!init_libssl()) {
		cerr << "Unable to initialise OpenSSL!" << endl;
		exit(-1);
	}
	init_libevent();
	
	if (!Healthcheck_ping::initialize()) {
		cerr << "Unable to initialize Healthcheck_ping!" << endl;
		exit(-1);
	}

	/* Load services and healthchecks. */
	ifstream config_file(config_file_name.c_str());
	if (!config_file) {
		cerr << "Unable to load configuration file!" << endl;
		exit(-1);
	}
	services = load_services(config_file);
	config_file.close();

	cout << "Entering the main loop..." << endl;
	setup_events(services);
	cout << "Left the main loop." << endl;

	cout << "Bye, see you next time!" << endl;

	finish_libevent();
	finish_libssl();
	Healthcheck_ping::destroy();
}

