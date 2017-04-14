#include <algorithm>
#include <iterator>

#include "lb_pool.h"
#include "mechanism.h"
#include "msg.h"

using namespace std;

Mechanism::Mechanism(LbPool* lbpool) {
	this->parent_lbpool = lbpool;
}

Mechanism::~Mechanism() {
}


void Mechanism::read_nodes() {
}

void Mechanism::set_nodes() {
}
