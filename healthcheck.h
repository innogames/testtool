#ifndef _HEALTHCHECK_H_
#define _HEALTHCHECK_H_

#include <iostream>
#include <sstream>

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
	 * The unexpected negative result
	 *
	 * This indicates that health-check has failed, because of
	 * an unexpected error like misconfiguration.  It behaves
	 * exactly same as HC_FAIL, but we can detect unexpected
	 * problems using this state.
	 */
	HC_ERROR,

	/*
	 * The terminating failure
	 *
	 * This indicates that the health-check has some problem which
	 * cannot be fixed on its own.  There is no point of checking
	 * it again.  The system is going to stop sending traffic to
	 * this server immediately, and it is not going to try checking
	 * its health again.  This is a useful result to return on
	 * mis-configurations like entering an out-of-range port
	 * number.
	 */
	HC_FATAL,

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


/* Those quite useful macros are available in sys/time.h but
   only for _KERNEL, at least in FreeBSD. */
#define	timespeccmp(tvp, uvp, cmp)					\
	(((tvp)->tv_sec == (uvp)->tv_sec) ?				\
	    ((tvp)->tv_nsec cmp (uvp)->tv_nsec) :			\
	    ((tvp)->tv_sec cmp (uvp)->tv_sec))
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
		static Healthcheck *healthcheck_factory(istringstream &definition, class LbNode *_parent_lbnode);
		virtual int schedule_healthcheck(struct timespec *now);
		Healthcheck(istringstream &definition, class LbNode *_parent_lbnode);
		void handle_result();
		void force_failure();
		virtual void finalize_result();

	protected:
		void read_confline(istringstream &definition);
		void end_check(HealthcheckResult result, string message);
	private:
		virtual void confline_callback(string &var, istringstream &val);


	/* Members */
	public:
		class LbNode		*parent_lbnode;
		char			 last_state;
		char			 hard_state;
		string			 type;

	protected:
		int			 port;             // Healthchecks assigned to one node can be performed against multiple ports. */
		struct timespec		 last_checked;     // The last time this host was checked.
		struct timespec		 timeout;
		bool			 is_running;

	private:
		int			 check_interval;    // Perform a check every n seconds (s).
		unsigned short		 extra_delay;       // Additional delay to spread checks uniformly (ms).
		int			 max_failed_checks; // Take action only after this number of contiguous checks fail.
		unsigned short		 failure_counter;   // This many checks have failed until now.

};

#endif
