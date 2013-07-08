#include <iostream>
#include <sstream>
#include <vector>

#include <errno.h>

#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <openssl/ssl.h>

#include "healthcheck.h"
#include "healthcheck_http.h"
#include "msg.h"

using namespace std;

extern struct event_base	*eventBase;
extern SSL_CTX			*sctx;
extern int			 verbose;

/*
   Constructor for HTTP healthcheck. Parses http(s)-specific parameters.
*/
Healthcheck_http::Healthcheck_http(string &definition, class Service &service): Healthcheck(definition, service) {

	std::stringstream s_parameters(parameters);

	/* Read test URL. */
	std::string url;
	getline(s_parameters, url, ':');
	this->url = new char[url.length()+1];
	strcpy(this->url, url.c_str());

	/* Read the list of OK HTTP codes. */
	std::string st_http_ok_codes; 
	getline(s_parameters, st_http_ok_codes, ':');

	/* Split the list by ".", convert each element to int and copy it into a vector. */
	stringstream ss_http_ok_codes(st_http_ok_codes);
	string http_ok_code;
	while(getline(ss_http_ok_codes, http_ok_code, '.'))
		http_ok_codes.push_back(atoi(http_ok_code.c_str()));

	if (verbose>0) {
		cout << "http_ok_codes:";
		for (unsigned int i = 0; i<http_ok_codes.size(); i++)
			cout << (int)http_ok_codes[i] << ",";
		cout << endl;
	}
}

/*
   Constructor for HTTPS healthcheck. It only calls HTTP constructor. Now that's what I call inheritance!
*/
Healthcheck_https::Healthcheck_https(string &definition, class Service &service): Healthcheck_http(definition, service) {

}


void Healthcheck_http::cleanup_connection() {
	/* Only the connection has to be freed. */
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
				showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - healthcheck_%s "CL_RED"timeout after %d,%ds"CL_RESET"\n",
						healthcheck->parent->name.c_str(), healthcheck->address.c_str(), healthcheck->port, healthcheck->type.c_str(), healthcheck->timeout.tv_sec, (healthcheck->timeout.tv_usec/10000));
			} else if (EVUTIL_SOCKET_ERROR() == 32) {
				/* Connection refused on a ssl check. */
				showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - healthcheck_%s "CL_RED"ssl connection refused"CL_RESET"\n",
						healthcheck->parent->name.c_str(), healthcheck->address.c_str(), healthcheck->port, healthcheck->type.c_str());
			} else if (EVUTIL_SOCKET_ERROR() == 54) {
				/* Connection refused. */
				showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - healthcheck_%s "CL_RED"connection refused"CL_RESET"\n",
						healthcheck->parent->name.c_str(), healthcheck->address.c_str(), healthcheck->port, healthcheck->type.c_str());
			} else if (EVUTIL_SOCKET_ERROR() == 64) {
				/* Host down immediately reported by system. */
				showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - healthcheck_%s "CL_RED"host down"CL_RESET"\n",
						healthcheck->parent->name.c_str(), healthcheck->address.c_str(), healthcheck->port, healthcheck->type.c_str());
			} else {
				showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - healthcheck_%s "CL_RED"other error (socket error: %d)"CL_RESET"\n",
						healthcheck->parent->name.c_str(), healthcheck->address.c_str(), healthcheck->port, healthcheck->type.c_str(), EVUTIL_SOCKET_ERROR());
			}

		}

	} else if (req->response_code == 0) { /* Connection not established or other http error. */

		healthcheck->last_state = STATE_DOWN;

		if (verbose>1 || healthcheck->hard_state != STATE_DOWN) {
			showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - healthcheck_%s "CL_RED"http protocol error"CL_RESET"\n",
					healthcheck->parent->name.c_str(), healthcheck->address.c_str(), healthcheck->port, healthcheck->type.c_str());
		}

	} else { /* The http request finished properly. */
		healthcheck->http_result = req->response_code;

		unsigned int i;
		/*Check if resulting http code is a good one. */
		for (i = 0; i<healthcheck->http_ok_codes.size(); i++) {

			if (req->response_code == healthcheck->http_ok_codes[i]) {

				if (verbose>1 || healthcheck->last_state == STATE_DOWN)
					showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - healthcheck_%s "CL_GREEN"good HTTP code: %ld"CL_RESET"\n",
						healthcheck->parent->name.c_str(), healthcheck->address.c_str(), healthcheck->port, healthcheck->type.c_str(), healthcheck->http_result);

				healthcheck->last_state = STATE_UP; /* Service is UP */
				break;
			}
		}

		/* Have all http codes been checked? */
		if (i == healthcheck->http_ok_codes.size()) {
			healthcheck->last_state = STATE_DOWN;
			if (verbose>1 || healthcheck->hard_state != STATE_DOWN) {
				showStatus(CL_WHITE"%s"CL_RESET" - "CL_CYAN"%s:%d"CL_RESET" - healthcheck_%s "CL_RED"bad HTTP code: %ld"CL_RESET"\n",
						healthcheck->parent->name.c_str(), healthcheck->address.c_str(), healthcheck->port, healthcheck->type.c_str(), healthcheck->http_result);
			}
		}
	}
	
	healthcheck->cleanup_connection();
	healthcheck->handle_result();
}


int Healthcheck_http::schedule_healthcheck() {
	/* Peform general stuff for scheduled healthcheck. */
	if (Healthcheck::schedule_healthcheck() == false)
		return false;

	bev = bufferevent_socket_new ( eventBase, -1, 0 | BEV_OPT_CLOSE_ON_FREE);

	conn = evhttp_connection_base_bufferevent_new(eventBase, NULL, bev, address.c_str(), port);

	evhttp_connection_set_timeout_tv(conn, &timeout);

	req = evhttp_request_new(&callback, (void *)this);
	evhttp_add_header(req->output_headers, "Host", address.c_str());
	evhttp_add_header(req->output_headers, "Connection", "close");

	evhttp_make_request(conn, req, EVHTTP_REQ_HEAD, url);

	return true;
}


int Healthcheck_https::schedule_healthcheck() {
	/* Peform general stuff for scheduled healthcheck. */
	if (Healthcheck::schedule_healthcheck() == false)
		return false;

	/* Always create new ssl, the old one is freed somewhere in evhttp_connection_free, called in connection finish handler */                                                                         
	ssl = SSL_new (sctx);
	bev = bufferevent_openssl_socket_new ( eventBase, -1, ssl, BUFFEREVENT_SSL_CONNECTING, 0 | BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS );

	conn = evhttp_connection_base_bufferevent_new(eventBase, NULL, bev, address.c_str(), port);

	evhttp_connection_set_timeout_tv(conn, &timeout);

	req = evhttp_request_new(&callback, (void *)this);
	evhttp_add_header(req->output_headers, "Host", address.c_str());
	evhttp_add_header(req->output_headers, "Connection", "close");

	evhttp_make_request(conn, req, EVHTTP_REQ_HEAD, url);

	return true;
}

