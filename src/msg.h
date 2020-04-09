//
// Testtool - Logging Routines
//
// Copyright (c) 2018 InnoGames GmbH
//

#ifndef _MSG_H_
#define _MSG_H_

#include <string>

#include "lb_pool.h"

using namespace std;

enum class MessageType {
  MSG_INFO,
  MSG_CRIT,
  MSG_DEBUG,
  MSG_STATE_UP,
  MSG_STATE_DOWN,
};

void start_logging();

void log(MessageType loglevel, string msg);
void log(MessageType loglevel, LbPool *lbpool, string msg);
void log(MessageType loglevel, LbNode *lbnode, string msg);
void log(MessageType loglevel, Healthcheck *hc, string msg);

#endif
