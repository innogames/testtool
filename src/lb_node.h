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
		LbNode(const YAML::Node& config, class LbPool *parent_lbpool, std::string proto);
		void schedule_healthchecks(struct timespec *now);
		void parse_healthchecks_results();

		void start_downtime();
		void end_downtime();
		bool is_downtimed();

		State state();

	private:
		void notify_state();

	/* Members */
	public:
		string			 address; /* libevent wants the address passed as char[] so keep to some string-like. */

		vector<class Healthcheck*> healthchecks;
		class LbPool		*parent_lbpool;
		bool			 downtime;

	private:
		State			 m_state; /* Current state of node. */
		State			 m_admin_state; /* Admin state (= downtime). */
		State			 m_pool_state; /* Last state reported to LbPool. */
};

#endif

