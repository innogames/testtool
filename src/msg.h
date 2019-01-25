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

typedef enum msgType {
  MSG_INFO,
  MSG_CRIT,
  MSG_DEBUG,
  MSG_STATE_UP,
  MSG_STATE_DOWN,
} msgType;

void init_logging();

void log(msgType loglevel, LbPool *lbpool, string msg);
void log(msgType loglevel, LbNode *lbnode, string msg);
void log(msgType loglevel, Healthcheck *hc, string msg);
void log(msgType loglevel, string msg);

#endif
