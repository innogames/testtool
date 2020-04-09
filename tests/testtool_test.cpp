//
// Tests for testtool
//

#include <boost/interprocess/ipc/message_queue.hpp>
#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <string>

#include "lb_node.h"
#include "msg.h"

using namespace std;
using namespace boost::interprocess;

// Check if an IP address is in the given table. Global variable used for
// faking state input for tests.
bool _pf_is_in_table = false;
bool pf_is_in_table(string *table, string *address, bool *answer) {
  *answer = _pf_is_in_table;
  return true;
}

bool send_message(message_queue *mq, string pool_name, string table_name,
                  set<LbNode *> all_lb_nodes, set<LbNode *> up_lb_nodes) {
  return true;
}

void log(MessageType loglevel, string msg){};
void log(MessageType loglevel, LbPool *lbpool, string msg){};
void log(MessageType loglevel, LbNode *lbnode, string msg){};
void log(MessageType loglevel, Healthcheck *hc, string msg){};

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
