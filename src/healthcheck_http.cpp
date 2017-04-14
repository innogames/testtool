#include <iostream>
#include <sstream>
#include <vector>
#include <fmt/format.h>
#include <yaml-cpp/yaml.h>

#include <errno.h>

#include <openssl/ssl.h>

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

	// Set defaults
	if (this->port == 0)
		this->port = 80;

	this->url = parse_string(config["url"], "/");
	this->st_http_ok_codes = parse_string(config["ok_codes"], "200");
	this->host = parse_string(config["host"], "");

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

	// Split the list by ".", copy each found code into the vector
	stringstream ss_http_ok_codes(this->st_http_ok_codes);
	string http_ok_code;
	while(getline(ss_http_ok_codes, http_ok_code, '.'))
		http_ok_codes.push_back(http_ok_code);

	// Print codes as they were parsed
	char codes_buf[1024];
	int offset = 0;
	for (unsigned int i = 0; i<http_ok_codes.size(); i++) {
		offset = snprintf(codes_buf+offset, sizeof(codes_buf)-offset, "%s,", http_ok_codes[i].c_str());
	}
	log(MSG_INFO, this, "url: " + url);

	type = "http";
	log(MSG_INFO, this, fmt::sprintf("new healthcheck, url: %s", this->url));
}

/*
 * Constructor for HTTPS healthcheck
 *
 * It only calls HTTP constructor.  Now that's what I call inheritance!
 */
Healthcheck_https::Healthcheck_https(const YAML::Node& config, class LbNode *_parent_lbnode): Healthcheck_http(config, _parent_lbnode) {
	if (this->port == 0)
		this->port = 443;
	type = "https";
	log(MSG_INFO, this, fmt::sprintf("new healthcheck, url: %s", this->url));
}

int Healthcheck_http::schedule_healthcheck(struct timespec *now) {

	// Peform general stuff for scheduled healthcheck
	if (Healthcheck::schedule_healthcheck(now) == false)
		return false;

	reply = "";

	bev = bufferevent_socket_new(eventBase, -1, 0 | BEV_OPT_CLOSE_ON_FREE);
	if (bev == NULL)
		return false;

	bufferevent_setcb(bev, &read_callback, NULL, &event_callback, this);
	bufferevent_enable(bev, EV_READ|EV_WRITE);
	evbuffer_add_printf(bufferevent_get_output(bev), "HEAD %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", this->url.c_str(), this->host.c_str());

	bufferevent_set_timeouts(bev, &this->timeout, &this->timeout);

	if (bufferevent_socket_connect(bev, addrinfo->ai_addr, addrinfo->ai_addrlen) < 0)
		return false;

	return true;
}

int Healthcheck_https::schedule_healthcheck(struct timespec *now) {
	SSL *ssl;

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
	evbuffer_add_printf(bufferevent_get_output(bev), "HEAD %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", this->url.c_str(), this->host.c_str());

	bufferevent_set_timeouts(bev, &this->timeout, &this->timeout);

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

	// Ignore READING, WRITING, CONNECTED events
	if (!(events & (BEV_EVENT_ERROR|BEV_EVENT_TIMEOUT|BEV_EVENT_EOF)))
		return;

	if (events & BEV_EVENT_TIMEOUT)
		return hc->end_check(HC_FAIL, "timeout");

	if (events & BEV_EVENT_ERROR)
		return hc->end_check(HC_FAIL, "connection error");

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

	std::stringstream message;
	message << "HTTP code " << statusline;

	unsigned int i;
	for (i = 0; i<hc->http_ok_codes.size(); i++)
		if (statusline.compare(hc->http_ok_codes[i]) == 0)
			return hc->end_check(HC_PASS, message.str());

	return hc->end_check(HC_FAIL, message.str());
}

/*
 * Override end_check() method to clean up things
 */
void Healthcheck_http::end_check(HealthcheckResult result, string message) {
	if (verbose >= 2 && result != HC_PASS && this->bev != NULL) {
		char *error = evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR());

		if (error != NULL && strlen(error) > 0)
			log(MSG_CRIT, fmt::sprintf("socket(): error: %s", strerror(errno)));
	}

	if (this->bev != NULL) {
		bufferevent_free(this->bev);
		this->bev = NULL;
	}

	Healthcheck::end_check(result, message);
}
