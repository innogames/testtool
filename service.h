#ifndef _SERVICE_H_
#define _SERVICE_H_

#include <iostream>
#include <vector>

#include "healthcheck.h"

using namespace std;

class Service {
	/* Methods */ 
	public:
		Service(std::string &name, int default_hwlb);
		void schedule_healthchecks();
		int count_live_nodes();

	/* Members */
	public:
		string				 name;
		vector<class Healthcheck*>	 healthchecks;
		class Service			*backup_pool;
		unsigned int			 backup_pool_trigger;
		vector<class Service*>		 used_as_backup;
		bool			 	 switched_to_backup;
		unsigned int			 healthchecks_ok;
		bool				 all_down_notified;
		int				 default_hwlb;
	private:
};

#endif

