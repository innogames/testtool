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
	/* Remove node from table. */
	pf_table_del(link->vip->name, node->address);
	/* Kill all src_nodes, linked states and unlinked states. */
	pf_kill_src_nodes_to(link->vip->name, node->address, true);
	pf_kill_states_to_rdr(link->vip->name, node->address, true);
	/* Kill nodes again, there might be some which were created after last kill
	   due to belonging to states with deferred src_nodes.
	   See TECH-6711 and around. */
	pf_kill_src_nodes_to(link->vip->name, node->address, true);
}

bool PfMechanism::has_node(LbPoolLink* link, LbNode* node) {
	return pf_is_in_table(m_vip->name, node->address);
}
