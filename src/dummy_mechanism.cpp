#include "dummy_mechanism.h"
#include "msg.h"

DummyMechanism::DummyMechanism(LbPool* lbpool) : Mechanism(lbpool){
}

DummyMechanism::~DummyMechanism() {
}

void DummyMechanism::read_nodes() {
	log(MSG_INFO, this->parent_lbpool, "DummyMechanism::read_nodes()");
}

void DummyMechanism::set_nodes() {
	log(MSG_INFO, this->parent_lbpool, "DummyMechanism::set_nodes()");
}
