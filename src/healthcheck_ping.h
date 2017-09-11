#ifndef _CHECK_PING_H_
#define _CHECK_PING_H_

#include <vector>
#include <sstream>
#include <yaml-cpp/yaml.h>

#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/util.h>

#include "healthcheck.h"

#define ICMP_FILL_DATA "Dave, this conversation can serve no purpose anymore. Goodbye."

#define ICMP_FILL_SIZE sizeof(ICMP_FILL_DATA)

/* As far as I understand, the target system should just reflect the packet to us,
   therefore apart from some basic, necessary headers, the rest of packet is all
   usable for any purposes we want. */
struct icmp_echo_struct {
	struct icmp		 icmp_header; /* Contains icmp_type, _code, _cksum, _id and _seq and some other stuff. */
	struct timespec		 timestamp;   /* I'm gonna go build my own timestamp. With blackjack and hookers! */
	char			 data[ICMP_FILL_SIZE];
};


class Healthcheck_ping: public Healthcheck {

	/* Methods */
	public:
		Healthcheck_ping(const YAML::Node& config, class LbNode *_parent_lbnode);
		int schedule_healthcheck(struct timespec *now);
		static int initialize();
		static void destroy();
		void finalize_result();


	protected:
		void end_check(HealthcheckResult result, string message);
		static void callback(evutil_socket_t fd, short what, void *arg);

	private:


	/* Members */
	private:
		/* Some variables and functions are static for all ping healthchecks. */
		static int		 socket_fd;
		static struct event	*ev;
		static uint16_t		 ping_id;
		static uint16_t		 ping_global_seq;
		uint16_t		 ping_my_seq;

		/* As ICMP socket is a raw one, we need some trick to map Echo Response to the object
		   which sent the Echo Request. So let us map the ICMP Sequence Number to the Object.
		   The object itself also knows the last Sequence Number it sent. */
		static Healthcheck_ping	**seq_map;

};

#endif

