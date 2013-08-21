#ifndef _LB_POOL_H_
#define _LB_POOL_H_

#include <iostream>
#include <vector>

#include "lb_node.h"

using namespace std;

class LbPool {
	/* Methods */ 
	public:
		LbPool(istringstream &parameters);
		void schedule_healthchecks();
		void parse_healthchecks_results();
		int count_live_nodes();

	/* Members */
	public:
		string				 name;
		vector<class LbNode*>		 nodes;
		int				 default_hwlb;
		bool			 	 switched_to_backup;
		unsigned int			 backup_pool_trigger;
		class LbPool			*backup_pool;
		string				 backup_pool_names; /* Temporary place to store the list (coma-separated string) of possible backup_pools. */
		vector<class LbPool*>		 used_as_backup;
		unsigned int			 nodes_alive;
		bool				 all_down_noticed;
	private:
};

#endif

