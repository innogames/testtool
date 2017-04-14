#include <fmt/format.h>
#include <yaml-cpp/yaml.h>

#include "config.h"
#include "msg.h"

#include "lb_pool.h"
#include "lb_node.h"
#include "pf_mechanism.h"

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
	this->mechanism = new PfMechanism(this);
	this->proto = proto;
	this->name = name;
	this->min_nodes = parse_int(config["min_nodes"], 0);
	this->max_nodes = parse_int(config["max_nodes"], 0);
	this->fault_policy = LbPool::fault_policy_by_name(parse_string(config["min_nodes_action"], "force_down"));

	this->state = STATE_DOWN;

	log(MSG_INFO, this, "new lbpool");

	if (this->min_nodes) {
		auto it = fault_policy_names.find(fault_policy);
		string actionstr = (it == fault_policy_names.end()) ? string("") : it->second;
		log(MSG_INFO, fmt::sprintf("Action %s below %d nodes", actionstr, this->min_nodes));
	}

	/*
	 * Glue things together. Please note that children append themselves
	 * to property of parent in their own code.
	 */
	for (
		YAML::const_iterator lbnode_it = config["nodes"].begin();
		lbnode_it != config["nodes"].end();
		lbnode_it++
	) {
		if ( ! lbnode_it->second["ip" + proto].IsNull()) {
			new LbNode(lbnode_it->first.as<std::string>(), lbnode_it->second, this, proto);
		}
	}
	for (size_t i = 0; i < config["healthchecks"].size(); i++) {
		const YAML::Node& hc_config = config["healthchecks"][i];
		for (auto node : this->nodes) {
			Healthcheck::healthcheck_factory(hc_config, node);
		}
	}
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
 */
void LbPool::parse_healthchecks_results() {

	/*
	 * Go over all nodes in this pool, each node will gather state
	 * from its healthchecks and update its own state.
	 */
	for (auto node : this->nodes)
		node->parse_healthchecks_results();
}

