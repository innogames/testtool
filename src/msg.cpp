#include <string>
#include <iostream>
#include <syslog.h>
#include <fmt/format.h>
#include <fmt/printf.h>

#include "msg.h"
#include "lb_pool.h"

using namespace std;

void start_logging() {
	openlog("testtool", LOG_PID, LOG_LOCAL3);
}

void log(int loglevel, string msg) {
	cout << msg << endl;
	int sysloglevel;
	switch(loglevel) {
		case MSG_INFO:
			sysloglevel = LOG_INFO;
		break;
		case MSG_CRIT:
			sysloglevel = LOG_CRIT;
		break;
		case MSG_DEBUG:
			sysloglevel = LOG_DEBUG;
		break;
		case MSG_STATE_UP:
			sysloglevel = LOG_INFO;
		break;
		case MSG_STATE_DOWN:
			sysloglevel = LOG_INFO;
		break;
	}
	syslog(sysloglevel | LOG_LOCAL3, "%s", msg.c_str());
}

void log(int loglevel, LbPool* lbpool, string msg) {
	string out  =  "lbpool: " + lbpool->name + " " + msg;
	log(loglevel, out);
}

void log(int loglevel, LbNode *lbnode, string msg) {
	string out  =  "lbnode: " + lbnode->name + " " + msg;
	log(loglevel, lbnode->parent_lbpool, out);
}

void log(int loglevel, Healthcheck *hc, string msg) {
	string out  =  "healthcheck: " + hc->type + " " + hc->log_prefix + " " + msg;
	log(loglevel, hc->parent_lbnode, out);
}
