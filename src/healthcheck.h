#ifndef _HEALTHCHECK_H_
#define _HEALTHCHECK_H_

#include <iostream>
#include <sstream>
#include <yaml-cpp/yaml.h>

#include <netinet/in.h>

#include <event2/event.h>

#include "lb_node.h"

enum HealthcheckState {
	STATE_DOWN = 0,
	STATE_UP   = 1
};

/*
 * The results health-check implementations can select
 */
enum HealthcheckResult {

	/*
	 * The excpected positive result
	 *
	 * This indicates that everything went fine with the check,
	 * and it is perfecly fine to send traffic to this server.
	 */
	HC_PASS,

	/*
	 * The expected negative result
	 *
	 * This indicates that health-check has failed.  It will be
	 * checked by the system again.  The traffic is not going to be
	 * send to this server, after it fails the configured number
	 * of times.
	 */
	HC_FAIL,

	/*
	 * The unexpected result
	 *
	 * This indicates something wrong with our system.  It can be
	 * returned when the memory allocation fails, for example.
	 * It has nothing to do with the target system.  The system
	 * is not going to make any change, and should reset itself
	 * to recover from this situation.
	 */
	HC_PANIC
};

/*
 * A quite useful macros are available in sys/time.h but
 * only for _KERNEL, at least in FreeBSD.
 */
#define timespecsub(vvp, uvp)						\
	do {								\
		(vvp)->tv_sec -= (uvp)->tv_sec;				\
		(vvp)->tv_nsec -= (uvp)->tv_nsec;			\
		if ((vvp)->tv_nsec < 0) {				\
			(vvp)->tv_sec--;				\
			(vvp)->tv_nsec += 1000000000;			\
		}							\
	} while (0)


using namespace std;

class Healthcheck {

	/* Methods */
	public:
		/* The check factory, which returns proper check object based its name. */
		static Healthcheck *healthcheck_factory(const YAML::Node& config, class LbNode *_parent_lbnode);
		virtual int schedule_healthcheck(struct timespec *now);
		Healthcheck(const YAML::Node&, class LbNode *_parent_lbnode);
		virtual void finalize();

	protected:
		void end_check(HealthcheckResult result, string message);

	private:
		void handle_result(string message);


	/* Members */
	public:
		class LbNode		*parent_lbnode;
		char			 last_state;
		char			 hard_state;
		string			 type;
		bool			 ran;              // This check was ran at least once.

	protected:
		struct timespec		 last_checked;     // The last time this host was checked.
		struct timeval		 timeout;
		bool			 is_running;

	private:
		int			 check_interval;    // Perform a check every n seconds (s).
		unsigned short		 extra_delay;       // Additional delay to spread checks uniformly (ms).
		int			 max_failed_checks; // Take action only after this number of contiguous checks fail.
		unsigned short		 failure_counter;   // This many checks have failed until now.

};

#endif
