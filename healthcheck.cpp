#include <iostream>
#include <sstream>

#include "msg.h"
#include "pfctl.h"

#include "service.h"
#include "healthcheck.h"
#include "healthcheck_http.h"
#include "healthcheck_ping.h"
#include "healthcheck_dns.h"

using namespace std;

extern int verbose;

int timespec_diffms(struct timespec *a, struct timespec *b) {
	time_t diff_sec  = a->tv_sec  - b->tv_sec;
	long   diff_nsec = a->tv_nsec - b->tv_nsec;
	long   diff_msec = (diff_sec*1000) + (diff_nsec/1000000);
	return diff_msec;
}


/*
   General constructor for Healthcheck objects.
   - read common parameters and set object's properties
   - storespecific parameters in "parameters" string, so that specific constructor can use them later
*/
Healthcheck::Healthcheck(string &definition, class Service &service) {
	service.healthchecks.push_back(this);
	this->parent = &service;

	clock_gettime(CLOCK_MONOTONIC, &last_checked);

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
	/* Default timeout is 1500ms. */
	this->timeout.tv_sec   = 1;
	this->timeout.tv_nsec  = 500 * 1000 * 1000;

	this->downtime = false; /* The real value for this flag is set right after testtool is started. */
	this->is_running = false;

	/* Check if the IP address is in pf table. This determines the initial state. */
	if (pf_is_in_table(this->parent->name ,this->address)) {
		this->last_state         = STATE_UP;
		this->hard_state         = STATE_UP;
		this->parent->healthchecks_ok++;
		this->failure_counter    = 0;
	} else {
		this->last_state         = STATE_DOWN;
		this->hard_state         = STATE_DOWN;
		this->failure_counter    = this->max_failed_tests;
	}

	if (verbose>0)
		/* This is the general information so no newline here.
		   The healthcheck constructor should write anything he has to to the screen and then write the newline */
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
	else if (type=="ping")
		new_healthcheck = new Healthcheck_ping(definition, service);
	else if (type=="dns")
		new_healthcheck = new Healthcheck_dns(definition, service);

	return new_healthcheck;
}


int Healthcheck::schedule_healthcheck() {
	struct timespec now;

	/* Do not schedule the healthcheck twice nor when there is a downtime. */
	if (is_running || downtime)
		return false;

	/* Check if host should be checked at this time. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	if( timespec_diffms(&now, &last_checked) < check_interval*1000 + extra_delay)
		return false;

	last_checked = now;
	is_running = true;

	if (verbose>1)
		showStatus(CL_WHITE"%s"CL_RESET" - Scheduling healthcheck_%s %s:%u...\n", parent->name.c_str(), type.c_str(), address.c_str(), port);

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
			showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%u"CL_RESET" - handle_check_result: "CL_GREEN"back online after %d failures"CL_RESET"\n",
				parent->name.c_str(), address.c_str(), port, failure_counter);
		}

		/* Full transition from hard DOWN state. */
		if (hard_state == STATE_DOWN) {
			hard_state = STATE_UP;
			parent->healthchecks_ok++;
			parent->all_down_notified = false;

			/* Add the node back to pool if it is in normal (not backup) operation. */
			if (parent->switched_to_backup == false) {
				pf_table_add(parent->name, address);
			}

			/* Switch parent pool from backup pool's nodes to normal nodes. */
			if (parent->backup_pool && parent->healthchecks_ok >= parent->backup_pool_trigger && parent->switched_to_backup == true) {
				showStatus(CL_WHITE"%s"CL_RESET" - At least %d nodes alive, removing backup_pool "CL_BLUE"%s"CL_RESET"\n", parent->name.c_str(), parent->backup_pool_trigger, parent->backup_pool->name.c_str());

				/* Add original nodes back when leaving the backup pool mode. */
				for (unsigned int i=0; i<parent->healthchecks.size(); i++) {
					/* Add only the ones which were not STATE_UP. */
					if (parent->healthchecks[i]->hard_state == STATE_UP) {
						pf_table_add(parent->name, parent->healthchecks[i]->address);
					}
				}

				/* Remove backup nodes. */
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
			if (parent->switched_to_backup == false)
				pf_table_rebalance(parent->name, address);
		}

		failure_counter = 0;
	}
	/* Transition from UP to DOWN */
	else if (hard_state == STATE_UP && last_state == STATE_DOWN) {
		
		failure_counter++;

		if (!downtime)
			showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%u"CL_RESET" - handle_check_result: "CL_RED"down for the %d time"CL_RESET"\n",
				parent->name.c_str(), address.c_str(), port, failure_counter);

		if (failure_counter >= max_failed_tests) {
			if (!downtime)
				showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%u"CL_RESET" - handle_check_result: "CL_RED"permanenty down"CL_RESET"\n",
					parent->name.c_str(), address.c_str(), port);
			hard_state = STATE_DOWN;
			parent->healthchecks_ok--;

			/* Remove the node from its primary pool if the pool is in normal (not backup) operation. */
			if (parent->switched_to_backup == false) {
				pf_table_del(parent->name, address);
				pf_kill_src_nodes_to(address, true);
			}

			if (!parent->backup_pool && parent->healthchecks_ok == 0 && parent->all_down_notified == false) {
				parent->all_down_notified = true;
				showWarning(CL_WHITE"%s"CL_RESET" - No nodes left to serve the traffic!\n", parent->name.c_str());
			}

			/* Switch parent pool to backup pool's nodes if needed. */
			if (parent->backup_pool && parent->healthchecks_ok < parent->backup_pool_trigger && parent->switched_to_backup == false) {
				showStatus(CL_WHITE"%s"CL_RESET" - Less than %d nodes alive, switching to backup_pool "CL_BLUE"%s"CL_RESET"\n", parent->name.c_str(), parent->backup_pool_trigger, parent->backup_pool->name.c_str());

				/* Remove original nodes if there any left, we are switching to backup pool! */
				for (unsigned int i=0; i<parent->healthchecks.size(); i++) {
					/* Remove only nodes which are STATE_UP, the ones in STATE_DOWN were already removed. */
					if (parent->healthchecks[i]->hard_state == STATE_UP) {
						pf_table_del(parent->name, parent->healthchecks[i]->address);
						pf_kill_src_nodes_to(parent->healthchecks[i]->address, true);
					}
				}

				/* Add backup nodes, should there be any alive. Complain, if not. */
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
	is_running = false;
}


/*
   Start a downtime.
*/
void Healthcheck::start_downtime() {

	/* Do not mark the node down twice. */
	if (downtime)
		return;
	downtime = true;

	showStatus(CL_WHITE"%s"CL_RESET" - Starting downtime for %s:%u...\n", parent->name.c_str(), address.c_str(), port);

	/* If the node is in hard STATE_UP state at the moment, pretend there was a maximum
	   number of failures and enforce handle_result to take care of performing the usual
	   actions. Should there happen a normal handle_result later (as a result of a test
	   running in the meantime), it will just see the node already down. If the node
	   is not in hard STATE_UP state, the traffic to it is already stopped the usual way. */
	if (hard_state == STATE_UP) {
		last_state = STATE_DOWN;
		failure_counter = max_failed_tests;
		handle_result();
	}
}


/*
   End a downtime.
*/
void Healthcheck::end_downtime() {

	/* Remove downtime only once. */
	if (!downtime)
		return;
	
	showStatus(CL_WHITE"%s"CL_RESET" - Ending downtime for %s:%u...\n", parent->name.c_str(), address.c_str(), port);

	/* Make the service checkable again. */
	downtime = false;
}

