#include <fmt/format.h>
#include <yaml-cpp/yaml.h>

#include "config.h"
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
	log(MSG_CRIT, "Unknown min_nodes_action " + name + ", falling back to force_down!");
	return FORCE_DOWN;
}

/*
 * The constructor has not much work to do, init some variables
 * and display the LbPool name if verbose.
 */
LbPool::LbPool(string name, const YAML::Node& config, string proto)
{
	this->proto = proto;
	this->name = name;
	this->hwlb = parse_string(config["hwlb_host"], "");
	this->m_min_nodes = parse_int(config["min_nodes"], 0);
	this->m_max_nodes = parse_int(config["max_nodes"], 0);
	this->m_fault_policy = LbPool::fault_policy_by_name(parse_string(config["min_nodes_action"], "force_down"));

	this->state = STATE_DOWN;

	log(MSG_INFO, this, "new lbpool");

	if (this->m_min_nodes) {
		auto it = fault_policy_names.find(m_fault_policy);
		string actionstr = (it == fault_policy_names.end()) ? string("") : it->second;
		log(MSG_INFO, fmt::sprintf("Action %s below %d nodes", actionstr, this->min_nodes));
	}
}

LbNode::State LbPool::sync_initial_state(LbNode* node) {
	auto initial_state = LbNode::STATE_DOWN;
	for (auto link : this->vips) {
		if (link->mechanism->sync_initial_state(link, node)) {
			initial_state = LbNode::STATE_UP;
		}
	}
	return initial_state;
}

/*
 * Go over all healthchecks and try to schedule them.
 */
void LbPool::schedule_healthchecks(struct timespec *now) {
	for (auto node : this->nodes) {
		node->schedule_healthchecks(now);
	}
}

/*
 * Check state of all nodes in this pool
 *
 * Nodes will notify us about state changes later on.
 */
void LbPool::parse_healthchecks_results() {

	/*
	 * Go over all nodes in this pool, each node will gather state
	 * from its healthchecks and update its own state.
	 */
	for (auto node : this->nodes)
		node->parse_healthchecks_results();
}

bool LbPool::all_active_nodes_up() {
	if (this->m_active_nodes.size() == 0) {
		return false;
	}

	for (auto node : this->m_active_nodes) {
		if (node->state() == LbNode::STATE_DOWN) {
			return false;
		}
	}
	return true;
}

void LbPool::update_state() {
	size_t nodes_alive = this->count_live_nodes();

	// Determine new state
	auto new_state = nodes_alive >= this->m_min_nodes ? STATE_UP : STATE_DOWN;

	if (new_state == STATE_DOWN && this->m_fault_policy == FORCE_UP) {
		new_state = STATE_UP;
	}

	if (new_state != this->state) {
		log_txt(MSG_TYPE_DEBUG, "%s - pool changed state: %d -> %d", this->name.c_str(), this->state, new_state);
		this->state = new_state;
	}

	this->update_nodes();
}

const set<LbNode*>& LbPool::active_nodes() {
	return m_active_nodes;
}

void LbPool::update_nodes() {
	set<LbNode*> new_nodes;

	/*
	 * If max_nodes is set and has same amount as active_nodes and all "active" nodes are UP - we preserve the set
	 */
	if (this->m_max_nodes > 0 && this->m_active_nodes.size() == this->m_max_nodes && this->all_active_nodes_up()) {
		new_nodes = this->m_active_nodes;
	} else {
		for (auto node : this->nodes) {
			if (node->state() == LbNode::STATE_UP) {
				if (this->m_max_nodes > 0 && new_nodes.size() >= this->m_max_nodes) {
					log_txt(MSG_TYPE_POOL_CRIT, "%s - %d/max:%d can't add more nodes",
						this->name.c_str(), new_nodes.size(), this->m_max_nodes);
				} else {
					new_nodes.insert(node);
				}
			}
		}
	}


	/*
	 * If we have a sufficient number of live nodes, we just
	 * use all of them.  If not, we have to find more nodes
	 * using the fault policy.
	 */
	if (new_nodes.size() >= this->m_min_nodes) {
		log_txt(MSG_TYPE_POOL_UP, "%s - %d/%d nodes alive, pool is UP.",
			this->name.c_str(), new_nodes.size(), this->nodes.size());
	} else {
		if (this->m_fault_policy == FORCE_UP) {
			log_txt(MSG_TYPE_POOL_CRIT, "%s - %d/%d nodes alive, FORCING UP STATE.",
				this->name.c_str(), new_nodes.size(), this->nodes.size());

			/*
			 * We need to make the given number of nodes running.
			 * We are starting with the live ones, continuing
			 * with active ones (which just went down), and
			 * then random down ones.
			 */
			for (auto& node : this->m_active_nodes) {
				if (new_nodes.size() >= this->m_min_nodes)
					break;
				if (node->is_downtimed())
					continue;

				new_nodes.insert(node);
			}

			for (auto& node : this->nodes) {
				if (new_nodes.size() >= this->m_min_nodes)
					break;
				if (node->is_downtimed())
					continue;

				new_nodes.insert(node);
			}
		} else {
			log_txt(MSG_TYPE_POOL_DOWN, "%s - %d/%d nodes alive, pool is DOWN.",
				this->name.c_str(), new_nodes.size(), this->nodes.size());
			new_nodes.clear();
		}
	}

	this->m_active_nodes = new_nodes;

	// Notify all VIPs (even inactive ones as they might reactivate themselves)
	for (auto link : this->vips) {
		link->vip->notify_pool_update(link);
	}
}

/*
 * Add a node to the pool
 */
void LbPool::add_node(LbNode* node) {
	this->nodes.push_back(node);

	if (node->state() == LbNode::STATE_UP) {
		this->m_active_nodes.insert(node);
	}

	this->update_state();
}

void LbPool::notify_node_update(LbNode* node, LbNode::State old_state, LbNode::State new_state) {
	this->update_state();
}

/*
 * Count nodes that should be part of the pool
*/
size_t LbPool::count_active_nodes() {
	return m_active_nodes.size();
}

size_t LbPool::count_live_nodes() {
	size_t nodes_alive = 0;

	/*
	 * If m_active_nodes is up to date (nothing has changed and we use min/max nodes functionality)
	 * then we report size of this list.
	 */
	if (this->all_active_nodes_up()) {
		return this->m_active_nodes.size();
	} else {
		for (auto node : this->nodes) {
        		if (node->state() == LbNode::STATE_UP) {
        			nodes_alive++;
        		}
        	}
	}

	return nodes_alive;
}
