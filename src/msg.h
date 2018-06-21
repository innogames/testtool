/*
 * Testtool - Logging Routines
 *
 * Copyright (c) 2018 InnoGames GmbH
 */

#ifndef _MSG_H_
#define _MSG_H_

#include <string>
#include "lb_pool.h"

using namespace std;

typedef enum msgType {
	MSG_INFO, MSG_CRIT, MSG_DEBUG,
	MSG_STATE_UP, MSG_STATE_DOWN,
} msgType;

void start_logging();

void log(int loglevel, string msg);
void log(int loglevel, LbPool* lbpool, string msg);
void log(int loglevel, LbNode *lbnode, string msg);
void log(int loglevel, Healthcheck *hc, string msg);

#endif
