//
// Testtool - Logging Routines
//
// Copyright (c) 2018 InnoGames GmbH
//

#define FMT_HEADER_ONLY

#include <fmt/format.h>
#include <fmt/printf.h>
#include <iostream>
#include <string>
#include <syslog.h>

#include "lb_pool.h"
#include "msg.h"

using namespace std;

void start_logging() { openlog("testtool", LOG_PID, LOG_LOCAL3); }

void log(MessageType loglevel, string msg) {
  cout << msg << endl;
  int sysloglevel;
  switch (loglevel) {
  case MessageType::MSG_INFO:
    sysloglevel = LOG_INFO;
    break;
  case MessageType::MSG_CRIT:
    sysloglevel = LOG_CRIT;
    break;
  case MessageType::MSG_DEBUG:
    sysloglevel = LOG_DEBUG;
    break;
  case MessageType::MSG_STATE_UP:
    sysloglevel = LOG_INFO;
    break;
  case MessageType::MSG_STATE_DOWN:
    sysloglevel = LOG_INFO;
    break;
  }
  syslog(sysloglevel | LOG_LOCAL3, "%s", msg.c_str());
}

void log(MessageType loglevel, LbPool *lbpool, string msg) {
  string out = "lbpool: " + lbpool->name + " " + msg;
  log(loglevel, out);
}

void log(MessageType loglevel, LbNode *lbnode, string msg) {
  string out = "lbnode: " + lbnode->name + " " + msg;
  log(loglevel, lbnode->parent_lbpool, out);
}

void log(MessageType loglevel, Healthcheck *hc, string msg) {
  string out = "healthcheck: " + hc->type + " " + hc->log_prefix + " " + msg;
  log(loglevel, hc->parent_lbnode, out);
}
