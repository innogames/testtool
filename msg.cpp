#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "msg.h"


static void _showMsg(msgType type, const char *msg, va_list ap){
	char str[4096];
	int off;
	int nots = 0;
	
	off = sprintf(str, CL_WHITE"["CL_RESET);
	
	// Format on type:
	switch(type){
		case MSG_TYPE_NONE:
			nots = 1;
		
		case MSG_TYPE_MSG: 
			off = 0;   *str = 0;
		break;
		
		case MSG_TYPE_STATUS:
			off += sprintf(str+off, CL_GREEN"Status");
		break;
		
		case MSG_TYPE_WARN: 
			off += sprintf(str+off, CL_YELLOW"Warning");
		break;
		
		case MSG_TYPE_ERROR:
			off += sprintf(str+off, CL_RED"Error");
		break;
	}

	if(off > 0){
		off += sprintf(str+off, CL_WHITE"]"CL_RESET" ");
			
	}
	
	vsnprintf(&str[off], sizeof(str)-128, msg, ap);

	
	// Show time every call to the proc, in front of the 
	// output
	if(nots == 0){
		struct timeval  tv;
		struct tm      *tm;
		char   buf[80];

		gettimeofday(&tv, NULL);
		tm = localtime(&tv.tv_sec);
				
		strftime(buf, sizeof(buf), "(%Y-%m-%d %H:%M:%S", tm);
		printf("%s.%06u) ", buf, (int)tv.tv_usec);
		
			
	}

	printf("%s", str);
	fflush(stdout);

}


void show(const char *fmt, ...){
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	va_end(ap);
	
	_showMsg(MSG_TYPE_NONE, fmt, ap2);
}

void showMessage(const char *fmt, ...){
	va_list ap, ap2;
	
	va_start(ap, fmt);
	va_copy(ap2, ap);
	va_end(ap);
	
	_showMsg(MSG_TYPE_NONE, fmt, ap2);
}

void showStatus(const char *fmt, ...){
	va_list ap, ap2;
	
	va_start(ap, fmt);
	va_copy(ap2, ap);
	va_end(ap);
	
	_showMsg(MSG_TYPE_STATUS, fmt, ap2);
}

void showWarning(const char *fmt, ...){
	va_list ap, ap2;
	
	va_start(ap, fmt);
	va_copy(ap2, ap);
	va_end(ap);
	
	_showMsg(MSG_TYPE_WARN, fmt, ap2);
}

void showError(const char *fmt, ...){
	va_list ap, ap2;
	
	va_start(ap, fmt);
	va_copy(ap2, ap);
	va_end(ap);
	
	_showMsg(MSG_TYPE_ERROR, fmt, ap2);
}

