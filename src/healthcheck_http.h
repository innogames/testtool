//
// Testtool - HTTP Health Check
//
// Copyright (c) 2018 InnoGames GmbH
//

#ifndef _CHECK_HTTP_HPP_
#define _CHECK_HTTP_HPP_

#include <event2/http.h>
#include <event2/http_struct.h>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <sstream>
#include <vector>

#include "healthcheck.h"

#define HC_UA "testtool"

class Healthcheck_http : public Healthcheck {

  // Methods
public:
  Healthcheck_http(const nlohmann::json &config, class LbNode *_parent_lbnode,
                   string *ip_address);
  static void check_http_callback(struct evhttp_request *req, void *arg);
  int schedule_healthcheck(struct timespec *now);

protected:
  static void event_callback(struct bufferevent *bev, short events, void *arg);
  static void read_callback(struct bufferevent *bev, void *arg);
  void end_check(HealthcheckResult result, string message);
  string parse_query_template();

  // Members
protected:
  bufferevent *bev;
  string query;
  string host;
  int port;
  vector<string> ok_codes;
  struct addrinfo *addrinfo;
  string reply;
};

class Healthcheck_https : public Healthcheck_http {

  // Methods
public:
  Healthcheck_https(const nlohmann::json &config, class LbNode *_parent_lbnode,
                    string *ip_address);
  int schedule_healthcheck(struct timespec *now);
};

#endif
