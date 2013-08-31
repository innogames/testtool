#include <iostream>
#include <sstream>
#include <typeinfo>

#include "msg.h"
#include "pfctl.h"

#include "lb_pool.h"
#include "lb_node.h"
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
   Pretend that this healthcheck is fully failed. This is to be used for downtimes.
*/
void Healthcheck::force_failure() {
	failure_counter = max_failed_checks;
	hard_state      = STATE_DOWN;
	last_state      = STATE_DOWN;
}


/*
   Link the healthcheck and its parent node, initialize some variables, print diagnostic information if necessary.
   Remember that this constructor is called from each healthcheck's type-specific constructor!
   And it is called *before* that constructor does its own work!
*/
Healthcheck::Healthcheck(istringstream &definition, class LbNode *_parent_lbnode) {
	/* Pretend that the healthcheck was performed just a moment ago.
	   This is necessary to perform the check in proper time. */
	clock_gettime(CLOCK_MONOTONIC, &last_checked);

	/* Link with parent lbnode. */
	parent_lbnode = _parent_lbnode;
	parent_lbnode->healthchecks.push_back(this);

	/* Read all things from the definition line.
	   "parameters" is the last word on the line, type-specific constructor will read
	   its ":"-separated list of parameters from there. */
	definition >> port >> check_interval >> max_failed_checks >> parameters;

	/* Random delay to spread healthchecks in space-time continuum. */
	this->extra_delay = rand() % 1000;

	/* Default timeout is 1500ms. */
	this->timeout.tv_sec   = 1;
	this->timeout.tv_nsec  = 500 * 1000 * 1000;

	this->is_running = false;

	/* Initialize healthchecks state basing on state of parent node.
	   Proper initial state for the healthcheck quarantees no unnecessary messages. */
	if (parent_lbnode->hard_state == STATE_UP) {
		hard_state      = STATE_UP;
		last_state      = STATE_UP;
		failure_counter = 0;

	} else {
		hard_state      = STATE_DOWN;
		last_state      = STATE_DOWN;
		failure_counter = max_failed_checks;
	}

	show_message(MSG_TYPE_DEBUG, "    * New healthcheck:");
	show_message(MSG_TYPE_DEBUG, "      interval: %d, max_fail: %d", check_interval, max_failed_checks);
}


/*
   Healthcheck factory:
   - read type of healthcheck
   - create an object of the required type, pass the definition to it
   - return it
*/
Healthcheck *Healthcheck::healthcheck_factory(istringstream &definition, class LbNode *_parent_lbnode) {

	Healthcheck * new_healthcheck = NULL;

	/* Read the check type. */
	string type;

	definition >> type;

	/* Check the healthcheck type and create a proper object.
	   Keep in mind that definition is already stripped from the first word,
	   the healthcheck constructor will read next words from it. */
	if (type=="http")
		new_healthcheck = new Healthcheck_http(definition, _parent_lbnode);
	else if (type=="https")
		new_healthcheck = new Healthcheck_https(definition, _parent_lbnode);
	else if (type=="ping")
		new_healthcheck = new Healthcheck_ping(definition, _parent_lbnode);
	else if (type=="dns")
		new_healthcheck = new Healthcheck_dns(definition, _parent_lbnode);

	return new_healthcheck;
}

/*
   Healthcheck scheduling is done via type-specific method,
   but that one calls this general method in parent class.
   This method checks if the healthcheck can be run now.
   Sub-class method should terminate if this one forbids the check from being run.
*/
int Healthcheck::schedule_healthcheck() {
	struct timespec now;

	/* Do not schedule the healthcheck twice nor when there is a downtime. */
	if (is_running || parent_lbnode->downtime)
		return false;

	/* Check if host should be checked at this time. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	if( timespec_diffms(&now, &last_checked) < check_interval*1000 + extra_delay)
		return false;

	last_checked = now;
	is_running = true;

	if (verbose>1) {
		if (port>0)
			show_message(MSG_TYPE_NOTICE, "%s %s:%u - Scheduling healthcheck_%s...", parent_lbnode->parent_lbpool->name.c_str(), parent_lbnode->address.c_str(), port, type.c_str());
		else
			show_message(MSG_TYPE_NOTICE, "%s %s - Scheduling healthcheck_%s...", parent_lbnode->parent_lbpool->name.c_str(), parent_lbnode->address.c_str(), type.c_str());

	}

	return true;
}


/*
   This metod allows the check to have some final thoughts on its result.
*/
void Healthcheck::finalize_result() {
	/* Nothing to do here, it is used only for some types of healthchecks. */
}


/*
   This method handles the change betwen UP and DOWN hard_state.
   It performs no pf actions, this is to be done via lb_node or lb_pool!
*/
void Healthcheck::handle_result() {

	/* Change from DOWN to UP. The healthcheck has passed again. */
	if (hard_state == STATE_DOWN && last_state == STATE_UP) {
		if (port>0)
			show_message(MSG_TYPE_HC_PASS, "%s %s:%u - passed again",
				parent_lbnode->parent_lbpool->name.c_str(), parent_lbnode->address.c_str(), port);
		else
			show_message(MSG_TYPE_HC_PASS, "%s %s - passed again",
				parent_lbnode->parent_lbpool->name.c_str(), parent_lbnode->address.c_str());

		hard_state = STATE_UP;
		failure_counter = 0;
	}
	/* Change from UP to DOWN. The healthcheck has failed. */
	else if (hard_state == STATE_UP && last_state == STATE_DOWN) {

		failure_counter++;

		if (!parent_lbnode->downtime) {
			if (port>0)
				show_message(MSG_TYPE_HC_FAIL, "%s %s:%u - failed for the %d time",
					parent_lbnode->parent_lbpool->name.c_str(), parent_lbnode->address.c_str(), port, failure_counter);
			else
				show_message(MSG_TYPE_HC_FAIL, "%s %s - failed for the %d time",
					parent_lbnode->parent_lbpool->name.c_str(), parent_lbnode->address.c_str(), failure_counter);

		}

		/* Mark the hard DOWN state only after the number of failed checks is reached. */
		if (failure_counter >= max_failed_checks) {
			if (!parent_lbnode->downtime) {
				if (port>0)
					show_message(MSG_TYPE_HC_HFAIL, "%s %s:%u - hard failure reached",
						parent_lbnode->parent_lbpool->name.c_str(), parent_lbnode->address.c_str(), port);
				else
					show_message(MSG_TYPE_HC_HFAIL, "%s %s - hard failure reached",
						parent_lbnode->parent_lbpool->name.c_str(), parent_lbnode->address.c_str());
			}

			hard_state = STATE_DOWN;
		}
	}

	/* Mark the check as not running, so it can be scheduled again. */
	is_running = false;
}


