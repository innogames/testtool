#include <iostream>
#include <sstream>
#include <vector>

#include <errno.h>

#include <openssl/ssl.h>

#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/buffer.h>

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
	bev = NULL;

	/* The string "parameters" was filled in by Healthcheck constructor, now turn it into a stream to read all the params. */
	istringstream ss_parameters(parameters);

	/* Read healthcheck URL. */
	std::string url;
	getline(ss_parameters, url, ':');
	this->url = new char[url.length()+1];
	strcpy(this->url, url.c_str());

	/* Copy address to addrinfo struct for connect. I am aware that converting port number to text and back to int is stupid */
	memset(&addrinfo, 0, sizeof(addrinfo));
	char port_str[256];
	memset(port_str, 0, sizeof(port_str));
	snprintf(port_str, sizeof(port_str), "%d", port);
	getaddrinfo(parent_lbnode->address.c_str(), port_str, NULL, &addrinfo);
	
	/* Read the list of OK HTTP codes. */
	std::string st_http_ok_codes; 
	getline(ss_parameters, st_http_ok_codes, ':');

	/* Split the list by ".", copy each found code into the vector. */
	stringstream ss_http_ok_codes(st_http_ok_codes);
	string http_ok_code;
	while(getline(ss_http_ok_codes, http_ok_code, '.'))
		http_ok_codes.push_back(http_ok_code);

	/* Print codes as they were parsed. */
	char codes_buf[1024];
	int offset = 0;
	for (unsigned int i = 0; i<http_ok_codes.size(); i++) {
		offset = snprintf(codes_buf+offset, sizeof(codes_buf)-offset, "%s,", http_ok_codes[i].c_str());
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
	if (bev)
		bufferevent_free(bev);
	bev = NULL;
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
   The callback is called by libevent, it's a static method that requires the Healthcheck object to be passed to it.
*/
void Healthcheck_http::event_callback(struct bufferevent *bev, short events, void *arg) {
	(void)(bev);
	Healthcheck_http *hc = (Healthcheck_http *)arg;

	/* Ignore READING, WRITING, CONNECTED events. */
	if (events & (BEV_EVENT_ERROR|BEV_EVENT_TIMEOUT|BEV_EVENT_EOF)) {

		if (events & BEV_EVENT_TIMEOUT) {
			hc->last_state = STATE_DOWN;
			if (verbose>1 || hc->hard_state != STATE_DOWN)
				show_message(MSG_TYPE_HC_FAIL, "%s %s:%d - Healthcheck_%s: timeout after %d,%ds; message: %s",
				    hc->parent_lbnode->parent_lbpool->name.c_str(),
				    hc->parent_lbnode->address.c_str(),
				    hc->port,
				    hc->type.c_str(),
				    hc->timeout.tv_sec,
				    (hc->timeout.tv_nsec/10000000),
				    evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));

		}
		else if (events & BEV_EVENT_ERROR) {
			hc->last_state = STATE_DOWN;
			if (verbose>1 || hc->hard_state != STATE_DOWN)
				show_message(MSG_TYPE_HC_FAIL, "%s %s:%d - Healthcheck_%s: error connecting; message: %s",
				    hc->parent_lbnode->parent_lbpool->name.c_str(),
				    hc->parent_lbnode->address.c_str(),
				    hc->port,
				    hc->type.c_str(),
				    evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
		}
		else if (events & BEV_EVENT_EOF) {
			/* Get 1st line of reply. */
			stringstream replystream(hc->reply);
			string statusline;
			getline(replystream, statusline);

			/* A line goes like this:
			 * HTTP/1.1 200 OK
			 * Http code is the string between 1st and 2nd ' '.
			 * It would be awesome to use regexp but then
			 * we need boost or C++11 which is not in FreeBSD 9 (but is in 10).
			 */ 
			int pos;
			pos = statusline.find(" ");
			statusline = statusline.substr(pos+1);
			pos = statusline.find(" ");
			statusline = statusline.substr(0, pos);

			unsigned int i;

			for (i = 0; i<hc->http_ok_codes.size(); i++) {
				if (statusline.compare(hc->http_ok_codes[i]) == 0) {
					if (verbose>1 || hc->last_state == STATE_DOWN)
						show_message(MSG_TYPE_HC_PASS, "%s %s:%d - Healthcheck_%s: good HTTP code: %s",
						    hc->parent_lbnode->parent_lbpool->name.c_str(),
						    hc->parent_lbnode->address.c_str(),
						    hc->port,
						    hc->type.c_str(),
						    statusline.c_str());
	
					hc->last_state = STATE_UP;
					break;
				}
			}
		
			if (i == hc->http_ok_codes.size()) {
				hc->last_state = STATE_DOWN;
				if (verbose>1 || hc->hard_state != STATE_DOWN) {
					show_message(MSG_TYPE_HC_FAIL, "%s %s:%d - Healthcheck_%s: bad HTTP code: %s",
					    hc->parent_lbnode->parent_lbpool->name.c_str(),
					    hc->parent_lbnode->address.c_str(),
					    hc->port,
					    hc->type.c_str(),
					    statusline.c_str());
				}
			}
		}

		hc->cleanup_connection();
		hc->handle_result();
	}
}


int Healthcheck_http::schedule_healthcheck(struct timespec *now) {
	/* Peform general stuff for scheduled healthcheck. */
	if (Healthcheck::schedule_healthcheck(now) == false)
		return false;

	reply = "";

	bev = bufferevent_socket_new(eventBase, -1, 0 | BEV_OPT_CLOSE_ON_FREE);
	if (bev == NULL)
		return false;

	bufferevent_setcb(bev, &read_callback, NULL, &event_callback, this);
	bufferevent_enable(bev, EV_READ|EV_WRITE);
	evbuffer_add_printf(bufferevent_get_output(bev), "HEAD %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", url, parent_lbnode->address.c_str());

	struct timeval timeout_tv;
	timeout_tv.tv_sec  = timeout.tv_sec;
	timeout_tv.tv_usec = timeout.tv_nsec / 1000;
	bufferevent_set_timeouts(bev, &timeout_tv, &timeout_tv);

	if (bufferevent_socket_connect(bev, addrinfo->ai_addr, sizeof(struct sockaddr)) < 0)
		return false;

	return true;
}


int Healthcheck_https::schedule_healthcheck(struct timespec *now) {
	SSL *ssl;

	/* Peform general stuff for scheduled healthcheck. */
	if (Healthcheck::schedule_healthcheck(now) == false)
		return false;

	reply = "";

	ssl = SSL_new(sctx);
	bev = bufferevent_openssl_socket_new(eventBase, -1, ssl, BUFFEREVENT_SSL_CONNECTING, 0 | BEV_OPT_CLOSE_ON_FREE);
	if (bev == NULL)
		return false;

	bufferevent_setcb(bev, &read_callback, NULL, &event_callback, this);
	bufferevent_enable(bev, EV_READ|EV_WRITE);
	evbuffer_add_printf(bufferevent_get_output(bev), "HEAD %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", url, parent_lbnode->address.c_str());

	struct timeval timeout_tv;
	timeout_tv.tv_sec  = timeout.tv_sec;
	timeout_tv.tv_usec = timeout.tv_nsec / 1000;
	bufferevent_set_timeouts(bev, &timeout_tv, &timeout_tv);

	if (bufferevent_socket_connect(bev, addrinfo->ai_addr, sizeof(struct sockaddr)) < 0)
		return false;

	return true;
}

