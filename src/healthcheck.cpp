/*
 * Testtool - Health Check Generals
 *
 * Copyright (c) 2018 InnoGames GmbH
 */

#include <iostream>
#include <sstream>
#include <typeinfo>
#include <stdlib.h>
#include <fmt/format.h>
#include <fmt/printf.h>
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
	this->check_interval = parse_int(config["hc_interval"], 2);
	this->max_failed_checks = parse_int(config["hc_max_failed"], 3);
	int tmp_timeout = parse_int(config["hc_timeout"], 1500);
	/* Timeout was read in ms, convert it to s and ns. */
	this->timeout.tv_sec   =  tmp_timeout / 1000;
	this->timeout.tv_usec  = (tmp_timeout % 1000) * 1000;
	/* Random delay to spread healthchecks in space-time continuum. */
	this->extra_delay = rand() % 1000;

	this->is_running = false;
	this->ran = false;

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

	std::string type = parse_string(config["hc_type"], "");

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
	else
		return NULL;

	log(MSG_INFO, new_healthcheck, "state: created");

	return new_healthcheck;
}

/*
 * Healthcheck scheduling is done via type-specific method, but that one
 * calls this general method in parent class.  This method checks if
 * the healthcheck can be run now.  Sub-class method should terminate,
 * if this one forbids the check from being run.
 */
int Healthcheck::schedule_healthcheck(struct timespec *now) {

	/* Do not schedule the healthcheck twice. */
	if (is_running)
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
void Healthcheck::finalize() {
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
			log_type = MSG_STATE_UP;
			this->last_state = STATE_UP;
			statemsg = fmt::sprintf("state: up message: %s", message);
			this->handle_result(statemsg);
			break;

		case HC_FAIL:
			log_type = MSG_STATE_DOWN;
			this->last_state = STATE_DOWN;
			statemsg = fmt::sprintf("state: down message: %s", message);
			this->handle_result(statemsg);
			break;

		case HC_PANIC:
			log_type = MSG_CRIT;
			statemsg = fmt::sprintf("state: failure message: %s", message);
			log(log_type, this, statemsg);
			exit(2);
			break;
	}

	this->parent_lbnode->node_logic();
}

/*
 * This method handles the change betwen UP and DOWN hard_state.
 * It performs no pf actions, this is to be done via lb_node or lb_pool!
 */
void Healthcheck::handle_result(string message) {
	string fail_message;
	bool changed = false;
	int log_level = MSG_INFO;

	// If a healtcheck has passed, zero the failure counter.
	if (last_state == STATE_UP)
		failure_counter = 0;

	// Change from DOWN to UP. The healthcheck has passed again.
	if (hard_state == STATE_DOWN && last_state == STATE_UP) {
		hard_state = STATE_UP;
		failure_counter = 0;
		changed = true;
		log_level = MSG_STATE_UP;
	}
	// Change from UP to DOWN. The healthcheck has failed.
	else if (hard_state == STATE_UP && last_state == STATE_DOWN) {
		changed = true;
		log_level = MSG_STATE_DOWN;

		failure_counter++;
		fail_message = fmt::sprintf("failure: %d of %d", failure_counter, max_failed_checks);

		// Mark the hard DOWN state only after the number of failed checks is reached.
		if (failure_counter >= max_failed_checks) {
			hard_state = STATE_DOWN;
		}
	}

	if (changed || verbose) {
		log(log_level, this, fmt::sprintf("%s %s", message, fail_message));
	}

	// Mark the check as not running, so it can be scheduled again.
	is_running = false;
	ran = true;
}
