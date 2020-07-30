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
                  set<LbNode *> all_lb_nodes, set<LbNode *> up_lb_nodes);

typedef struct {
  LbNodeState wanted_state;     // To add or remove LB Node from table.
  LbNodeAdminState admin_state; // How to remove LB Node from table.
  // IPv4 and IPv6 addresses.
  char ip_address[2][ADDR_LEN];
} SyncedLbNode;

typedef struct {
  // This struct is sent over a queue, complex datatypes won't work here.
  char pool_name[NAME_LEN];
  char table_name[NAME_LEN];
  SyncedLbNode synced_lb_nodes[MAX_NODES];
} pfctl_msg;

#endif
