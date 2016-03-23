#ifndef _LB_POOL_H_
#define _LB_POOL_H_

#include <iostream>
#include <list>
#include <map>
#include <string>
#include <vector>

#include "lb_node.h"

using namespace std;

struct LbPoolLink;
class Mechanism;

class LbPool {
	/* Enums */
	public:
		enum State {
			STATE_DOWN = 0,
			STATE_UP   = 1
		};

		/* Fault policy infers the pool state from the set of node states. */
		enum FaultPolicy {
			FORCE_DOWN = 0, /* Pool fails if number of count_live_nodes() < min_nodes */
			FORCE_UP   = 1  /* Last min_nodes nodes will be treated as online, even if they seem down */
		};
		static const map<FaultPolicy, string> fault_policy_names;
		static FaultPolicy fault_policy_by_name(string name);

	/* Methods */ 
	public:
		LbPool(string name, string hwlb, int min_nodes, FaultPolicy fault_policy);

		void schedule_healthchecks(struct timespec *now);
		void parse_healthchecks_results();

		void add_node(LbNode* node);
		size_t count_active_nodes(); /* Nodes that are forced to be part of the pool, regardless of their state. */
		size_t count_live_nodes(); /* Healthy nodes */

		void notify_node_update(LbNode* node, LbNode::State old_state, LbNode::State new_state);

		const set<LbNode*>& active_nodes(); /* Returns active nodes. */

		/* Registers the node with all attached VIPs and returns the initial state. */
		LbNode::State sync_initial_state(LbNode* node);

	private:
		void update_state();
		void update_nodes();

	/* Members */
	public:
		string			 name;
		string			 hwlb;
		State			 state;

		vector<class LbNode*> nodes;
		vector<LbPoolLink*> vips;

	private:
		bool			 m_active; /* Whether the pool is actively used by any VIP. */
		size_t			 m_min_nodes; /* Minimum number of UP hosts (inclusive) before the fault policy kicks in. */
		FaultPolicy		 m_fault_policy;

		/* Set of all nodes that are considered alive for any active VIP connections.
		   These nodes aren't necessarily up, as some fault policies might treat down nodes as up. */
		set<LbNode*>	 m_active_nodes;
};

#endif
