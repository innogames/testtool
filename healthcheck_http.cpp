#include <iostream>
#include <sstream>
#include <vector>

#include <errno.h>

#include <openssl/ssl.h>

#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>

#include "msg.h"

#include "lb_pool.h"
#include "lb_node.h"
#include "healthcheck.h"
#include "healthcheck_http.h"

using namespace std;

extern struct event_base	*eventBase;
extern SSL_CTX			*sctx;
extern int			 verbose;


/*
   Constructor for HTTP healthcheck. Parses http(s)-specific parameters.
*/
Healthcheck_http::Healthcheck_http(istringstream &definition, class LbNode *_parent_lbnode): Healthcheck(definition, _parent_lbnode) {
	/* Initialize pointers to NULL, this is not done automatically. */
	conn = NULL;

	/* The string "parameters" was filled in by Healthcheck constructor, now turn it into a stream to read all the params. */
	istringstream ss_parameters(parameters);

	/* Read healthcheck URL. */
	std::string url;
	getline(ss_parameters, url, ':');
	this->url = new char[url.length()+1];
	strcpy(this->url, url.c_str());

	/* Read the list of OK HTTP codes. */
	std::string st_http_ok_codes; 
	getline(ss_parameters, st_http_ok_codes, ':');

	/* Split the list by ".", convert each element to int and copy it into a vector. */
	stringstream ss_http_ok_codes(st_http_ok_codes);
	string http_ok_code;
	while(getline(ss_http_ok_codes, http_ok_code, '.'))
		http_ok_codes.push_back(atoi(http_ok_code.c_str()));

	char codes_buf[1024];
	int offset = 0;
	for (unsigned int i = 0; i<http_ok_codes.size(); i++) {
		offset = snprintf(codes_buf+offset, sizeof(codes_buf)-offset, "%d,", http_ok_codes[i]);
	}
	show_message(MSG_TYPE_DEBUG, "      type: http(s), url: %s, ok_codes: %s", url.c_str(), codes_buf);

	type = "http";
}


/*
   Constructor for HTTPS healthcheck. It only calls HTTP constructor. Now that's what I call inheritance!
*/
Healthcheck_https::Healthcheck_https(istringstream &definition, class LbNode *_parent_lbnode): Healthcheck_http(definition, _parent_lbnode) {
	type = "https";
}


void Healthcheck_http::cleanup_connection() {
	/* Only the connection has to be explicitly freed.
	 * Buffers will be freed by evhttp_connection_free. */
	if (conn)
		evhttp_connection_free(conn);
	conn = NULL;
}


/*
   The callback is called by libevent, it's a static method that requires the Healthcheck object to be passed to it.
*/
void Healthcheck_http::callback(struct evhttp_request *req, void *arg) {

	Healthcheck_http * healthcheck = (Healthcheck_http *)arg;

	/* Socket error would be the last encountered error in this connection and is not zeroed after success, so we can not relay on this function. */
	if ( req == NULL || req->response_code == 0) { /* Libevent encountered some problem. */

		healthcheck->last_state = STATE_DOWN;

		if (verbose>1 || healthcheck->hard_state != STATE_DOWN) {

			/* There is a function to translate error number to a meaningful information, but the meaning is usually quite confusing. */

			if (EVUTIL_SOCKET_ERROR() == 36 || EVUTIL_SOCKET_ERROR() == 9) {
				/* Connect timeout or connecting interrupted by libevent timeout. */
				show_message(MSG_TYPE_HC_FAIL, "%s %s:%d - Healthcheck_%s: timeout after %d,%ds",
						healthcheck->parent_lbnode->parent_lbpool->name.c_str(), healthcheck->parent_lbnode->address.c_str(), healthcheck->port, healthcheck->type.c_str(), healthcheck->timeout.tv_sec, (healthcheck->timeout.tv_nsec/10000000));
			} else if (EVUTIL_SOCKET_ERROR() == 32) {
				/* Connection refused on a ssl check. */
				show_message(MSG_TYPE_HC_FAIL, "%s %s:%d - Healthcheck_%s: ssl connection refused",
						healthcheck->parent_lbnode->parent_lbpool->name.c_str(), healthcheck->parent_lbnode->address.c_str(), healthcheck->port, healthcheck->type.c_str());
			} else if (EVUTIL_SOCKET_ERROR() == 54) {
				/* Connection refused. */
				show_message(MSG_TYPE_HC_FAIL, "%s %s:%d - Healthcheck_%s: connection refused",
						healthcheck->parent_lbnode->parent_lbpool->name.c_str(), healthcheck->parent_lbnode->address.c_str(), healthcheck->port, healthcheck->type.c_str());
			} else if (EVUTIL_SOCKET_ERROR() == 64) {
				/* Host down immediately reported by system. */
				show_message(MSG_TYPE_HC_FAIL, "%s %s:%d - Healthcheck_%s: host down",
						healthcheck->parent_lbnode->parent_lbpool->name.c_str(), healthcheck->parent_lbnode->address.c_str(), healthcheck->port, healthcheck->type.c_str());
			} else {
				show_message(MSG_TYPE_HC_FAIL, "%s %s:%d - Healthcheck_%s: other error (socket error: %d)",
						healthcheck->parent_lbnode->parent_lbpool->name.c_str(), healthcheck->parent_lbnode->address.c_str(), healthcheck->port, healthcheck->type.c_str(), EVUTIL_SOCKET_ERROR());

			}

		}
	} else { /* The http request finished properly. */
		healthcheck->http_result = req->response_code;

		unsigned int i;
		/*Check if resulting http code is a good one. */
		for (i = 0; i<healthcheck->http_ok_codes.size(); i++) {

			if (req->response_code == healthcheck->http_ok_codes[i]) {

				if (verbose>1 || healthcheck->last_state == STATE_DOWN)
					show_message(MSG_TYPE_HC_PASS, "%s %s:%d - Healthcheck_%s: good HTTP code: %ld",
						healthcheck->parent_lbnode->parent_lbpool->name.c_str(), healthcheck->parent_lbnode->address.c_str(), healthcheck->port, healthcheck->type.c_str(), healthcheck->http_result);

				healthcheck->last_state = STATE_UP; /* Service is UP */
				break;
			}
		}

		/* Have all http codes been checked? */
		if (i == healthcheck->http_ok_codes.size()) {
			healthcheck->last_state = STATE_DOWN;
			if (verbose>1 || healthcheck->hard_state != STATE_DOWN) {
				show_message(MSG_TYPE_HC_FAIL, "%s %s:%d - Healthcheck_%s: bad HTTP code: %ld",
						healthcheck->parent_lbnode->parent_lbpool->name.c_str(), healthcheck->parent_lbnode->address.c_str(), healthcheck->port, healthcheck->type.c_str(), healthcheck->http_result);
			}
		}
	}
	
	healthcheck->cleanup_connection();
	healthcheck->handle_result();
}


int Healthcheck_http::schedule_healthcheck(struct timespec *now) {
	struct bufferevent	*bev;
	struct evhttp_request	*req;

	/* Peform general stuff for scheduled healthcheck. */
	if (Healthcheck::schedule_healthcheck(now) == false)
		return false;

	bev = bufferevent_socket_new(eventBase, -1, 0 | BEV_OPT_DEFER_CALLBACKS);

	conn = evhttp_connection_base_bufferevent_new(eventBase, NULL, bev, parent_lbnode->address.c_str(), port);

	struct timeval timeout_tv;
	timeout_tv.tv_sec  = timeout.tv_sec;
	timeout_tv.tv_usec = timeout.tv_nsec / 1000;
	evhttp_connection_set_timeout_tv(conn, &timeout_tv); /* We use timestruct everywhere but libevent wants timeval. */

	req = evhttp_request_new(&callback, (void *)this);
	evhttp_add_header(req->output_headers, "Host", parent_lbnode->address.c_str());
	evhttp_add_header(req->output_headers, "Connection", "close");

	evhttp_make_request(conn, req, EVHTTP_REQ_HEAD, url);

	return true;
}


int Healthcheck_https::schedule_healthcheck(struct timespec *now) {
	struct bufferevent	*bev;
	struct evhttp_request	*req;
	SSL			*ssl;

	/* Peform general stuff for scheduled healthcheck. */
	if (Healthcheck::schedule_healthcheck(now) == false)
		return false;

	/* Always create new ssl, the old one is freed somewhere in evhttp_connection_free, called in connection finish handler */
	ssl = SSL_new (sctx);
	bev = bufferevent_openssl_socket_new( eventBase, -1, ssl, BUFFEREVENT_SSL_CONNECTING, 0 | BEV_OPT_DEFER_CALLBACKS);

	conn = evhttp_connection_base_bufferevent_new(eventBase, NULL, bev, parent_lbnode->address.c_str(), port);

	struct timeval timeout_tv;
	timeout_tv.tv_sec  = timeout.tv_sec;
	timeout_tv.tv_usec = timeout.tv_nsec / 1000;
	evhttp_connection_set_timeout_tv(conn, &timeout_tv); /* We use timestruct everywhere but libevent wants timeval. */

	req = evhttp_request_new(&callback, (void *)this);
	evhttp_add_header(req->output_headers, "Host", parent_lbnode->address.c_str());
	evhttp_add_header(req->output_headers, "Connection", "close");

	evhttp_make_request(conn, req, EVHTTP_REQ_HEAD, url);

	return true;
}

