//
// Testtool - PF Control Worker
//
// Copyright (c) 2018 InnoGames GmbH
//

#ifndef _PFCTL_WORKER_H_
#define _PFCTL_WORKER_H_

#include <boost/interprocess/ipc/message_queue.hpp>

#include "lb_node.h"
#include "lb_pool.h"

using namespace std;
using namespace boost::interprocess;

#define NAME_LEN 256 // 64 in Serveradmin
// Operations which got into queue are installed as soon as possible.
// Operations which did not fit will be re-done after given HC finishes its
// next run. Usually checks run each 2000ms and each operation takes around
// 120ms. Optimal lenght would be 16. Keep it a bit shorter in case operations
// take way longer, for example when HWLB is under a DDoS.
#define QUEUE_LEN 10
#define MAX_NODES 20 // I hope 20 LB Nodes is reasonable enough
#define ADDR_LEN sizeof("FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:255.255.255.255") + 1

message_queue *start_pfctl_worker();
void stop_pfctl_worker();
bool send_message(message_queue *mq, string pool_name, string table_name,
                  set<LbNode *> lb_nodes);

typedef struct {
  // This struct is sent over a queue, complex datatypes won't work here.
  char pool_name[NAME_LEN];
  char table_name[NAME_LEN];
  char wanted_addresses[MAX_NODES * 2][ADDR_LEN];
} pfctl_msg;

#endif
