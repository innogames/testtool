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

using namespace std;

class Healthcheck {
	/* Methods */
	public:
		/* The check factory, which returns proper check object based its name. */
		static Healthcheck *healthcheck_factory(string &definition, class Service &service);
		bool can_run_now();
		virtual int schedule_healthcheck();
		Healthcheck(string &parameters, class Service &service);
		void handle_result();

	/* Members */
	public:
		class Service		*parent;
		char			 last_state;
		char			 hard_state;
		string			 type;
		string			 address; /* Address and port are not of any "network" type */
		int			 port;    /* because they are often printed and libevent expects them to be char[] and int. */

	private:
		struct timespec		 last_checked;     // The last time this host was checked.
		int			 check_interval;   // Perform a test every n seconds (s).
		unsigned short		 extra_delay;      // Additional delay to spread tests uniformly (ms).
		int			 max_failed_tests; // Take action only after this number of contiguous tests fail.
		unsigned short		 failure_counter;  // This many tests have failed until now.

	protected:
		string			 parameters;
		struct timeval		 timeout;
		bool			 is_running;
};

#endif

