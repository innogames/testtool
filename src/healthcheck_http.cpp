#include <iostream>
#include <sstream>
#include <vector>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <yaml-cpp/yaml.h>
#include <boost/algorithm/string/join.hpp>

#include <errno.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/buffer.h>

#include "config.h"
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
 * Constructor for HTTP healthcheck
 *
 * Parses http(s)-specific parameters.
 */
Healthcheck_http::Healthcheck_http(const YAML::Node& config, class LbNode *_parent_lbnode): Healthcheck(config, _parent_lbnode) {
	// This is not done automatically.
	bev = NULL;
	ssl_session = NULL;
	ssl = NULL;

	// Set defaults
	this->type = parse_string(config["hc_type"], "http");
	if (this->type == "https") {
		this->port = parse_int(config["hc_port"], 443);
	} else {
		this->port = parse_int(config["hc_port"], 80);
	}
	this->query = parse_string(config["hc_query"], "HEAD /");
	this->host = parse_string(config["hc_host"], "");

	for (YAML::Node ok_code_node: config["hc_ok_codes"]) {
		/*
		 * OK Codes are stored as integers in Serveradmin but HTTP
		 * protocol returns strings. Convert them now and compare
		 * strings later.
		 */
		int ok_code = parse_int(ok_code_node, 200);
		this->ok_codes.push_back(fmt::sprintf("%d", ok_code));
	}
	if (this->ok_codes.size() == 0) {
		this->ok_codes.push_back("200");
	}
	// If host was not given, use IP address
	if (host == "")
		host = parent_lbnode->address.c_str();

	/*
	 * Copy address to addrinfo struct for connect
	 *
	 * I am aware that converting port number to text and back to int is stupid.
	 */
	memset(&addrinfo, 0, sizeof(addrinfo));
	char port_str[256];
	memset(port_str, 0, sizeof(port_str));
	snprintf(port_str, sizeof(port_str), "%d", port);
	getaddrinfo(parent_lbnode->address.c_str(), port_str, NULL, &addrinfo);

	this->log_prefix = fmt::sprintf(
		"query: '%s' port: %d ok_codes: %s",
		this->query,
		this->port,
		boost::algorithm::join(this->ok_codes, ",")
	);
}

/*
 * Constructor for HTTPS healthcheck
 *
 * It only calls HTTP constructor.  Now that's what I call inheritance!
 */
Healthcheck_https::Healthcheck_https(const YAML::Node& config, class LbNode *_parent_lbnode): Healthcheck_http(config, _parent_lbnode) {
	/*
	 * Nothing to do. Due to constructor calling order default port
	 * must be specified in parent class.
	 */
}

int Healthcheck_http::schedule_healthcheck(struct timespec *now) {

	// Peform general stuff for scheduled healthcheck
	if (Healthcheck::schedule_healthcheck(now) == false)
		return false;

	reply = "";

	bev = bufferevent_socket_new(eventBase, -1, 0);
	if (bev == NULL)
		return false;

	bufferevent_setcb(bev, &read_callback, NULL, &event_callback, this);
	bufferevent_enable(bev, EV_READ|EV_WRITE);
	evbuffer_add_printf(bufferevent_get_output(bev), "%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", this->query.c_str(), this->host.c_str());

	bufferevent_set_timeouts(bev, &this->timeout, &this->timeout);

	if (bufferevent_socket_connect(bev, addrinfo->ai_addr, addrinfo->ai_addrlen) < 0)
		return false;

	return true;
}

int Healthcheck_https::schedule_healthcheck(struct timespec *now) {
	// Peform general stuff for scheduled healthcheck
	if (Healthcheck::schedule_healthcheck(now) == false)
		return false;

	reply = "";

	ssl = SSL_new(sctx);

	bev = bufferevent_openssl_socket_new(eventBase, -1, ssl, BUFFEREVENT_SSL_CONNECTING, 0 | BEV_OPT_CLOSE_ON_FREE);
	if (bev == NULL)
		return false;

	bufferevent_setcb(bev, &read_callback, NULL, &event_callback, this);
	bufferevent_enable(bev, EV_READ|EV_WRITE);
	evbuffer_add_printf(bufferevent_get_output(bev), "%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", this->query.c_str(), this->host.c_str());

	bufferevent_set_timeouts(bev, &this->timeout, &this->timeout);

	if (this->ssl_session) {
		SSL_set_session(this->ssl, this->ssl_session);
	}

	if (bufferevent_socket_connect(bev, addrinfo->ai_addr, addrinfo->ai_addrlen) < 0)
		return false;

	return true;
}

void Healthcheck_http::read_callback(struct bufferevent *bev, void * arg) {
	struct evbuffer *input = bufferevent_get_input(bev);
	Healthcheck_http *hc = (Healthcheck_http *)arg;
	char buf[1024];

	memset(buf, 0, sizeof(buf));

	while (evbuffer_remove(input, buf, sizeof(buf) > 0)) {
		hc->reply.append(buf);
	}
}

/*
 * The callback is called by libevent
 *
 * It's a static method that requires the Healthcheck object to be passed to it.
*/
void Healthcheck_http::event_callback(struct bufferevent *bev, short events, void *arg) {
	(void)(bev);
	Healthcheck_http *hc = (Healthcheck_http *)arg;
	string message;

	// CONNECTED event is when connection is ready for us.
	// In case of HTTPS checks it means that TLS was negotiated.
	if (events & BEV_EVENT_CONNECTED && hc->ssl != NULL) {

		if (hc->ssl_session != NULL)
			SSL_SESSION_free(hc->ssl_session);

		hc->ssl_session = SSL_get1_session(hc->ssl);
	}

	// Ignore READING, WRITING, CONNECTED events.
	// Pass through for connection errors, timeouts and connection closed.
	if (!(events & (BEV_EVENT_ERROR|BEV_EVENT_TIMEOUT|BEV_EVENT_EOF)))
		return;

	if (events & BEV_EVENT_TIMEOUT) {
		message = fmt::sprintf(
			"timeout after %d.%3ds",
			hc->timeout.tv_sec,
			hc->timeout.tv_usec / 1000
		);
		return hc->end_check(HC_FAIL, message);
	}

	if (events & BEV_EVENT_ERROR) {
		if (hc->type == "https")
			return hc->end_check(
				HC_FAIL,
				fmt::sprintf("bev error: %s openssl error: %s",
					evutil_socket_error_to_string(
						evutil_socket_geterror(bufferevent_getfd(bev))
					),
					ERR_reason_error_string(
						bufferevent_get_openssl_error(bev)
					)
				)
			);
		else
			return hc->end_check(
				HC_FAIL,
				fmt::sprintf("bev error: %s",
					evutil_socket_error_to_string(
						evutil_socket_geterror(bufferevent_getfd(bev))
					)
				)
			);
	}

	// Get 1st line of reply
	stringstream replystream(hc->reply);
	string statusline;
	getline(replystream, statusline);

	/*
	 * This first line goes like this:
	 *
	 *	HTTP/1.1 200 OK
	 *
	 * HTTP code is the string between 1st and 2nd " ".
	 */
	int pos;
	pos = statusline.find(" ");
	statusline = statusline.substr(pos + 1);
	pos = statusline.find(" ");
	statusline = statusline.substr(0, pos);

	message = fmt::sprintf("HTTP code %s", statusline);

	for (auto ok_code: hc->ok_codes)
		if (statusline.compare(ok_code) == 0)
			return hc->end_check(HC_PASS, message);

	if (hc->ssl != NULL) {
		SSL *ctx = bufferevent_openssl_get_ssl(bev);
		SSL_set_shutdown(ctx, SSL_RECEIVED_SHUTDOWN);
		SSL_shutdown(ctx);
	}

	return hc->end_check(HC_FAIL, message);
}

/*
 * Override end_check() method to clean up things
 */
void Healthcheck_http::end_check(HealthcheckResult result, string message) {
	if (verbose >= 2 && result != HC_PASS && this->bev != NULL) {
		char *error = evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR());

		if (error != NULL && strlen(error) > 0)
			message += fmt::sprintf(", socket() error: %s", strerror(errno));
	}

	if (this->bev != NULL) {
		bufferevent_free(this->bev);
		this->bev = NULL;
	}

	Healthcheck::end_check(result, message);
}
