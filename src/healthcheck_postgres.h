//
// Testtool - Postgres Health Check
//
// Copyright (c) 2018 InnoGames GmbH
//

#ifndef _CHECK_POSTGRES_HPP_
#define _CHECK_POSTGRES_HPP_

// Uncomment to disable assert()
//#define NDEBUG

#include <event2/event_struct.h>
#include <openssl/ssl.h>
#include <sstream>
#include <vector>
#include <yaml-cpp/yaml.h>

#if defined(__FreeBSD__) || defined(__APPLE__)
#include <libpq-fe.h>
#else
#include <postgresql/libpq-fe.h>
#endif

#include "healthcheck.h"

class Healthcheck_postgres : public Healthcheck {

  // Methods
public:
  Healthcheck_postgres(const YAML::Node &config, class LbNode *_parent_lbnode,
                       string *ip_address);
  int schedule_healthcheck(struct timespec *now);

protected:
  void start_conn();
  void poll_conn();
  void send_query();
  void flush_query();
  void handle_query();
  void end_check(HealthcheckResult result, string message);
  void register_io_event(short flag, void (Healthcheck_postgres::*method)());
  void register_timeout_event();
  static void handle_io_event(int fd, short flag, void *arg);
  static void handle_timeout_event(int fd, short flag, void *arg);

  // Members
protected:
  string host;
  int port;
  string dbname;
  string user;
  string query;
  PGconn *conn;
  PostgresPollingStatusType *poll_status;
  PGresult *result = NULL;
  struct event *io_event;
  struct event *timeout_event;
  short event_flag;
  int event_counter;
  void (Healthcheck_postgres::*callback_method)();
};

#endif
