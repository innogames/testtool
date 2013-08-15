#include <iostream>

#include "service.h"
#include "healthcheck.h"

#include "msg.h"
#include "pfctl.h"

using namespace std;


/* Global variables. */
extern bool	 	 pf_action;
extern int	 	 verbose;


/*
   The constructor has not much work to do, init some variables
   and display the Service name if verbose.
*/
Service::Service(string &name, int default_hwlb) {
	this->name = name;
	this->default_hwlb = default_hwlb;
	switched_to_backup = false;
	backup_pool = NULL;
	healthchecks_ok = 0;
	if (verbose>0)
		cout << "New service " << name << " on HWLB " << default_hwlb << ":" << endl;
}

void Service::schedule_healthchecks() {
	for(unsigned int i=0; i<healthchecks.size(); i++) {
		healthchecks[i]->schedule_healthcheck();
	}
}


/*
   Count nodes in hard STATE_UP state.
*/
int Service::count_live_nodes() {
	int ret = 0;
	for(unsigned int hc=0; hc<healthchecks.size(); hc++) {
		if (healthchecks[hc]->hard_state == STATE_UP)
			ret++;
	}

	return ret;
}


