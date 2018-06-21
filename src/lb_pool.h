/*
 * Testtool - Load Balancing Pool
 *
 * Copyright (c) 2018 InnoGames GmbH
 */

#ifndef _LB_POOL_H_
#define _LB_POOL_H_

#include <iostream>
#include <exception>
#include <list>
#include <map>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "lb_node.h"

using namespace std;

class NotLbPoolException {
	public:
		NotLbPoolException(const string& msg) : msg_(msg) {};
		~NotLbPoolException( ) {};
		const char* what ( ) {return(msg_.c_str());}
	private:
		string msg_;

};


class LbPool {
	/* Enums */
	public:
		enum State {
			STATE_DOWN = 0,
			STATE_UP   = 1
		};

		/* Fault policy infers the pool state from the set of node states. */
		enum FaultPolicy {
			FORCE_DOWN  = 0, /* Pool fails if up_nodes < min_nodes. */
			FORCE_UP    = 1, /* Last min_nodes nodes will be treated as online, even if they seem down */
			BACKUP_POOL = 2, /* Switch to backup pool. */
		};
		static const map<FaultPolicy, string> fault_policy_names;
		static FaultPolicy fault_policy_by_name(string name);

	/* Methods */ 
	public:
		LbPool(string name, const YAML::Node& config, string proto, set<string> *downtimes, map<std::string, LbPool*> *all_lb_pools);
		void schedule_healthchecks(struct timespec *now);
		void pool_logic(LbNode *last_node);
		void finalize_healthchecks();
		void update_pfctl();
		size_t count_up_nodes();
		string get_backup_pool_state();


	/* Members */
	public:
		string			 name;
		string			 proto;
		string			 ip_address;
		string			 pf_name;
		State			 state;
		vector<class LbNode*>	 nodes;
		set<class LbNode*>	 up_nodes;

	private:
		string			 backup_pool_name;
		bool			 backup_pool_active;
		size_t			 min_nodes; /* Minimum number of UP hosts (inclusive) before the fault policy kicks in. */
		size_t			 max_nodes; /* Maximum number of UP hosts (inclusive) for security.  0 disables the check.*/
		FaultPolicy		 fault_policy;
		map<std::string, LbPool*> *all_lb_pools;
		bool			 pf_synced;
};

#endif
