#include <iostream>
#include <sstream>
#include <syslog.h>

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
LbNode::LbNode(istringstream &parameters, class LbPool *parent_lbpool) {
	parameters >> address;

	this->downtime = false;
	this->parent_lbpool = parent_lbpool;

	/* Check if the IP address is enabled in mechanism. This determines the initial state of node. */
	m_state = parent_lbpool->sync_initial_state(this);
	m_admin_state = STATE_UP;
	m_pool_state = state();

	this->parent_lbpool->add_node(this);

	log_txt(MSG_TYPE_DEBUG, "  * New LbNode %s, pf_state: %s", address.c_str(), (m_state==STATE_DOWN?"DOWN":"UP"));
}

LbNode::State LbNode::state() {
	if (m_admin_state == STATE_DOWN) {
		return STATE_DOWN;
	} else {
		return m_state;
	}
}


/*
   Notifies the parent pool about a changed node state.
*/
void LbNode::notify_state() {
	auto new_state = state();
	if (m_pool_state != new_state) {
		this->parent_lbpool->notify_node_update(this, m_pool_state, new_state);
		m_pool_state = new_state;
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
	if (m_state == STATE_UP && new_state == STATE_DOWN) {
		m_state = STATE_DOWN;
		log_lb(MSG_TYPE_NODE_DOWN, parent_lbpool->name.c_str(), address.c_str(), 0, "%d of %d checks failed", num_healthchecks-ok_healthchecks, num_healthchecks);
	} else if (m_state == STATE_DOWN && new_state == STATE_UP) {
		m_state = STATE_UP;
		log_lb(MSG_TYPE_NODE_UP, parent_lbpool->name.c_str(), address.c_str(), 0, "all of %d checks passed", num_healthchecks);
	}

	notify_state();
}


/*
   Returns whether a host is downtimed.
*/
bool LbNode::is_downtimed() {
	return m_admin_state == STATE_DOWN;
}


/*
   Start a downtime.
*/
void LbNode::start_downtime() {
	/* Do not mark the node down twice. */
	if (is_downtimed())
		return;

	log_lb(MSG_TYPE_NODE_DOWN, parent_lbpool->name.c_str(), address.c_str(), 0, "starting downtime");

	m_admin_state = STATE_DOWN;
	notify_state();
}


/*
   End a downtime.
*/
void LbNode::end_downtime() {
	/* Remove downtime only once. */
	if (!is_downtimed())
		return;

	log_lb(MSG_TYPE_NODE_UP, parent_lbpool->name.c_str(), address.c_str(), 0, "ending downtime");

	m_admin_state = STATE_UP;

	/* Pretend that this host is fully down. */
	m_state = STATE_DOWN;
	for (unsigned int hc=0; hc<healthchecks.size(); hc++) {
		healthchecks[hc]->force_failure();
	}
	/* No need to notify_state(), since we still pretend to be down. */
}
