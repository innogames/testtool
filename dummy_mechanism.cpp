#include "dummy_mechanism.h"
#include "msg.h"

DummyMechanism::DummyMechanism(LbVip* vip)
	: Mechanism(vip)
{
}

DummyMechanism::~DummyMechanism() {
}

void DummyMechanism::add_node(LbPoolLink* link, LbNode* node) {
	log_txt(MSG_TYPE_PFCTL, "add_node: %s", node->address.c_str());
}

void DummyMechanism::del_node(LbPoolLink* link, LbNode* node) {
	log_txt(MSG_TYPE_PFCTL, "del_node: %s", node->address.c_str());
}

bool DummyMechanism::has_node(LbPoolLink* link, LbNode* node) {
	bool has = false;
	log_txt(MSG_TYPE_PFCTL, "has_node: %s - let's just say %s.", node->address.c_str(), has?"yes":"no");
	return has;
}