#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <syslog.h>

#include "msg.h"

void start_logging() {
	openlog("testtool", LOG_PID, LOG_LOCAL3);
}


void show_message(msgType type, const char *fmt, ...) {
	char message_buf[4096];
	char cmsgtype_buf[128];
	char msgtype_buf[128];
	char timebuf[128];
	int off = 0;
	int loglevel;

	message_buf[0] = 0;
	cmsgtype_buf[0] = 0;
	msgtype_buf[0] = 0;

	va_list args;
	va_start(args, fmt);
	vsnprintf(message_buf, sizeof(message_buf), fmt, args);
	va_end(args);

	// Format on type:
	switch(type) {
		/* General messages. */
		case MSG_TYPE_NOTICE:
			loglevel = LOG_NOTICE;
			snprintf(cmsgtype_buf, sizeof(cmsgtype_buf), CL_WHITE"["CL_CYAN  "  Notice   "CL_WHITE"]"CL_RESET);
			snprintf(msgtype_buf, sizeof(msgtype_buf), "Notice");
		break;
		
		case MSG_TYPE_WARNING:
			loglevel = LOG_WARNING;
			snprintf(cmsgtype_buf, sizeof(cmsgtype_buf), CL_WHITE"["CL_YELLOW"  Warning  "CL_WHITE"]"CL_RESET);
			snprintf(msgtype_buf, sizeof(msgtype_buf), "Warning");
		break;
		
		case MSG_TYPE_ERROR:
			loglevel = LOG_ERR;
			snprintf(cmsgtype_buf, sizeof(cmsgtype_buf), CL_WHITE"["CL_RED   "   Error   "CL_WHITE"]"CL_RESET);
			snprintf(msgtype_buf, sizeof(msgtype_buf), "Error");
		break;

		/* Node state changes. */
		case MSG_TYPE_NODE_UP:
			loglevel = LOG_NOTICE;
			snprintf(cmsgtype_buf, sizeof(cmsgtype_buf), CL_WHITE"["CL_GREEN "  Node Up  "CL_WHITE"]"CL_RESET);
			snprintf(msgtype_buf, sizeof(msgtype_buf), "Node Up");
		break;

		case MSG_TYPE_NODE_DOWN:
			loglevel = LOG_WARNING;
			snprintf(cmsgtype_buf, sizeof(cmsgtype_buf), CL_WHITE"["CL_YELLOW" Node Down "CL_WHITE"]"CL_RESET);
			snprintf(msgtype_buf, sizeof(msgtype_buf), "Node Down");
		break;

		/* Healthcheck results. */
		case MSG_TYPE_HC_PASS:
			loglevel = LOG_NOTICE;
			snprintf(cmsgtype_buf, sizeof(cmsgtype_buf), CL_WHITE"["CL_GREEN " HC Passed "CL_WHITE"]"CL_RESET);
			snprintf(msgtype_buf, sizeof(msgtype_buf), "HC Passed");
		break;

		case MSG_TYPE_HC_FAIL:
			loglevel = LOG_ERR;
			snprintf(cmsgtype_buf, sizeof(cmsgtype_buf), CL_WHITE"["CL_RED   " HC Failed "CL_WHITE"]"CL_RESET);
			snprintf(msgtype_buf, sizeof(msgtype_buf), "HC Failed");
		break;

		case MSG_TYPE_HC_HFAIL:
			loglevel = LOG_ERR;
			snprintf(cmsgtype_buf, sizeof(cmsgtype_buf), CL_WHITE"["CL_RED   "HC HardFail"CL_WHITE"]"CL_RESET);
			snprintf(msgtype_buf, sizeof(msgtype_buf), "HC HardFail");
		break;

		/* pfctl operations */
		case MSG_TYPE_PFCTL:
			loglevel = LOG_NOTICE;
			snprintf(cmsgtype_buf, sizeof(cmsgtype_buf), CL_WHITE"["CL_CYAN  "   pfctl   "CL_WHITE"]"CL_RESET);
			snprintf(msgtype_buf, sizeof(msgtype_buf), "pfctl");
		break;

		case MSG_TYPE_DEBUG:
			loglevel = LOG_DEBUG;
		break;
	}

	struct timeval  tv;
	struct tm      *tm;

	gettimeofday(&tv, NULL);
	tm = localtime(&tv.tv_sec);
				
	off = strftime(timebuf, sizeof(timebuf), "(%Y-%m-%d %H:%M:%S", tm);
	snprintf(timebuf+off, sizeof(timebuf)-off, ".%06u)", (int)tv.tv_usec);

	if (type == MSG_TYPE_DEBUG) {
		printf("%s\n", message_buf);
		syslog(loglevel | LOG_LOCAL3, message_buf);
	} else {
		syslog(loglevel | LOG_LOCAL3, "%s - %s", msgtype_buf, message_buf);
		printf("%s %s %s\n", timebuf, cmsgtype_buf, message_buf);
	}
	fflush(stdout);

}

