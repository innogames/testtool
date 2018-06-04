#include <syslog.h>
#include <fmt/format.h>
#include <fmt/printf.h>

#include "msg.h"
#include "pfctl.h"

#include "lb_pool.h"
#include "lb_node.h"
#include "healthcheck.h"

using namespace std;


/* Global variables. */
extern bool	 	 pf_action;
extern int	 	 verbose;


/*
   Link the node and its parent pool, initialize some variables, print diagnostic information if necessary.
*/
LbNode::LbNode(string name, const YAML::Node& config, class LbPool *parent_lbpool, std::string proto, set<string> *downtimes) {
	this->name = name;
	this->address = config["ip" + proto].as<std::string>();

	this->parent_lbpool = parent_lbpool;

	this->admin_state = STATE_UP;
	/* Read initial state of host from pf. */
	bool pf_state = false;
	pf_is_in_table(&this->parent_lbpool->pf_name, &this->address, &pf_state);
	if (pf_state)
		this->state = STATE_UP;
	else
		this->state = STATE_DOWN;
	this->checked = false;
	this->min_nodes_kept = false;
	this->max_nodes_kept = false;

	this->parent_lbpool->nodes.push_back(this);

	if (downtimes->count(parent_lbpool->pf_name + " " + this->address)) {
		this->admin_state = STATE_DOWN;
	}

	/*
	 * Determine type of IP address given to this node.
	 * Checks based on functions of libevent take IP address in string form
	 * and perform their own magic. Custom checks like tcp or ping operate
	 * on old style structures and have different code for each address
	 * family so we can as well help them.
	 */
	struct addrinfo hint, *res = NULL;
	int ret;

	memset(&hint, 0, sizeof hint);
	hint.ai_family = PF_UNSPEC;
	hint.ai_flags = AI_NUMERICHOST;
	ret = getaddrinfo(this->address.c_str(), NULL, &hint, &res);
	if (ret) {
		// We should throw an exception.
	} else {
		this->address_family = res->ai_family;
		freeaddrinfo(res);
	}

	log(MSG_INFO, this, fmt::sprintf("state: created initial_state: %s", this->get_state_text()));
}


string LbNode::get_state_text() {
	if (admin_state == STATE_DOWN)
		return "DOWNTIME";
	if (state == STATE_UP)
		return "UP";
	return "DOWN";
}


LbNode::State LbNode::get_state() {
	if (admin_state == STATE_DOWN) {
		return STATE_DOWN;
	} else {
		return state;
	}
}


/*
   Try to schedule all healthcheck of this node. Do not try if there is a downtime for this node.
*/
void LbNode::schedule_healthchecks(struct timespec *now) {
	for(unsigned int hc=0; hc<healthchecks.size(); hc++) {
		healthchecks[hc]->schedule_healthcheck(now);
	}
}


/*
 * Some types of Healthchecks might require additional work to fully finish
 * their job. This is particulary true for ping test which requires manual
 * intervetion to check for timeouts.
 */
void LbNode::finalize_healthchecks() {
	for (auto& hc : healthchecks) {
		hc->finalize();
	}
}


/*
   Check results of all healthchecks for this node and act accordingly:
   - set hard_state
   - display messages
   - notify pool about state changes
*/
void LbNode::node_logic() {
	unsigned int num_healthchecks = healthchecks.size();
	unsigned int ok_healthchecks = 0;

	/* Go over all healthchecks for this node and count hard STATE_UP healthchecks. */
	for (auto& hc : healthchecks) {
		if (hc->ran)
			checked = true;
		if (hc->hard_state == STATE_UP)
			ok_healthchecks++;
	}

	/* Do not check all healthchecks and don't notify pool until all check report at least once */
	if (!checked)
		return;

	/* Fill in node state basing on passed healthchecks. Display information.
	   Log and update pool if state changed. There is no need to check for downtimes. */
	state_changed = false;
	auto new_state = (ok_healthchecks < num_healthchecks) ? STATE_DOWN : STATE_UP;
	if (state == STATE_UP && new_state == STATE_DOWN) {
		state_changed = true;
		max_nodes_kept = false;
		state = STATE_DOWN;
		log(MSG_STATE_DOWN, this, fmt::sprintf("message: %d of %d checks failed", num_healthchecks-ok_healthchecks, num_healthchecks));
	} else if (state == STATE_DOWN && new_state == STATE_UP) {
		state_changed = true;
		min_nodes_kept = false;
		state = STATE_UP;
		log(MSG_STATE_DOWN, this, fmt::sprintf("message: all of %d checks passed", num_healthchecks));
	}

	/*
	 * Notify parent pool. Do it always, no matter if there was change or not.
	 * The pool might not be synced if pfctl was busy.
	 */
	parent_lbpool -> pool_logic(this);
}


/*
   Start a downtime.
*/
void LbNode::start_downtime() {
	/* Start downtime only once. */
	if (admin_state == STATE_DOWN)
		return;

	log(MSG_STATE_DOWN, this, "downtime: starting");

	admin_state = STATE_DOWN;
	this->state_changed = true;
	max_nodes_kept = false;
	/* Call pool logic. It will detect a down node and remove it. */
	parent_lbpool -> pool_logic(this);
}


/*
   End a downtime.
*/
void LbNode::end_downtime() {
	/* Remove downtime only once. */
	if (admin_state == STATE_UP)
		return;

	log(MSG_STATE_UP, this, "downtime: ending");

	admin_state = STATE_UP;
	this->state_changed = true;
	parent_lbpool -> pool_logic(this);
}
