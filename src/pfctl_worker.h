#ifndef _PFCTL_WORKER_H_
#define _PFCTL_WORKER_H_

#include <boost/interprocess/ipc/message_queue.hpp>
#include "lb_node.h"
#include "lb_pool.h"

using namespace std;
using namespace boost::interprocess;

#define NAME_LEN 256 // 64 in Serveradmin
#define QUEUE_LEN 100 // Totally arbitrary number
#define MAX_NODES 20 // I hope 20 addresses is reasonable enough
#define ADDR_LEN sizeof("FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:255.255.255.255") + 1

message_queue* start_pfctl_worker();
void stop_pfctl_worker();
void send_message(message_queue* mq, string pool_name, string table_name, set<LbNode*> lb_nodes);

typedef struct {
	// This struct is sent over a queue, complex datatypes won't work here.
	char pool_name[NAME_LEN];
	char table_name[NAME_LEN];
	char wanted_addresses[MAX_NODES][ADDR_LEN];
} pfctl_msg;

#endif

