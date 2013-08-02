#ifndef _HEALTHCHECK_H_
#define _HEALTHCHECK_H_

#include <iostream>

#include <event2/event.h>
#include <netinet/in.h>

#include "service.h"

#define STATE_DOWN       0
#define STATE_UP         1

#define RESULT_OK        0
#define RESULT_FAILED	 1

#define CONNECTION_OK      1
#define CONNECTION_TIMEOUT 2
#define CONNECTION_FAILED  3

/* Those quite useful macros are available in sys/time.h but
   only for _KERNEL, at least in FreeBSD. */
#define	timespecclear(tvp)	((tvp)->tv_sec = (tvp)->tv_nsec = 0)
#define	timespecisset(tvp)	((tvp)->tv_sec || (tvp)->tv_nsec)
#define	timespeccmp(tvp, uvp, cmp)					\
	(((tvp)->tv_sec == (uvp)->tv_sec) ?				\
	    ((tvp)->tv_nsec cmp (uvp)->tv_nsec) :			\
	    ((tvp)->tv_sec cmp (uvp)->tv_sec))
#define timespecadd(vvp, uvp)						\
	do {								\
		(vvp)->tv_sec += (uvp)->tv_sec;				\
		(vvp)->tv_nsec += (uvp)->tv_nsec;			\
		if ((vvp)->tv_nsec >= 1000000000) {			\
			(vvp)->tv_sec++;				\
			(vvp)->tv_nsec -= 1000000000;			\
		}							\
	} while (0)
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
		static Healthcheck *healthcheck_factory(string &definition, class Service &service);
		virtual int schedule_healthcheck();
		Healthcheck(string &parameters, class Service &service);
		void handle_result();
		void start_downtime();
		void end_downtime();

	/* Members */
	public:
		class Service		*parent;
		char			 last_state;
		char			 hard_state;
		string			 type;
		string			 address; /* Address and port are not of any "network" type */
		int			 port;    /* because they are often printed and libevent expects them to be char[] and int. */

	private:
		int			 check_interval;   // Perform a test every n seconds (s).
		unsigned short		 extra_delay;      // Additional delay to spread tests uniformly (ms).
		int			 max_failed_tests; // Take action only after this number of contiguous tests fail.
		unsigned short		 failure_counter;  // This many tests have failed until now.
		bool			 downtime;

	protected:
		struct timespec		 last_checked;     // The last time this host was checked.
		struct timespec		 timeout;
		string			 parameters;
		bool			 is_running;
};

#endif

