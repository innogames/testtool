#ifndef _CHECK_HTTP_HPP_
#define _CHECK_HTTP_HPP_

#include <vector>

#include <event2/http.h>
#include <event2/http_struct.h>
#include <openssl/ssl.h>

#include "healthcheck.h"

class Healthcheck_http: public Healthcheck {
	public:
		Healthcheck_http(string &definition, class Service &service);
		static void check_http_callback(struct evhttp_request *req, void *arg);
		int schedule_healthcheck();

	protected:
		struct evhttp_connection	*conn;
		struct evhttp_request		*req;
		struct bufferevent		*bev;
		long				 http_result;
		char				*url;
		vector<int>			 http_ok_codes;
		static void callback(struct evhttp_request *req, void *arg);
		void cleanup_connection();
};


class Healthcheck_https: public Healthcheck_http {
	public:
		Healthcheck_https(string &definition, class Service &service);
		int schedule_healthcheck();

	private:
		SSL				*ssl; /* Required only for https */

};


#endif

