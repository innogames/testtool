#include <fmt/format.h>
#include <yaml-cpp/yaml.h>

#include "config.h"
#include "pfctl.h"
#include "msg.h"

#include "lb_pool.h"
#include "lb_node.h"

using namespace std;

const map<LbPool::FaultPolicy, string> LbPool::fault_policy_names = {
	{FORCE_DOWN, "force_down"},
	{FORCE_UP, "force_up"},
	{BACKUP_POOL, "backup_pool"},
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
LbPool::LbPool(string name, const YAML::Node& config, string proto, set<string> *downtimes, map<std::string, LbPool*> *all_lb_pools) {
	this->proto = proto;
	this->all_lb_pools = all_lb_pools;

	/* Perform some checks to verify if this is really an LB Pool */
	if  (!node_defined(config["ip"+proto])) {
		throw(NotLbPoolException(fmt::sprintf("No ip%s configured!", proto)));
	}
	if  (!node_defined(config["protocol_port"])) {
		throw(NotLbPoolException("No protocol_port configured!"));
	}
	if  (!node_defined(config["pf_name"])) {
		throw(NotLbPoolException("No pf_name configured!"));
	}
	this->name = name;
	this->ip_address = config["ip" + proto].as<std::string>();
	this->pf_name = config["pf_name"].as<std::string>() + "_" + proto;
	this->min_nodes = parse_int(config["min_nodes"], 0);
	this->max_nodes = parse_int(config["max_nodes"], 0);
	if (max_nodes < min_nodes) {
		max_nodes = min_nodes;
	}
	this->fault_policy = LbPool::fault_policy_by_name(parse_string(config["min_nodes_action"], "force_down"));
	this->backup_pool_name = parse_string(config["backup_pool"], "");
	if (this->backup_pool_name != "") {
		this->fault_policy = BACKUP_POOL;
		this->backup_pool_name = this->backup_pool_name + "_" + proto;
	}

	this->state = STATE_DOWN;

	auto it = fault_policy_names.find(fault_policy);
	string fault_policy_name = (it == fault_policy_names.end()) ? string("") : it->second;

	log(MSG_INFO, this, fmt::sprintf("min %d max %d policy %s action: created", min_nodes, max_nodes, fault_policy_name));

	/*
	 * Glue things together. Please note that children append themselves
	 * to property of parent in their own code.
	 */
	for (auto lbnode_it : config["nodes"]) {
		if ( ! lbnode_it.second["ip" + proto].IsNull()) {
			new LbNode(lbnode_it.first.as<std::string>(), lbnode_it.second, this, proto, downtimes);
		}
	}
	for (auto hc_it : config["healthchecks"]) {
		for (auto node : this->nodes) {
			Healthcheck::healthcheck_factory(hc_it, node);
		}
	}

	/*
	 * State of nodes loaded from pf must be verified. Maybe it is empty
	 * in pf and must have min_nodes added? Maybe it contains entries which
	 * are not in config file anymore? Maybe it has no checks assigned?
	 */
	pool_logic(NULL);
}

/*
 * Go over all healthchecks and try to schedule them.
 */
void LbPool::schedule_healthchecks(struct timespec *now) {
	for (auto node : this->nodes) {
		node->schedule_healthchecks(now);
	}
}

size_t LbPool::count_up_nodes() {
	return up_nodes.size();
}

string LbPool::get_backup_pool_state() {
	if (backup_pool_name == "")
		return "none";
	if (backup_pool_active)
		return "active";
	return "configured";
}

/*
 * Check state of all nodes in this pool
 */
void LbPool::parse_healthchecks_results() {

	/*
	 * Go over all nodes in this pool, each node will gather state
	 * from its healthchecks and update its own state.
	 */
	for (auto node : this->nodes)
		node->parse_healthchecks_results();
}

void LbPool::pool_logic(LbNode *last_node) {
	/*
	 * Now that checks are know, operations can be performed on pf.
	 * I tried to come multiple times with a differential algorithm.
	 * I always failed because of:
	 * - To satisfy min_nodes iteration over other nodes was necessary.
	 * - They could not properly fill in the pool on program startup.
	 * So here is an algorithm that always builds set of wanted nodes from
	 * scratch.
	 */
        set<LbNode*> wanted_nodes;

	/*
	 * Try running without backup pool. Enable it only if not enough up
	 * nodes are found.
	 */
	backup_pool_active = false;

	/*
	 * Add nodes while satisfying max_nodes if it is set. First add nodes
	 * which were added on previous change in order to avoid rebalancing.
	 */
	for (auto node: nodes) {
		if (node->max_nodes_kept == true) {
			if (node->get_state() == node->STATE_UP && (max_nodes == 0 || wanted_nodes.size() < max_nodes)) {
				wanted_nodes.insert(node);
			}
		}
	}
	/* Then add other active nodes if still possible within max_nodes limit. */
	for (auto node: nodes) {
		if (node->max_nodes_kept == false) {
			if (node->get_state() == node->STATE_UP && (max_nodes == 0 || wanted_nodes.size() < max_nodes)) {
				wanted_nodes.insert(node);
				node->max_nodes_kept = true;
			}
		}
	}
	/* Now satisfy minNodes depending on its configuration */
	if (min_nodes > 0 && wanted_nodes.size() < min_nodes) {
		if (fault_policy == FORCE_DOWN) {
			/*
			 * If there is not enough nodes, bring the whole pool down.
			 */
			wanted_nodes.clear();
		} else if (fault_policy == FORCE_UP) {
			/*
			 * More nodes must be added to pool even if they are
			 * down. Similar thing as above: start with nodes which
			 * where in pool on previous change. And even before
			 * that start with the node from which pool_logic was
			 * called.
			 */
			if (last_node) {
				last_node->min_nodes_kept = true;
			}
			for (auto node: nodes) {
				if (node->is_downtimed() == false && node->min_nodes_kept && wanted_nodes.size() < min_nodes) {
					wanted_nodes.insert(node);
				}
			}
			for (auto node: nodes) {
				if (node->is_downtimed() == false && wanted_nodes.size() < min_nodes) {
					wanted_nodes.insert(node);
					node->min_nodes_kept = true;
				}
			}
		} else if (fault_policy == BACKUP_POOL) {
			if (all_lb_pools->find(backup_pool_name) != all_lb_pools->end()) {
				log(MSG_INFO, this, fmt::sprintf("Switching to backup pool %s", backup_pool_name));
				wanted_nodes = all_lb_pools->find(backup_pool_name)->second->up_nodes;
				backup_pool_active = true;
			} else {
				log(MSG_CRIT, this, fmt::sprintf("No LB Pool '%s' to use as backup pool!", backup_pool_name));
			}
		}
	}

	if (wanted_nodes != up_nodes) {
		up_nodes = wanted_nodes;
		up_nodes_changed = true;
	}

	log(MSG_INFO, this, fmt::sprintf("%d/%d nodes up", up_nodes.size(), nodes.size()));
	for (auto node: up_nodes){
		log(MSG_INFO, this, fmt::sprintf("up_node: %s", node->name));
	}

	/* Pool state will be used for configuring BGP */
	if (up_nodes.empty()) {
		state = STATE_DOWN;
	} else {
		state = STATE_UP;
	}
}


/*
 * Update pfctl to last known wanted_nodes if necessary.
 */
void LbPool::update_pfctl(void) {
	if (up_nodes_changed == false) {
		return;
	}

	set<string> wanted_addresses;

	for (auto node: up_nodes) {
		wanted_addresses.insert(node->address);
	}

	pf_sync_table(&pf_name, &wanted_addresses);

	for (auto& lb_pool: *all_lb_pools) {
		if (lb_pool.second->backup_pool_active && lb_pool.second->backup_pool_name == name) {
			log(MSG_INFO, this, fmt::sprintf("Updating backup pool of %s", lb_pool.second->name));
			pf_sync_table(&lb_pool.second->pf_name, &wanted_addresses);
		}
	}

	up_nodes_changed = false;
}
