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
		int schedule_healthcheck();

	protected:
		static void callback(struct evhttp_request *req, void *arg);
		void cleanup_connection();


	/* Members */
	protected:
		struct evhttp_connection	*conn;
		struct evhttp_request		*req;
		struct bufferevent		*bev;
		long				 http_result;
		char				*url;
		vector<int>			 http_ok_codes;

};


class Healthcheck_https: public Healthcheck_http {

	/* Methods */
	public:
		Healthcheck_https(istringstream &definition, class LbNode *_parent_lbnode);
		int schedule_healthcheck();


	/* Members */
	private:
		SSL	*ssl; /* Required only for https */


};


#endif

