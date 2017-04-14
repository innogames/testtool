#include <iostream>
#include <sstream>
#include <typeinfo>
#include <stdlib.h>
#include <fmt/format.h>
#include <yaml-cpp/yaml.h>

#include "config.h"
#include "msg.h"
#include "pfctl.h"

#include "lb_pool.h"
#include "lb_node.h"
#include "healthcheck.h"
#include "healthcheck_tcp.h"
#include "healthcheck_http.h"
#include "healthcheck_ping.h"
#include "healthcheck_postgres.h"
#include "healthcheck_dns.h"

using namespace std;

extern int verbose;

int timespec_diffms(struct timespec *a, struct timespec *b) {
	time_t diff_sec  = a->tv_sec  - b->tv_sec;
	long   diff_nsec = a->tv_nsec - b->tv_nsec;
	long   diff_msec = (diff_sec * 1000) + (diff_nsec / 1000000);
	return diff_msec;
}


/*
 * Pretend that this healthcheck is fully failed.  This is to be used
 * for downtimes.
 */
void Healthcheck::force_failure() {
	failure_counter = max_failed_checks;
	hard_state      = STATE_DOWN;
	last_state      = STATE_DOWN;
}

/*
 * Link the healthcheck and its parent node, initialize some variables,
 * print diagnostic information if necessary.  Remember that this
 * constructor is called from each healthcheck's type-specific
 * constructor!  And it is called *before* that constructor does its
 * own work!
 */
Healthcheck::Healthcheck(const YAML::Node& config, class LbNode *_parent_lbnode) {
	/*
	 * Pretend that the healthcheck was performed just a moment ago.
	 * This is necessary to perform the check in proper time.
	 */
	clock_gettime(CLOCK_MONOTONIC, &last_checked);

	/* Link with parent lbnode. */
	parent_lbnode = _parent_lbnode;
	parent_lbnode->healthchecks.push_back(this);

	/* Set defaults, same as with old testtool. */
	this->port = parse_int(config["port"], 0);
	this->check_interval = parse_int(config["interval"], 2);
	this->max_failed_checks = parse_int(config["max_failed"], 3);
	int tmp_timeout = parse_int(config["timeout"], 1500);
	/* Timeout was read in ms, convert it to s and ns. */
	this->timeout.tv_sec   =  tmp_timeout / 1000;
	this->timeout.tv_usec  = (tmp_timeout % 1000) * 1000;
	/* Random delay to spread healthchecks in space-time continuum. */
	this->extra_delay = rand() % 1000;

	this->is_running = false;

	/*
	 * Initialize healthchecks state basing on state of parent node.
	 * Proper initial state for the healthcheck quarantees no
	 * unnecessary messages.
	 */
	if (parent_lbnode->get_state() == LbNode::STATE_UP) {
		hard_state      = STATE_UP;
		last_state      = STATE_UP;
		failure_counter = 0;
	} else {
		hard_state      = STATE_DOWN;
		last_state      = STATE_DOWN;
		failure_counter = max_failed_checks;
	}
}


/*
 * Healthcheck factory:
 *
 * - read type of healthcheck
 * - create an object of the required type, pass the config to it
 * - return it
*/
Healthcheck *Healthcheck::healthcheck_factory(const YAML::Node& config, class LbNode *_parent_lbnode) {

	Healthcheck * new_healthcheck = NULL;

	std::string type = config["type"].as<std::string>();
	if (type == "http")
		new_healthcheck = new Healthcheck_http(config, _parent_lbnode);
	else if (type == "https")
		new_healthcheck = new Healthcheck_https(config, _parent_lbnode);
	else if (type == "tcp")
		new_healthcheck = new Healthcheck_tcp(config, _parent_lbnode);
	else if (type == "ping")
		new_healthcheck = new Healthcheck_ping(config, _parent_lbnode);
	else if (type == "postgres")
		new_healthcheck = new Healthcheck_postgres(config, _parent_lbnode);
	else if (type == "dns")
		new_healthcheck = new Healthcheck_dns(config, _parent_lbnode);

	return new_healthcheck;
}

/*
 * Healthcheck scheduling is done via type-specific method, but that one
 * calls this general method in parent class.  This method checks if
 * the healthcheck can be run now.  Sub-class method should terminate,
 * if this one forbids the check from being run.
 */
int Healthcheck::schedule_healthcheck(struct timespec *now) {

	/* Do not schedule the healthcheck twice nor when there is a downtime. */
	if (is_running || parent_lbnode->downtime)
		return false;

	/* Check if host should be checked at this time. */
	if (timespec_diffms(now, &last_checked) < check_interval * 1000 + extra_delay)
		return false;

	memcpy(&last_checked, now, sizeof(struct timespec));
	is_running = true;

	if (verbose > 1)
		log(MSG_INFO, this, "scheduling");

	return true;
}


/*
 * This metod allows the check to have some final thoughts on its result.
 */
void Healthcheck::finalize_result() {
	/* Nothing to do here, it is used only for some types of healthchecks. */
}

/*
 * End health check
 *
 * It is the last function that has to be called at the end of
 * the health check.  If it wouldn't be called, the process is not
 * going to continue.
 */
void Healthcheck::end_check(HealthcheckResult result, string message) {
	msgType log_type;
	string statemsg;

	switch (result) {
		case HC_PASS:
			log_type = MSG_STATE_DOWN;
			statemsg = "state changed to up";
			this->last_state = STATE_UP;
			this->handle_result();
			break;

		case HC_FAIL:
			log_type = MSG_STATE_DOWN;
			statemsg = "state changed to down";
			this->last_state = STATE_DOWN;
			this->handle_result();
			break;

		case HC_PANIC:
			log_type = MSG_CRIT;
			statemsg = "check result failure";
			break;
	}

	if (verbose > 1 || this->last_state != this->hard_state || result > HC_FAIL)
		log(log_type, this, statemsg);

	if (result == HC_PANIC)
		exit(2);
}

/*
 * This method handles the change betwen UP and DOWN hard_state.
 * It performs no pf actions, this is to be done via lb_node or lb_pool!
 *
 * XXX This method is deprecated.  Use end_check() instead.  This will
 * be made private.
 */
void Healthcheck::handle_result() {

	// If a healtcheck has passed, zero the failure counter.
	if (last_state == STATE_UP)
		failure_counter = 0;

	// Change from DOWN to UP. The healthcheck has passed again.
	if (hard_state == STATE_DOWN && last_state == STATE_UP) {
		log(MSG_STATE_UP, this, "passed again");
		hard_state = STATE_UP;
		failure_counter = 0;
	}
	// Change from UP to DOWN. The healthcheck has failed.
	else if (hard_state == STATE_UP && last_state == STATE_DOWN) {

		failure_counter++;

		if (!parent_lbnode->downtime)
			log(MSG_STATE_DOWN, this, fmt::sprintf("failed for the %d time", failure_counter));

		// Mark the hard DOWN state only after the number of failed checks is reached.
		if (failure_counter >= max_failed_checks) {
			if (!parent_lbnode->downtime)
				log(MSG_STATE_DOWN, this, "hard failure reached");
			hard_state = STATE_DOWN;
		}
	}

	// Mark the check as not running, so it can be scheduled again.
	is_running = false;
}
