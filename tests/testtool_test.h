#ifndef TESTTOOL_TEST_H
#define TESTTOOL_TEST_H

#include "cmake_dirs.h"
#include "healthcheck_dummy.h"
#include "lb_node.h"
#include "lb_pool.h"

using namespace std;
using namespace boost::interprocess;

using json = nlohmann::json;

class LbPoolTestException : virtual public runtime_error {
public:
  explicit LbPoolTestException(const string &msg) : runtime_error(msg) {}
  virtual ~LbPoolTestException() throw() {}
};

class TesttoolTest : public ::testing::Test {
protected:
  map<string, LbPool *> lb_pools;
  json base_config;
  set<LbNode *> up_nodes_test;
  string test_lb_pool = "lbpool.example.com";

  virtual void SetUp();
  virtual void SetUp(bool init_state);
  virtual void TearDown();
  void EndDummyHC(string lb_pool_name, string lb_node_name,
                  HealthcheckResult result, bool all_hcs);
  LbNode *GetLbNode(string lb_pool_name, string lb_node_name);
  LbNodeState GetLbNodeState(string lb_pool_name, string lb_node_name);
  set<string> UpNodesNames();
};

#endif
