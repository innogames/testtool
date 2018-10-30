/*
 * Testtool - HTTP Health Check
 *
 * Copyright (c) 2018 InnoGames GmbH
 */

#ifndef _CHECK_HTTP_HPP_
#define _CHECK_HTTP_HPP_

#include <vector>
#include <sstream>
#include <yaml-cpp/yaml.h>

#include <event2/http.h>
#include <event2/http_struct.h>

#include <openssl/ssl.h>

#include "healthcheck.h"


class Healthcheck_http: public Healthcheck {

	/* Methods */
	public:
		Healthcheck_http(const YAML::Node& config, class LbNode *_parent_lbnode);
		static void check_http_callback(struct evhttp_request *req, void *arg);
		int schedule_healthcheck(struct timespec *now);

	protected:
		static void event_callback(struct bufferevent *bev, short events, void *arg);
		static void read_callback(struct bufferevent *bev, void *arg);
		void end_check(HealthcheckResult result, string message);
		string parse_query_template();


	/* Members */
	protected:
		bufferevent			*bev;
		string				 query;
		string				 host;
		int			 	 port;
		vector<string>			 ok_codes;
		struct addrinfo			*addrinfo;
		string				 reply;

};


class Healthcheck_https: public Healthcheck_http {

	/* Methods */
	public:
		Healthcheck_https(const YAML::Node& config, class LbNode *_parent_lbnode);
		int schedule_healthcheck(struct timespec *now );
};

#endif
