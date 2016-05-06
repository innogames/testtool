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

