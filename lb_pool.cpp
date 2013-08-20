#include <iostream>
#include <sstream>

#include "msg.h"
#include "pfctl.h"

#include "lb_pool.h"
#include "lb_node.h"

using namespace std;


/* Global variables. */
extern bool	 	 pf_action;
extern int	 	 verbose;


/*
   The constructor has not much work to do, init some variables
   and display the LbPool name if verbose.
*/
LbPool::LbPool(istringstream &parameters) {
	backup_pool_trigger = 0;
	parameters >> name >> default_hwlb >> backup_pool_trigger >> backup_pool_names;
	switched_to_backup = false;
	backup_pool = NULL;
	nodes_alive = 0;
	all_down_noticed = false;

	if (verbose>0) {
		cout << "New LbPool " << name << ", HWLB " << default_hwlb;
		if (backup_pool_trigger)
			cout << ", backup_pools " << backup_pool_names << ", trigger " << backup_pool_trigger;
		cout << endl;
	}

}


void LbPool::schedule_healthchecks() {

	for(unsigned int nd=0; nd<nodes.size(); nd++) {
		nodes[nd]->schedule_healthchecks();
	}
}


/*
   Check state of all nodes in this pool and react accordingly.
*/
void LbPool::parse_healthchecks_results() {

	/* Go over all nodes in this pool, each node will perform all required pf operations. */
	for (unsigned int nd=0; nd<nodes.size(); nd++) {
		nodes[nd]->parse_healthchecks_results();
	}

	/* Enough dead nodes to switch to backup pool? */
	if (backup_pool && nodes_alive < backup_pool_trigger && switched_to_backup == false) {
		showStatus(CL_WHITE"%s"CL_RESET" - Less than %d nodes alive, switching to backup_pool "CL_BLUE"%s"CL_RESET"\n", name.c_str(), backup_pool_trigger, backup_pool->name.c_str());

		/* Remove original nodes if there any left, we are switching to backup pool! */
		for (unsigned int nd=0; nd<nodes.size(); nd++) {
			/* Remove only nodes which are STATE_UP, the ones in STATE_DOWN were already removed. */
			if (nodes[nd]->hard_state == STATE_UP) {
				pf_table_del(name, nodes[nd]->address);
				pf_kill_src_nodes_to(nodes[nd]->address, true);
			}
		}

		/* Add backup nodes, should there be any alive. Complain, if not. */
		if (backup_pool->nodes_alive > 0) {
			for (unsigned int nd=0; nd<backup_pool->nodes.size(); nd++) {
				if (backup_pool->nodes[nd]->hard_state == STATE_UP) {
					string address = backup_pool->nodes[nd]->address;
					pf_table_add(name, address); /* Add backup pool's IP to this pool. */
				}
			}
		}

		switched_to_backup = true;
	}
	/* Enough nodes alive to switch back to normal pool? */
	else if (backup_pool && nodes_alive >= backup_pool_trigger && switched_to_backup == true) {
		showStatus(CL_WHITE"%s"CL_RESET" - At least %d nodes alive, removing backup_pool "CL_BLUE"%s"CL_RESET"\n", name.c_str(), backup_pool_trigger, backup_pool->name.c_str());

		/* Add original nodes back when leaving the backup pool mode. */
		for (unsigned int nd=0; nd<nodes.size(); nd++) {
			/* Add only the ones which were not STATE_UP. */
			if (nodes[nd]->hard_state == STATE_UP) {
				pf_table_add(name, nodes[nd]->address);
			}
		}

		/* Remove backup nodes. */
		for (unsigned int nd=0; nd<backup_pool->nodes.size(); nd++) {
			if (backup_pool->nodes[nd]->hard_state == STATE_UP) {
				string address = backup_pool->nodes[nd]->address;
					pf_table_del(name, address);
					pf_kill_src_nodes_to(address, true); /* Kill traffic going to backup pool's node. */
				}
		}

		switched_to_backup = false;
	}
	
	if (nodes_alive <= 0 && all_down_noticed == false) {
		all_down_noticed = true;
		showWarning(CL_WHITE"%s"CL_RESET" - no nodes left to serve the traffic!\n", name.c_str());
	}

}


/*
   Count nodes in hard STATE_UP state.
*/
int LbPool::count_live_nodes() {
	int ret = 0;
	for(unsigned int node=0; node<nodes.size(); node++) {
		if (nodes[node]->hard_state == STATE_UP)
			ret++;
	}

	return ret;
}


