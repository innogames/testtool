#ifndef _CHECK_HTTP_HPP_
#define _CHECK_HTTP_HPP_

#include <vector>
#include <sstream>

#include <event2/http.h>
#include <event2/http_struct.h>

#include <openssl/ssl.h>

#include "healthcheck.h"


class Healthcheck_http: public Healthcheck {

	/* Methods */
	public:
		Healthcheck_http(istringstream &definition, class LbNode *_parent_lbnode);
		static void check_http_callback(struct evhttp_request *req, void *arg);
		int schedule_healthcheck(struct timespec *now);

	protected:
		static void event_callback(struct bufferevent *bev, short events, void *arg);
		static void read_callback(struct bufferevent *bev, void *arg);
		void cleanup_connection();

	private:
		void confline_callback(string &var, istringstream &val);

	/* Members */
	protected:
		bufferevent			*bev;
		string				 url;
		string				 host;
		string				 st_http_ok_codes;
		vector<string>			 http_ok_codes;
		struct addrinfo			*addrinfo;
		string				 reply;

};


class Healthcheck_https: public Healthcheck_http {

	/* Methods */
	public:
		Healthcheck_https(istringstream &definition, class LbNode *_parent_lbnode);
		int schedule_healthcheck(struct timespec *now );
};


#endif

