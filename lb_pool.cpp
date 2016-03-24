#include <iostream>
#include <sstream>

#include "msg.h"
#include "pfctl.h"

#include "lb_vip.h"
#include "lb_pool.h"
#include "lb_node.h"

using namespace std;

const map<LbPool::FaultPolicy, string> LbPool::fault_policy_names = {
	{FORCE_DOWN, "force_down"},
	{FORCE_UP, "force_up"}
};

LbPool::FaultPolicy LbPool::fault_policy_by_name(string name) {
	for (auto& it : fault_policy_names) {
		if (it.second == name) {
			return it.first;
		}
	}
	log_txt(MSG_TYPE_CRITICAL, "Unknown min_nodes_action '%s'. Falling back to force_down.", name.c_str());
	return FORCE_DOWN;
}

/*
   The constructor has not much work to do, init some variables
   and display the LbPool name if verbose.
*/
LbPool::LbPool(string name, string hwlb, int min_nodes, LbPool::FaultPolicy fault_policy)
	: name(name), hwlb(hwlb)
{
	m_min_nodes = min_nodes;
	m_fault_policy = fault_policy;
	this->state = STATE_DOWN;

	log_txt(MSG_TYPE_DEBUG, "* New LbPool %s on HWLB %s", name.c_str(), hwlb.c_str());
	if (m_min_nodes) {
		auto it = fault_policy_names.find(m_fault_policy);
		string actionstr = (it == fault_policy_names.end()) ? string("") : it->second;
			log_txt(MSG_TYPE_DEBUG, "  action %s below %d nodes", actionstr.c_str(), m_min_nodes);
	}
}


LbNode::State LbPool::sync_initial_state(LbNode* node) {
	auto initial_state = LbNode::STATE_DOWN;
	for (auto link : vips) {
		if (link->mechanism->sync_initial_state(link, node)) {
			initial_state = LbNode::STATE_UP;
		}
	}
	return initial_state;
}


/*
   Go over all healthchecks and try to schedule them.
*/
void LbPool::schedule_healthchecks(struct timespec *now) {
	for (auto node : nodes) {
		node->schedule_healthchecks(now);
	}
}


/*
   Check state of all nodes in this pool. Nodes will notify us about state changes later on.
*/
void LbPool::parse_healthchecks_results() {
	/* Go over all nodes in this pool, each node will gather state from its healthchecks and update its own state. */
	for (auto node : nodes) {
		/* Gather results from node. */
		node->parse_healthchecks_results();
	}
}

void LbPool::update_state() {
	size_t nodes_alive = count_live_nodes();

	/* Determine new state. */
	auto new_state = nodes_alive >= m_min_nodes ? STATE_UP : STATE_DOWN;

	if (new_state == STATE_DOWN && m_fault_policy == FORCE_UP) {
		new_state = STATE_UP;
	}

	if (new_state != this->state) {
		log_txt(MSG_TYPE_DEBUG, "%s - pool changed state: %d -> %d", this->name.c_str(), this->state, new_state);
		this->state = new_state;
	}
	update_nodes();
}


const set<LbNode*>& LbPool::active_nodes() {
	return m_active_nodes;
}

void LbPool::update_nodes() {
	set<LbNode*> new_nodes;
	for (auto node : nodes) {
		if (node->state() == LbNode::STATE_UP) {
			new_nodes.insert(node);
		}

	if (new_nodes.size() >= m_min_nodes) {
		/* If we have a sufficient number of live nodes, just use all of them. */
		log_txt(MSG_TYPE_POOL_UP, "%s - %d/%d nodes alive, pool is UP.", this->name.c_str(), new_nodes.size(), nodes.size());
	} else {
		/* We need more nodes. Now the fault policy comes into play. */
		switch (m_fault_policy) {
		case FORCE_UP:
			/* Pick live nodes, then active ones (which just went down), then random down ones. */
			log_txt(MSG_TYPE_POOL_UP, "%s - %d/%d nodes alive, FORCING UP STATE.", this->name.c_str(), new_nodes.size(), nodes.size());

			for (auto& node : m_active_nodes) {
				if (new_nodes.size() >= m_min_nodes) {
					break;
				}
				if (node->is_downtimed()) {
					continue;
				}
				new_nodes.insert(node);
			}
			for (auto& node : nodes) {
				if (new_nodes.size() >= m_min_nodes) {
					break;
				}
				if (!node->is_downtimed()) {
					continue;
				}
				new_nodes.insert(node);
			}
			break;

		default: /* fallback to FORCE_DOWN */
		case FORCE_DOWN:
			log_txt(MSG_TYPE_POOL_DOWN, "%s - %d/%d nodes alive, pool is DOWN.", this->name.c_str(), new_nodes.size(), nodes.size());
			new_nodes.clear();
			break;
		}
	}

	m_active_nodes = new_nodes;

	/* Notify all VIPs (even inactive ones, they might reactivate themselves.) */
	for (auto link : vips) {
		link->vip->notify_pool_update(link);
	}
}

/*
   Add a node to the pool.
*/
void LbPool::add_node(LbNode* node) {
	nodes.push_back(node);

	if (node->state() == LbNode::STATE_UP) {
		m_active_nodes.insert(node);
	}

	update_state();
}

void LbPool::notify_node_update(LbNode* node, LbNode::State old_state, LbNode::State new_state) {
	update_state();
}

/*
   Count nodes that should be part of the pool.
*/
size_t LbPool::count_active_nodes() {
	return m_active_nodes.size();
}

size_t LbPool::count_live_nodes() {
	size_t nodes_alive = 0;
	for (auto node : nodes) {
		if (node->state() == LbNode::STATE_UP) {
			nodes_alive++;
		}
	}
	return nodes_alive;
}
