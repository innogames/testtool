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
		if (healthchecks[i]->can_run_now()) {
			if (verbose>1)
				showStatus(CL_WHITE"%s"CL_RESET" - Scheduling healthcheck_%s %s:%u...\n", healthchecks[i]->parent->name.c_str(), healthchecks[i]->type.c_str(), healthchecks[i]->address.c_str(), healthchecks[i]->port);
			healthchecks[i]->schedule_healthcheck();
		}
	}
}


