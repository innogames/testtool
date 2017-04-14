#include <algorithm>
#include <vector>

#include "msg.h"
#include "lb_pool.h"
#include "pf_mechanism.h"
#include "pfctl.h"

PfMechanism::PfMechanism(LbPool *lbpool) : Mechanism(lbpool) {
}

PfMechanism::~PfMechanism() {
}

void PfMechanism::read_nodes() {

}

void PfMechanism::set_nodes() {
}
