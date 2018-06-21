/*
 * Testtool - Health check - TCP
 *
 * Copyright (c) 2018 InnoGames GmbH
 */

#ifndef _CHECK_TCP_HPP_
#define _CHECK_TCP_HPP_

#include <vector>
#include <sstream>

#include <unistd.h>

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/util.h>

#include "healthcheck.h"


class Healthcheck_tcp: public Healthcheck {

	/* Methods */
	public:
		Healthcheck_tcp(const YAML::Node& config, class LbNode *_parent_lbnode);
		static void check_tcp_callback(struct evtcp_request *req, void *arg);
		int schedule_healthcheck(struct timespec *now);

	protected:
		static void callback(evutil_socket_t fd, short what , void *arg);

	private:
		int		 socket_fd;
		struct event	*ev;
		int	 	 port;
};


#endif
