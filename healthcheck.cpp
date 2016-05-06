#include <iostream>
#include <sstream>
#include <typeinfo>
#include <stdlib.h>

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

void Healthcheck::read_confline(istringstream &definition) {
	/*
	 * Read all things from the definition line.  Configuration is
	 * specified by 'var=val' strings.  Each pair is separated by
	 * a space.  There must be no spaces around = character.
	 */
	string confword;

	/*
	 * Rewind to the beginning of line, order of configuration entries
	 * must be irrelevant.
	 */
	definition.clear();
	definition.seekg(0);

	while (definition >> confword) {
		int split = confword.find('=');
		if (split == -1)
			continue;
		string var = confword.substr(0, split);
		istringstream val(confword.substr(split + 1));

		this->confline_callback(var, val);
	}
}

void Healthcheck::confline_callback(string &var, istringstream &val) {
	/*
	 * Temporary timeout in ms, to read from config file and then
	 * store it in this->timeout.  1,5s is the default for
	 * compatibility with old testtool.
	 */
	int tmp_timeout = 1500;

	if (var == "port")
		val >> this->port;
	else if (var == "interval")
		val >> this->check_interval;
	else if (var == "max_failed")
		val >> this->max_failed_checks;
	else if (var == "timeout") {
		val >> tmp_timeout;
		/* Timeout was read in ms, convert it to s and ns. */
		this->timeout.tv_sec   =  tmp_timeout / 1000;
		this->timeout.tv_nsec  = (tmp_timeout % 1000) * 1000 * 1000;
	}
}

/*
 * Link the healthcheck and its parent node, initialize some variables,
 * print diagnostic information if necessary.  Remember that this
 * constructor is called from each healthcheck's type-specific
 * constructor!  And it is called *before* that constructor does its
 * own work!
 */
Healthcheck::Healthcheck(istringstream &definition, class LbNode *_parent_lbnode) {
	/* Set defaults, same as with old testtool. */
	this->check_interval = 2;
	this->max_failed_checks = 3;
	this->timeout.tv_sec   = 1;
	this->timeout.tv_nsec  = 500 * 1000 * 1000;
	this->port = 0; /* Specific healthchecks should set this value. */

	/*
	 * Pretend that the healthcheck was performed just a moment ago.
	 * This is necessary to perform the check in proper time.
	 */
	clock_gettime(CLOCK_MONOTONIC, &last_checked);

	/* Link with parent lbnode. */
	parent_lbnode = _parent_lbnode;
	parent_lbnode->healthchecks.push_back(this);

	this->read_confline(definition);
	/* Random delay to spread healthchecks in space-time continuum. */
	this->extra_delay = rand() % 1000;

	this->is_running = false;

	/*
	 * Initialize healthchecks state basing on state of parent node.
	 * Proper initial state for the healthcheck quarantees no
	 * unnecessary messages.
	 */
	if (parent_lbnode->state() == LbNode::STATE_UP) {
		hard_state      = STATE_UP;
		last_state      = STATE_UP;
		failure_counter = 0;
	} else {
		hard_state      = STATE_DOWN;
		last_state      = STATE_DOWN;
		failure_counter = max_failed_checks;
	}

	log_txt(MSG_TYPE_DEBUG, "    * New healthcheck:");
	log_txt(MSG_TYPE_DEBUG, "      interval: %d, max_failed: %d, interval: %d,%03ds",
		this->check_interval, this->max_failed_checks,
		this->timeout.tv_sec, this->timeout.tv_nsec / 1000000);
}


/*
 * Healthcheck factory:
 *
 * - read type of healthcheck
 * - create an object of the required type, pass the definition to it
 * - return it
*/
Healthcheck *Healthcheck::healthcheck_factory(istringstream &definition, class LbNode *_parent_lbnode) {

	Healthcheck * new_healthcheck = NULL;

	/* Read the check type. */
	string type;
	string confword;
	definition.seekg(0);
	definition.clear();
	while (definition >> confword) {
		int split = confword.find('=');
		if (split == -1)
			continue;
		string var = confword.substr(0, split);
		istringstream val(confword.substr(split + 1));
		if (var == "type")
			val >> type;
	}

	/*
	 * Check the healthcheck type and create a proper object.
	 * Keep in mind that definition is already stripped from
	 * the first word, the healthcheck constructor will read next
	 * words from it.
	 */
	if (type == "http")
		new_healthcheck = new Healthcheck_http(definition, _parent_lbnode);
	else if (type == "https")
		new_healthcheck = new Healthcheck_https(definition, _parent_lbnode);
	else if (type == "tcp")
		new_healthcheck = new Healthcheck_tcp(definition, _parent_lbnode);
	else if (type == "ping")
		new_healthcheck = new Healthcheck_ping(definition, _parent_lbnode);
	else if (type == "postgres")
		new_healthcheck = new Healthcheck_postgres(definition, _parent_lbnode);
	else if (type == "dns")
		new_healthcheck = new Healthcheck_dns(definition, _parent_lbnode);

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
		log_lb(MSG_TYPE_DEBUG, parent_lbnode->parent_lbpool->name.c_str(),
		       parent_lbnode->address.c_str(), port,
		       "Scheduling healthcheck_%s...", type.c_str());

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

	switch (result) {
		case HC_PASS:
			log_type = MSG_TYPE_HC_PASS;
			this->last_state = STATE_UP;
			this->handle_result();
			break;

		case HC_FAIL:
			log_type = MSG_TYPE_HC_FAIL;
			this->last_state = STATE_UP;
			this->handle_result();
			break;

		case HC_ERROR:
			log_type = MSG_TYPE_HC_ERROR;
			this->last_state = STATE_UP;
			this->handle_result();
			break;

		case HC_FATAL:
			log_type = MSG_TYPE_HC_FATAL;
			this->last_state = STATE_DOWN;
			this->hard_state = STATE_DOWN;
			/*
			 * We are leaving "is_running" as true, so it
			 * is not going to be scheduled again.
			 *
			 * TODO Make it more sensible as it is not
			 * actually "running".
			 */
			break;

		case HC_PANIC:
			log_type = MSG_TYPE_HC_PANIC;
			break;
	}

	if (verbose > 1 || this->last_state != this->hard_state || result >= HC_ERROR)
		log_lb(log_type, this->parent_lbnode->parent_lbpool->name.c_str(),
		       this->parent_lbnode->address.c_str(), this->port,
		       "health check %s - %s", this->type.c_str(),
		       message.c_str());

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

	/* Change from DOWN to UP. The healthcheck has passed again. */
	if (hard_state == STATE_DOWN && last_state == STATE_UP) {
		log_lb(MSG_TYPE_HC_PASS, parent_lbnode->parent_lbpool->name.c_str(),
		       parent_lbnode->address.c_str(), port, "passed again");
		hard_state = STATE_UP;
		failure_counter = 0;
	}
	/* Change from UP to DOWN. The healthcheck has failed. */
	else if (hard_state == STATE_UP && last_state == STATE_DOWN) {

		failure_counter++;

		if (!parent_lbnode->downtime)
			log_lb(MSG_TYPE_HC_FAIL,
			       parent_lbnode->parent_lbpool->name.c_str(),
			       parent_lbnode->address.c_str(), port,
			       "failed for the %d time", failure_counter);

		/* Mark the hard DOWN state only after the number of failed checks is reached. */
		if (failure_counter >= max_failed_checks) {
			if (!parent_lbnode->downtime)
				log_lb(MSG_TYPE_HC_HFAIL,
				       parent_lbnode->parent_lbpool->name.c_str(),
				       parent_lbnode->address.c_str(), port,
				       "hard failure reached");

			hard_state = STATE_DOWN;
		}
	}

	/* Mark the check as not running, so it can be scheduled again. */
	is_running = false;
}
