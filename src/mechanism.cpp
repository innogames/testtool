#include <algorithm>
#include <iterator>

#include "lb_vip.h"
#include "mechanism.h"
#include "msg.h"

using namespace std;

Mechanism::Mechanism(LbVip* vip)
	: m_vip(vip)
{
}

Mechanism::~Mechanism() {
}

bool Mechanism::sync_initial_state(LbPoolLink* link, LbNode* node) {
	bool has = has_node(link, node);
	if (has) {
		m_active_nodes[link].insert(node);
	}
	return has;
}

void Mechanism::update_nodes(LbPoolLink* link, const set<LbNode*>& add_nodes, const set<LbNode*>& del_nodes) {
	for (auto& node : add_nodes) {
		add_node(link, node);
	}
	for (auto& node : del_nodes) {
		del_node(link, node);
	}

	log_txt(MSG_TYPE_PFCTL, "Mechanism::update_nodes called");
}

void Mechanism::sync_nodes(LbPoolLink* link, const set<LbNode*>& new_nodes) {
	auto& current_nodes = m_active_nodes[link];

	set<LbNode*> add_nodes;
	set<LbNode*> del_nodes;
	if (link->active) {
		set_difference(new_nodes.begin(), new_nodes.end(), current_nodes.begin(), current_nodes.end(), inserter(add_nodes, add_nodes.begin()));
		set_difference(current_nodes.begin(), current_nodes.end(), new_nodes.begin(), new_nodes.end(), inserter(del_nodes, del_nodes.begin()));
	} else {
		del_nodes = current_nodes;
	}

	log_txt(MSG_TYPE_DEBUG, "sync %s@%s - add %ld, del %ld, current %ld", link->pool->name.c_str(), link->vip->name.c_str(), add_nodes.size(), del_nodes.size(), current_nodes.size());

	for (auto node : del_nodes) {
		current_nodes.erase(node);
	}

	for (auto node : add_nodes) {
		current_nodes.insert(node);
	}

	update_nodes(link, add_nodes, del_nodes);
}