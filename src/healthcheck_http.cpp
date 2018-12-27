//
// Testtool - HTTP Health Check
//
// Copyright (c) 2018 InnoGames GmbH
//

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <errno.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sstream>
#include <vector>

#include "config.h"
#include "healthcheck.h"
#include "healthcheck_http.h"
#include "lb_node.h"
#include "lb_pool.h"
#include "msg.h"

using namespace std;

extern struct event_base *eventBase;
extern SSL_CTX *sctx;
extern int verbose;

/// Constructor for HTTP healthcheck.
///
/// Parses http(s)-specific parameters.
Healthcheck_http::Healthcheck_http(const nlohmann::json &config,
                                   class LbNode *_parent_lbnode,
                                   string *ip_address)
    : Healthcheck(config, _parent_lbnode, ip_address) {
  // This is not done automatically.
  bev = NULL;

  // Set defaults
  this->type = safe_get<string>(config, "hc_type", "http");
  if (this->type == "https") {
    this->port = safe_get<int>(config, "hc_port", 443);
  } else {
    this->port = safe_get<int>(config, "hc_port", 80);
  }
  this->query = safe_get<string>(config, "hc_query", "HEAD /");
  this->host = safe_get<string>(config, "hc_host", "");

  for (const int &ok_code : config["hc_ok_codes"]) {
    // OK Codes are stored as integers in Serveradmin but HTTP
    // protocol returns strings. Convert them now and compare
    // strings later.
    this->ok_codes.push_back(fmt::sprintf("%d", ok_code));
  }
  if (this->ok_codes.size() == 0) {
    this->ok_codes.push_back("200");
  }
  // If host was not given, use IP address
  if (("" == host) && (AF_INET == address_family)) {
    host = *ip_address;
  } else if (("" == host) && (AF_INET6 == address_family)) {
    host = "[" + *ip_address + "]";
  }

  // Copy address to addrinfo struct for connect
  //
  // I am aware that converting port number to text and back to int is stupid.
  memset(&addrinfo, 0, sizeof(addrinfo));
  char port_str[256];
  memset(port_str, 0, sizeof(port_str));
  snprintf(port_str, sizeof(port_str), "%d", port);
  getaddrinfo(ip_address->c_str(), port_str, NULL, &addrinfo);

  this->log_prefix =
      fmt::sprintf("query: '%s' port: %d ok_codes: %s", this->query, this->port,
                   boost::algorithm::join(this->ok_codes, ","));
}

/// Constructor for HTTPS healthcheck
///
/// It only calls HTTP constructor.  Now that's what I call inheritance!
Healthcheck_https::Healthcheck_https(const nlohmann::json &config,
                                     class LbNode *_parent_lbnode,
                                     string *ip_address)
    : Healthcheck_http(config, _parent_lbnode, ip_address) {
  // Nothing to do. Due to constructor calling order default port
  // must be specified in parent class.
}

string Healthcheck_http::parse_query_template() {
  string new_query = string(this->query);

  boost::replace_all(new_query, "{POOL_NAME}",
                     this->parent_lbnode->parent_lbpool->name);
  if (this->address_family == AF_INET)
    boost::replace_all(new_query, "{POOL_ADDRESS}",
                       this->parent_lbnode->parent_lbpool->ipv4_address);
  else
    boost::replace_all(new_query, "{POOL_ADDRESS}",
                       this->parent_lbnode->parent_lbpool->ipv6_address);
  boost::replace_all(new_query, "{NODE_NAME}", this->parent_lbnode->name);
  boost::replace_all(new_query, "{NODE_ADDRESS}", *this->ip_address);

  vector<std::string> up_nodes_names;
  for (auto node : this->parent_lbnode->parent_lbpool->up_nodes) {
    up_nodes_names.push_back(node->name);
  }
  string joined_up_nodes_names = boost::algorithm::join(up_nodes_names, ",");
  boost::replace_all(new_query, "{ACTIVE_NODES_NAMES}", joined_up_nodes_names);

  vector<std::string> up_nodes_addresses;
  for (auto node : this->parent_lbnode->parent_lbpool->up_nodes) {
    if (!node->ipv4_address.empty())
      up_nodes_addresses.push_back(node->ipv4_address);
    if (!node->ipv6_address.empty())
      up_nodes_addresses.push_back(node->ipv6_address);
  }
  string joined_up_nodes_addresses =
      boost::algorithm::join(up_nodes_addresses, ",");
  boost::replace_all(new_query, "{ACTIVE_NODES_ADDRESSES}",
                     joined_up_nodes_addresses);

  return new_query;
}

int Healthcheck_http::schedule_healthcheck(struct timespec *now) {
  // Peform general stuff for scheduled healthcheck
  if (Healthcheck::schedule_healthcheck(now) == false)
    return false;

  reply = "";
  string new_query = this->parse_query_template();

  bev = bufferevent_socket_new(eventBase, -1, 0 | BEV_OPT_CLOSE_ON_FREE);
  if (bev == NULL)
    return false;

  bufferevent_setcb(bev, &read_callback, NULL, &event_callback, this);
  bufferevent_enable(bev, EV_READ | EV_WRITE);
  evbuffer_add_printf(bufferevent_get_output(bev),
                      "%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                      new_query.c_str(), this->host.c_str());

  bufferevent_set_timeouts(bev, &this->timeout, &this->timeout);

  if (bufferevent_socket_connect(bev, addrinfo->ai_addr, addrinfo->ai_addrlen) <
      0)
    return false;

  return true;
}

int Healthcheck_https::schedule_healthcheck(struct timespec *now) {
  SSL *ssl;

  // Peform general stuff for scheduled healthcheck
  if (Healthcheck::schedule_healthcheck(now) == false)
    return false;

  reply = "";
  string new_query = this->parse_query_template();

  ssl = SSL_new(sctx);
  bev = bufferevent_openssl_socket_new(eventBase, -1, ssl,
                                       BUFFEREVENT_SSL_CONNECTING,
                                       0 | BEV_OPT_CLOSE_ON_FREE);
  if (bev == NULL)
    return false;

  bufferevent_setcb(bev, &read_callback, NULL, &event_callback, this);
  bufferevent_enable(bev, EV_READ | EV_WRITE);
  evbuffer_add_printf(bufferevent_get_output(bev),
                      "%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                      new_query.c_str(), this->host.c_str());

  bufferevent_set_timeouts(bev, &this->timeout, &this->timeout);

  if (bufferevent_socket_connect(bev, addrinfo->ai_addr, addrinfo->ai_addrlen) <
      0)
    return false;

  return true;
}

void Healthcheck_http::read_callback(struct bufferevent *bev, void *arg) {
  struct evbuffer *input = bufferevent_get_input(bev);
  Healthcheck_http *hc = (Healthcheck_http *)arg;
  char buf[1024];

  memset(buf, 0, sizeof(buf));

  while (evbuffer_remove(input, buf, sizeof(buf) > 0)) {
    hc->reply.append(buf);
  }
}

/// /// Libecent callback for HTTP healthcheck.
///
/// It's a static method that requires the Healthcheck object to be passed to
/// it.
void Healthcheck_http::event_callback(struct bufferevent *bev, short events,
                                      void *arg) {
  (void)(bev);
  Healthcheck_http *hc = (Healthcheck_http *)arg;
  string message;

  // Ignore READING, WRITING, CONNECTED events
  if (!(events & (BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT | BEV_EVENT_EOF)))
    return;

  if (events & BEV_EVENT_TIMEOUT) {
    message = fmt::sprintf("timeout after %d.%3ds", hc->timeout.tv_sec,
                           hc->timeout.tv_usec / 1000);
    return hc->end_check(HC_FAIL, message);
  }

  if (events & BEV_EVENT_ERROR) {
    if (hc->type == "https")
      return hc->end_check(
          HC_FAIL,
          fmt::sprintf(
              "bev error: %s openssl error: %s",
              evutil_socket_error_to_string(
                  evutil_socket_geterror(bufferevent_getfd(bev))),
              ERR_reason_error_string(bufferevent_get_openssl_error(bev))));
    else
      return hc->end_check(
          HC_FAIL,
          fmt::sprintf("bev error: %s",
                       evutil_socket_error_to_string(
                           evutil_socket_geterror(bufferevent_getfd(bev)))));
  }

  // Get 1st line of reply
  stringstream replystream(hc->reply);
  string statusline;
  getline(replystream, statusline);

  // This first line goes like this:
  //
  //	HTTP/1.1 200 OK
  //
  // HTTP code is the string between 1st and 2nd " ".

  int pos;
  pos = statusline.find(" ");
  statusline = statusline.substr(pos + 1);
  pos = statusline.find(" ");
  statusline = statusline.substr(0, pos);

  message = fmt::sprintf("HTTP code %s", statusline);

  for (auto ok_code : hc->ok_codes)
    if (statusline.compare(ok_code) == 0)
      return hc->end_check(HC_PASS, message);

  return hc->end_check(HC_FAIL, message);
}

/// Overrides end_check() method to clean up things
void Healthcheck_http::end_check(HealthcheckResult result, string message) {
  if (verbose >= 2 && result != HC_PASS && this->bev != NULL) {
    char *error = evutil_socket_error_to_string(evutil_socket_geterror());

    if (error != NULL && strlen(error) > 0)
      message += fmt::sprintf(", socket() error: %s", strerror(errno));
  }

  if (this->bev != NULL) {
    bufferevent_free(this->bev);
    this->bev = NULL;
  }

  Healthcheck::end_check(result, message);
}
