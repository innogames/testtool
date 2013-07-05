#include <iostream>
#include <sstream>

#include "msg.h"
#include "pfctl.h"

#include "service.h"
#include "healthcheck.h"
#include "healthcheck_http.h"

using namespace std;

extern int verbose;

int timespec_diffms(struct timespec *a, struct timespec *b) {
	time_t diff_sec  = a->tv_sec  - b->tv_sec;
	long   diff_nsec = a->tv_nsec - b->tv_nsec;
	long   diff_msec = (diff_sec*1000) + (diff_nsec/1000000);
	return diff_msec;
}


int Healthcheck::schedule_healthcheck() {
	cout << "dummy schedule healthcheck" << endl;
	return false;
}


/*
   General constructor for Healthcheck objects.
   - read common parameters and set object's properties
   - storespecific parameters in "parameters" string, so that specific constructor can use them later
*/
Healthcheck::Healthcheck(string &definition, class Service &service) {
	service.healthchecks.push_back(this);
	this->parent = &service;

	clock_gettime(CLOCK_REALTIME, &last_checked);

	istringstream istr_definition(definition);
	int	 check_interval;
	int	 max_failed_tests;
	string	 address;
	int	 port;

	string linetype;

	istr_definition >> linetype >> type >> check_interval >> max_failed_tests >> address >> port >> parameters;

	/* Fill in missing parameters. Be sure to copy all values instead of using
	   pointers to variables declared in this function. */
	this->check_interval   = check_interval;
	this->max_failed_tests = max_failed_tests;
	this->address          = address;
	this->port             = port;

	this->extra_delay      = rand() % 1000;
	this->timeout.tv_sec   = 1;
	this->timeout.tv_usec  = 500 * 1000;

	/* Check if the IP address is in pf table. This determines the initial state. */
	if (pf_is_in_table(this->parent->name ,this->address)) {
		this->last_state         = STATE_UP;
		this->hard_state         = STATE_UP;
		this->parent->healthchecks_ok++;
	} else {
		this->last_state         = STATE_DOWN;
		this->hard_state         = STATE_DOWN;
		this->failure_counter    = this->max_failed_tests;
	}

	if (verbose>0)
		cout << "  New healthcheck: type:" << type << " address:" << address << ":" << port << " interval:" << this->check_interval << " max_fail:" << this->max_failed_tests << " current_state:" << (this->hard_state==STATE_DOWN?"D":"U") << " ";
}


/*
   Healthcheck factory:
   - read type of healthcheck
   - create an object of the required type, pass the definition to it
   - return it
*/
Healthcheck *Healthcheck::healthcheck_factory(string &definition, class Service &service) {

	Healthcheck * new_healthcheck;

	/* Read the check type. */
	istringstream istr_definition(definition);
	string linetype;
	string type;
	istr_definition >> linetype >> type;

	/* Check the test type and create a proper object. */
	if (type=="http")
		new_healthcheck = new Healthcheck_http(definition, service);
	else if (type=="https")
		new_healthcheck = new Healthcheck_https(definition, service);

	return new_healthcheck;
}


bool Healthcheck::can_run_now() {
	struct timespec now;

	if (is_running)
		return false;

	/* Check if host should be checked at this time. */
	clock_gettime(CLOCK_REALTIME, &now);
	if( timespec_diffms(&now, &last_checked) < check_interval*1000 + extra_delay)
		return false;

	last_checked = now;
	is_running = true;

	return true;
}


/*
   After the test is finished, handle the result:
   - Add/remove the node to/from the pool.
   - Add/remove the node to/from other pools if used as a backup pool.
*/
void Healthcheck::handle_result() {
	/* Transition from DOWN to UP */
	if (last_state == STATE_UP) {
		
		/* Hard state was not reached yet, it is an UP after DOWNs<max_failures */
		if (failure_counter > 0) {
			showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%u"CL_RESET" - handle_check_result: "CL_GREEN"back online"CL_RESET"\n",
				parent->name.c_str(), address.c_str(), port);
		}

		/* Full transition from hard DOWN state. */
		if (hard_state == STATE_DOWN) {
			hard_state = STATE_UP;
			parent->healthchecks_ok++;
			parent->all_down_notified = false;

			/* Add the IP to this pool. */
			pf_table_add(parent->name, address);

			/* Switch parent pool from backup pool's nodes to normal nodes. */
			if (parent->backup_pool && parent->healthchecks_ok >= parent->backup_pool_trigger && parent->switched_to_backup == true) {
				showStatus(CL_WHITE"%s"CL_RESET" - More than %d nodes alive, removing backup_pool "CL_BLUE"%s"CL_RESET"\n", parent->name.c_str(), parent->backup_pool_trigger, parent->backup_pool->name.c_str());
				for (unsigned int i=0; i<parent->backup_pool->healthchecks.size(); i++) {
					if (parent->backup_pool->healthchecks[i]->hard_state == STATE_UP) {
						string address = parent->backup_pool->healthchecks[i]->address;
						pf_table_del(parent->name, address);
						pf_kill_src_nodes_to(address, true); /* Kill traffic going to backup pool's node. */
					}
				}
				parent->switched_to_backup = false;
			}

			/* Add the IP to other pools when used as backup_pool. */
			if (parent->used_as_backup.size())
				for (unsigned int i=0; i<parent->used_as_backup.size(); i++) {
					if (parent->used_as_backup[i]->switched_to_backup) {
						showStatus(CL_WHITE"%s"CL_RESET" - Used as backup, adding to other pool: "CL_BLUE"%s"CL_RESET"\n", parent->name.c_str(), parent->used_as_backup[i]->name.c_str());
						pf_table_add(parent->used_as_backup[i]->name, address);
						/* Do not rebalance traffic in other pools, we don't want to loose src_nodes in them,
						   so killing traffic to backup nodes will be possible later. */
					}
				}

			/* Finally rebalance traffic in this pool.
			   This must be done after traffic to removed backup nodes was killed
			   (killing, like rebalacing is done for src_nodes). */
			pf_table_rebalance(parent->name, address);
		}

		failure_counter = 0;
	}
	/* Transition from UP to DOWN */
	else if (hard_state == STATE_UP && last_state == STATE_DOWN) {
		
		failure_counter++;

		showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%u"CL_RESET" - handle_check_result: "CL_RED"down for the %d time"CL_RESET"\n",
			parent->name.c_str(), address.c_str(), port, failure_counter);

		if (failure_counter >= max_failed_tests) {
			showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%u"CL_RESET" - handle_check_result: "CL_RED"permanenty down"CL_RESET"\n",
				parent->name.c_str(), address.c_str(), port);
			hard_state = STATE_DOWN;
			parent->healthchecks_ok--;

			if (!parent->backup_pool && parent->healthchecks_ok == 0 && parent->all_down_notified == false) {
				parent->all_down_notified = true;
				showWarning(CL_WHITE"%s"CL_RESET" - No nodes left to serve the traffic!\n", parent->name.c_str());
			}

			/* Remove the node's IP from parent pool. */
			pf_table_del(parent->name, address);
			pf_kill_src_nodes_to(address, true);

			/* Switch parent pool to backup pool's nodes if needed. */
			if (parent->backup_pool && parent->healthchecks_ok < parent->backup_pool_trigger && parent->switched_to_backup == false) {
				showStatus(CL_WHITE"%s"CL_RESET" - Less than %d nodes alive, switching to backup_pool "CL_BLUE"%s"CL_RESET"\n", parent->name.c_str(), parent->backup_pool_trigger, parent->backup_pool->name.c_str());

				if (parent->backup_pool->healthchecks_ok>0)
				{
					for (unsigned int i=0; i<parent->backup_pool->healthchecks.size(); i++) {
						if (parent->backup_pool->healthchecks[i]->hard_state == STATE_UP) {
							string address = parent->backup_pool->healthchecks[i]->address;
							pf_table_add(parent->name, address); /* Add backup pool's IP to this pool. */
						}
					}
				} else {
					showWarning(CL_WHITE"%s"CL_RESET" - Backup_pool "CL_BLUE"%s"CL_RESET" has no nodes left to serve the traffic!\n", parent->name.c_str(), parent->backup_pool->name.c_str());
				}

				parent->switched_to_backup = true;
			}

			/* Remove the IP from other pools when used as backup_pool. */
			if (parent->used_as_backup.size())
				for (unsigned int i=0; i<parent->used_as_backup.size(); i++) {
					if (parent->used_as_backup[i]->switched_to_backup) {
						showStatus(CL_WHITE"%s"CL_RESET" - Used as backup, removing from other pool: "CL_BLUE"%s"CL_RESET"\n", parent->name.c_str(), parent->used_as_backup[i]->name.c_str());
						pf_table_del(parent->used_as_backup[i]->name, address);
						pf_kill_src_nodes_to(address, true);
					}
				}
		}
	}

	/* Mark the check as not running, so it can be scheduled again. */
	is_running=0;
}

