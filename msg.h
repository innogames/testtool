#ifndef _MSG_H_
#define _MSG_H_



#define CL_RESET    "\033[0m"
#define CL_CLS      "\033[2J"
#define CL_CLL      "\033[K"
#define CL_BOLD     "\033[1m"
#define CL_NORM     CL_RESET
#define CL_NORMAL   CL_RESET
#define CL_NONE     CL_RESET
#define CL_WHITE    "\033[1;37m"
#define CL_GRAY     "\033[1;30m"
#define CL_RED      "\033[1;31m"
#define CL_GREEN    "\033[1;32m"
#define CL_YELLOW   "\033[1;33m"
#define CL_BLUE     "\033[1;34m"
#define CL_MAGENTA  "\033[1;35m"
#define CL_CYAN     "\033[1;36m"


typedef enum msgType {
	MSG_TYPE_NODE_UP, MSG_TYPE_NODE_DOWN,
	MSG_TYPE_POOL_UP, MSG_TYPE_POOL_DOWN, MSG_TYPE_POOL_CRIT,
	MSG_TYPE_HC_PASS, MSG_TYPE_HC_FAIL, MSG_TYPE_HC_HFAIL,
	MSG_TYPE_HC_ERROR, MSG_TYPE_HC_FATAL, MSG_TYPE_HC_PANIC,
	MSG_TYPE_PFCTL,
	MSG_TYPE_DEBUG, MSG_TYPE_CRITICAL
} msgType;


void start_logging();
void log_txt(msgType type, const char *fmt, ...);
void log_lb(msgType type, const char *lb_pool, const char *lb_node, const int port, const  char *fmt, ...);

#endif
