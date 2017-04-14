#ifndef _LB_NODE_H_
#define _LB_NODE_H_

#include <iostream>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "healthcheck.h"
#include "mechanism.h"

using namespace std;

class LbNode {
	/* Enums */
	public:
	enum State {
		STATE_DOWN = 0,
		STATE_UP   = 1
	};

	/* Methods */
	public:
		LbNode(string name, const YAML::Node& config, class LbPool *parent_lbpool, std::string proto);
		void schedule_healthchecks(struct timespec *now);
		void parse_healthchecks_results();

		void start_downtime();
		void end_downtime();
		bool is_downtimed();

		State get_state(); /* getter for private member */

	/* Members */
	public:
		string			 name;
		string			 address; /* libevent wants the address passed as char[] so keep to some string-like. */
		class LbPool		*parent_lbpool;
		State			 state;
		vector<class Healthcheck*> healthchecks;
		bool			 downtime;

	private:
		State			 admin_state;
		bool			 backup;
};

#endif

