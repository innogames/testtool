#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <syslog.h>

#include "msg.h"

/*
     LOG_EMERG     A panic condition.  This is normally broadcast to all
                   users.

     LOG_ALERT     A condition that should be corrected immediately, such as a
                   corrupted system database.

     LOG_CRIT      Critical conditions, e.g., hard device errors.

     LOG_ERR       Errors.

     LOG_WARNING   Warning messages.

     LOG_NOTICE    Conditions that are not error conditions, but should possi-
                   bly be handled specially.

     LOG_INFO      Informational messages.

     LOG_DEBUG     Messages that contain information normally of use only when
                   debugging a program.
*/

void start_logging() {
	openlog("testtool", LOG_PID, LOG_LOCAL3);
}

void log_txt(msgType type, const char *fmt, ...) {
	char mbuf[4096]; // Message body buffer.
	char cbuf[128];  // Colourful stamp buffer.
	char pbuf[128];  // Plaintext stamp buffer.
	char tbuf[128];  // Timestamp buffer.
	int loglevel;

	memset(mbuf, 0, sizeof(mbuf));
	memset(cbuf, 0, sizeof(cbuf));
	memset(pbuf, 0, sizeof(pbuf));
	memset(tbuf, 0, sizeof(tbuf));

	va_list args;
	va_start(args, fmt);
	vsnprintf(mbuf, sizeof(mbuf), fmt, args);
	va_end(args);

	switch(type) {
		/* Node state changes. */
		case MSG_TYPE_NODE_UP:
			loglevel = LOG_NOTICE;
			snprintf(cbuf, sizeof(cbuf), CL_WHITE "[" CL_GREEN "  Node Up  " CL_WHITE  "]" CL_RESET);
			snprintf(pbuf, sizeof(pbuf), "Node up");
		break;

		case MSG_TYPE_NODE_DOWN:
			loglevel = LOG_CRIT;
			snprintf(cbuf, sizeof(cbuf), CL_WHITE "[" CL_YELLOW" Node Down " CL_WHITE  "]" CL_RESET);
			snprintf(pbuf, sizeof(pbuf), "Node down");
		break;

		/* Pool state changes. */
		case MSG_TYPE_POOL_UP:
			loglevel = LOG_NOTICE;
			snprintf(cbuf, sizeof(cbuf), CL_WHITE "[" CL_GREEN "  Pool Up  " CL_WHITE  "]" CL_RESET);
			snprintf(pbuf, sizeof(pbuf), "Pool up");
		break;

		case MSG_TYPE_POOL_DOWN:
			loglevel = LOG_CRIT;
			snprintf(cbuf, sizeof(cbuf), CL_WHITE "[" CL_YELLOW" Pool Down " CL_WHITE  "]" CL_RESET);
			snprintf(pbuf, sizeof(pbuf), "Pool down");
		break;

		case MSG_TYPE_POOL_CRIT:
			loglevel = LOG_CRIT;
			snprintf(cbuf, sizeof(cbuf), CL_RED   "[" CL_GREEN " Pool Error " CL_RED    "]" CL_RESET);
			snprintf(pbuf, sizeof(mbuf), "Pool Error");
		break;

		/* Healthcheck results. */
		case MSG_TYPE_HC_PASS:
			loglevel = LOG_NOTICE;
			snprintf(cbuf, sizeof(cbuf), CL_WHITE "[" CL_GREEN " HC Passed " CL_WHITE  "]" CL_RESET);
			snprintf(pbuf, sizeof(pbuf), "HC Passed");
		break;

		case MSG_TYPE_HC_FAIL:
			loglevel = LOG_ERR;
			snprintf(cbuf, sizeof(cbuf), CL_WHITE "[" CL_RED   " HC Failed " CL_WHITE  "]" CL_RESET);
			snprintf(pbuf, sizeof(mbuf), "HC Failed");
		break;

		case MSG_TYPE_HC_HFAIL:
			loglevel = LOG_ERR;
			snprintf(cbuf, sizeof(cbuf), CL_WHITE "[" CL_RED   "HC HardFail" CL_WHITE  "]" CL_RESET);
			snprintf(pbuf, sizeof(mbuf), "HC HardFail");
		break;

		case MSG_TYPE_HC_PANIC:
			loglevel = LOG_ERR;
			snprintf(cbuf, sizeof(cbuf), CL_RED   "[" CL_BLUE  " HC Panic  " CL_RED    "]" CL_RESET);
			snprintf(pbuf, sizeof(mbuf), "HC Panic   ");
		break;

		/* pfctl operations */
		case MSG_TYPE_PFCTL:
			loglevel = LOG_NOTICE;
			snprintf(cbuf, sizeof(cbuf), CL_WHITE "[" CL_CYAN  "   pfctl   " CL_WHITE  "]" CL_RESET);
			snprintf(pbuf, sizeof(pbuf), "pfctl");
		break;

		case MSG_TYPE_DEBUG:
			loglevel = LOG_DEBUG;
		break;

		case MSG_TYPE_CRITICAL:
			loglevel = LOG_CRIT;
		break;

	}

	/* Prepare timestamp for stdout output. */
	struct timeval  tv;
	struct tm      *tm;
	int off;
	gettimeofday(&tv, NULL);
	tm = localtime(&tv.tv_sec);
	off = strftime(tbuf, sizeof(tbuf), "(%Y-%m-%d %H:%M:%S", tm);
	snprintf(tbuf+off, sizeof(tbuf)-off, ".%06u)", (int)tv.tv_usec);


	/* Syslog gets plain text message with proper log level. */
	syslog(loglevel | LOG_LOCAL3, "%s - %s", pbuf, mbuf);

	/* Stdout gets a colourful message with timestamp. */
	printf("%s %s %s\n", tbuf, cbuf, mbuf);

	fflush(stdout);


}

void log_lb(msgType type, const char *lb_pool, const char *lb_node, const int port, const char *fmt, ...) {
	char message_buf[4096];

	va_list args;
	va_start(args, fmt);
	vsnprintf(message_buf, sizeof(message_buf), fmt, args);
	va_end(args);

	/* This one is to be parsed by logstash. */
	if (port)
		log_txt(type, "%s - %s:%d - %s", lb_pool, lb_node, port, message_buf);
	else
		log_txt(type, "%s - %s - %s", lb_pool, lb_node, message_buf);

}

