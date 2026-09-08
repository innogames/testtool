//
// Testtool - MySQL Health Check
//
// Copyright (c) 2026 InnoGames GmbH
//

#ifndef _CHECK_MYSQL_HPP_
#define _CHECK_MYSQL_HPP_

#include <event2/event_struct.h>
#include <mysql.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

#include "healthcheck.h"

class Healthcheck_mysql : public Healthcheck {

  // Methods
public:
  Healthcheck_mysql(const nlohmann::json &config, class LbNode *_parent_lbnode,
                    string *ip_address);
  int schedule_healthcheck(struct timespec *now);

protected:
  void start_conn();
  void poll_conn();
  void finish_conn();
  void send_query();
  void poll_query();
  void finish_query();
  void store_result();
  void poll_store();
  void handle_result();
  void end_check(HealthcheckResult result, string message);
  void register_step(int status, void (Healthcheck_mysql::*method)());
  void register_timeout_event();
  int event_flag_to_wait_status();
  static void handle_io_event(int fd, short flag, void *arg);
  static void handle_timeout_event(int fd, short flag, void *arg);

  // Members
protected:
  string host;
  int port;
  string dbname;
  string user;
  string password;
  string query;
  MYSQL *conn;
  MYSQL *conn_ret; // Out-param of mysql_real_connect_start()/_cont()
  int query_ret;   // Out-param of mysql_real_query_start()/_cont()
  MYSQL_RES *result = NULL;
  struct event *io_event;
  struct event *timeout_event;
  short event_flag;
  int event_counter;
  void (Healthcheck_mysql::*callback_method)();
};

#endif
