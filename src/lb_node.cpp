#include <syslog.h>
#include <fmt/format.h>

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
LbNode::LbNode(string name, const YAML::Node& config, class LbPool *parent_lbpool, std::string proto) {
	this->name = name;
	this->address = config["ip" + proto].as<std::string>();

	this->downtime = false;
	this->parent_lbpool = parent_lbpool;

	this->admin_state = STATE_UP;
	bool pf_state = STATE_DOWN;
	pf_is_in_table(&this->name, &this->address, &pf_state);

	if (pf_state)
		this->state = STATE_UP;
	else
		this->state = STATE_DOWN;

	this->parent_lbpool->nodes.push_back(this);

	log(MSG_INFO, this, fmt::sprintf("state %s created", (this->get_state()==STATE_DOWN?"DOWN":"UP")));
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
	if (is_downtimed()) {
		return;
	}

	for(unsigned int hc=0; hc<healthchecks.size(); hc++) {
		healthchecks[hc]->schedule_healthcheck(now);
	}
}


/*
   Check results of all healthchecks for this node and act accordingly:
   - set hard_state
   - display messages
   - notify pool about state changes
*/
void LbNode::parse_healthchecks_results() {
	if (is_downtimed()) {
		return;
	}

	unsigned int num_healthchecks = healthchecks.size();
	unsigned int ok_healthchecks = 0;

	/* Some typs of healthchecks might require additional work to fully finish their job.
	   This is particulary true for ping test which requires manual intervetion to check for timeouts. */
	for (auto& hc : healthchecks) {
		hc->finalize_result();
	}

	/* If there is no downtime, go over all healthchecks for this node and count hard STATE_UP healthchecks. */
	for (auto& hc : healthchecks) {
		if (hc->hard_state == STATE_UP) {
			ok_healthchecks++;
		}
	}

	/* Fill in node state basing on passed healthchecks. Display information.
	   Log and update pool if state changed. There is no need to check for downtimes. */
	auto new_state = (ok_healthchecks < num_healthchecks) ? STATE_DOWN : STATE_UP;
	if (state == STATE_UP && new_state == STATE_DOWN) {
		state = STATE_DOWN;
		log(MSG_STATE_DOWN, this, fmt::sprintf("%d of %d checks failed", num_healthchecks-ok_healthchecks, num_healthchecks));
	} else if (state == STATE_DOWN && new_state == STATE_UP) {
		state = STATE_UP;
		log(MSG_STATE_DOWN, this, fmt::sprintf("all of %d checks passed", num_healthchecks));
	}
}


/*
   Returns whether a host is downtimed.
*/
bool LbNode::is_downtimed() {
	return admin_state == STATE_DOWN;
}


/*
   Start a downtime.
*/
void LbNode::start_downtime() {
	/* Do not mark the node down twice. */
	if (is_downtimed())
		return;

	log(MSG_STATE_DOWN, this, "starting downtime");

	admin_state = STATE_DOWN;
}


/*
   End a downtime.
*/
void LbNode::end_downtime() {
	/* Remove downtime only once. */
	if (!is_downtimed())
		return;

	log(MSG_STATE_UP, this, "ending downtime");

	admin_state = STATE_UP;
	state = STATE_DOWN;

	/* Pretend that this host is fully down. */
	for (unsigned int hc=0; hc<healthchecks.size(); hc++) {
		healthchecks[hc]->force_failure();
	}
}
