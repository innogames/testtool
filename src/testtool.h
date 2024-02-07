//
// Testtool - Generals
//
// Copyright (c) 2018 InnoGames GmbH
//

#ifndef _TESTTOOL_H
#define _TESTTOOL_H

#include <list>
#include <string>

class LbVip;
class LbPool;

class TestTool {

  // Methods
public:
  TestTool(string config_file_name);
  void load_config();
  void load_downtimes();

  void setup_events();
  void dump_status();

  void schedule_healthchecks();
  void finalize_healthchecks();
  void sync_lbpools_without_healthchecks();
  boost::interprocess::message_queue *pfctl_mq;

  // Members
private:
  string config_file_name;
  set<string> downtimes;
  std::map<std::string, LbPool *> lb_pools;
  std::set<string *> bird_ips_alive[2];
};

#endif
