#ifndef _LB_NODE_H_
#define _LB_NODE_H_

#include <iostream>
#include <vector>

#include "healthcheck.h"

using namespace std;

class LbNode {
	/* Methods */
	public:
		LbNode(istringstream &parameters, class LbPool *_parent_lbpool);
		void schedule_healthchecks();
		void parse_healthchecks_results();
		void start_downtime();
		void end_downtime();

	private:

	/* Members */
	public:
		string			 address; /* libevent wants the address passed as char[] so keep to some string-like */
		char			 hard_state; /* Same as for healthcheck, this is to acknowledge that action was performed on the node after its state has changed. */
		vector<class Healthcheck*> healthchecks;
		class LbPool		*parent_lbpool;
		bool			 downtime;

	private:
};

#endif

