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
   Pretend that this healthtest is fully failed.
*/
void Healthcheck::downtime_failure() {
	failure_counter = max_failed_tests;
	hard_state      = STATE_DOWN;
	last_state      = STATE_DOWN;
}


/*
   General constructor for Healthcheck objects.
   - read common parameters and set object's properties
   - storespecific parameters in "parameters" string, so that specific constructor can use them later
*/
Healthcheck::Healthcheck(istringstream &definition, string _type, class LbNode *_parent_lbnode) {
	clock_gettime(CLOCK_MONOTONIC, &last_checked);

	parent_lbnode = _parent_lbnode;
	parent_lbnode->healthchecks.push_back(this);

	definition >> port >> check_interval >> max_failed_tests >> parameters;

	this->type = _type;

	this->extra_delay      = rand() % 1000;
	/* Default timeout is 1500ms. */
	this->timeout.tv_sec   = 1;
	this->timeout.tv_nsec  = 500 * 1000 * 1000;

	this->is_running = false;

	/* Initialize healthchecks state basing on state of parent node. */
	if (parent_lbnode->hard_state == STATE_UP) {
		hard_state      = STATE_UP;
		last_state      = STATE_UP;
		failure_counter = 0;

	} else {
		hard_state      = STATE_DOWN;
		last_state      = STATE_DOWN;
		failure_counter = max_failed_tests;
	}

	if (verbose>0) {
		/* This is the general information so no newline here.
		   The healthcheck constructor should write anything he has to to the screen and then write the newline */
		cout << "    New healthcheck: type:" << type;
		if (port>0)
			cout << " port:" << port;
		cout << " interval:" << this->check_interval << " max_fail:" << this->max_failed_tests << " ";
	}
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

	/* Check the test type and create a proper object. */
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

	if (verbose>1)
		showStatus(CL_WHITE"%s"CL_RESET" - Scheduling healthcheck_%s %s:%u...\n", parent_lbnode->parent_lbpool->name.c_str(), type.c_str(), parent_lbnode->address.c_str(), port);

	return true;
}


/*
   Handle changes of state. Do not perform any real actions, just note when a hard state is reached.
*/
void Healthcheck::handle_result() {

	if (hard_state == STATE_DOWN && last_state == STATE_UP) {
		showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%u"CL_RESET" - Healthcheck: "CL_GREEN"passed again"CL_RESET"\n",
			parent_lbnode->parent_lbpool->name.c_str(), parent_lbnode->address.c_str(), port);

		hard_state = STATE_UP;
		failure_counter = 0;
	}
	else if (hard_state == STATE_UP && last_state == STATE_DOWN) {

		failure_counter++;

		if (!parent_lbnode->downtime)
			showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%u"CL_RESET" - Healthcheck: "CL_RED"failed for the %d time"CL_RESET"\n",
				parent_lbnode->parent_lbpool->name.c_str(), parent_lbnode->address.c_str(), port, failure_counter);

		if (failure_counter >= max_failed_tests) {
			if (!parent_lbnode->downtime)
				showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%u"CL_RESET" - Healthcheck: "CL_RED"hard failure reached"CL_RESET"\n",
					parent_lbnode->parent_lbpool->name.c_str(), parent_lbnode->address.c_str(), port);

			hard_state = STATE_DOWN;
		}
	}

	/* Mark the check as not running, so it can be scheduled again. */
	is_running = false;
}


