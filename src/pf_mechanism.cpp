#include <algorithm>
#include <vector>

#include "lb_vip.h"
#include "msg.h"
#include "pf_mechanism.h"
#include "pfctl.h"

PfMechanism::PfMechanism(LbVip* vip)
	: Mechanism(vip)
{
}

PfMechanism::~PfMechanism() {
}

void PfMechanism::update_nodes(LbPoolLink* link, const set<LbNode*>& add_nodes, const set<LbNode*>& del_nodes) {
	for (auto node : add_nodes) {
		pf_table_add(link->vip->name, node->address);
	}
	for (auto node : del_nodes) {
		del_node(link, node);
	}

	/* Rebalance if any nodes have been added. */
	if (add_nodes.size() > 0) {
		auto skip_ips = set<string>();
		for (auto node : add_nodes) {
			skip_ips.insert(node->address);
		}
		pf_table_rebalance(link->vip->name, skip_ips);
	}
}

void PfMechanism::add_node(LbPoolLink* link, LbNode* node) {
	pf_table_add(link->vip->name, node->address);

	auto skip_ips = set<string>({node->address});
	pf_table_rebalance(link->vip->name, skip_ips);
}

void PfMechanism::del_node(LbPoolLink* link, LbNode* node) {
	del_address(node->address);
}

void PfMechanism::del_address(string& address) {
	string& table = m_vip->name;

	/* Remove node from table. */
	pf_table_del(table, address);
	/* Kill all src_nodes, linked states and unlinked states. */
	pf_kill_src_nodes_to(table, address, true);
	pf_kill_states_to_rdr(table, address, true);
	/* Kill nodes again, there might be some which were created after last kill
	   due to belonging to states with deferred src_nodes.
	   See TECH-6711 and around. */
	pf_kill_src_nodes_to(table, address, true);
}

bool PfMechanism::has_node(LbPoolLink* link, LbNode* node) {
	return pf_is_in_table(m_vip->name, node->address);
}

void PfMechanism::cleanup_orphans() {
	set<string> allowed_addresses;
	for (auto& kv : m_active_nodes) {
		for (auto node : kv.second) {
			allowed_addresses.insert(node->address);
		}
	}

	set<string> actual_addresses = pf_table_members(m_vip->name);

	vector<string> orphans;
	set_difference(actual_addresses.begin(), actual_addresses.end(), allowed_addresses.begin(), allowed_addresses.end(), inserter(orphans, orphans.begin()));
	for (auto& orphan : orphans) {
		log_txt(MSG_TYPE_PFCTL, "%s - %s - removing orphan node not belonging to any active pool", m_vip->name.c_str(), orphan.c_str());
		del_address(orphan);
	}
}
