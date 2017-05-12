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
			FORCE_DOWN =  0, /* Pool fails if up_nodes < min_nodes. */
			FORCE_UP    = 1, /* Last min_nodes nodes will be treated as online, even if they seem down */
			BACKUP_POOL = 2, /* Switch to backup pool. */
		};
		static const map<FaultPolicy, string> fault_policy_names;
		static FaultPolicy fault_policy_by_name(string name);

	/* Methods */ 
	public:
		LbPool(string name, const YAML::Node& config, string proto);

		void schedule_healthchecks(struct timespec *now);
		void parse_healthchecks_results();


	/* Members */
	public:
		string			 name;
		string			 proto;
		string			 ip_address;
		State			 state;
		vector<class LbNode*>	 nodes;

	private:
		size_t			 min_nodes; /* Minimum number of UP hosts (inclusive) before the fault policy kicks in. */
		size_t			 max_nodes; /* Maximum number of UP hosts (inclusive) for security.  0 disables the check.*/
		FaultPolicy		 fault_policy;
		set<string>		 wanted_nodes;
		bool			 wanted_nodes_changed;
};

#endif
