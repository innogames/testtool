/*
 * Testtool - Generals
 *
 * Copyright (c) 2018 InnoGames GmbH
 */

#ifndef _TESTTOOL_H
#define _TESTTOOL_H

#include <string>
#include <list>

class LbVip;
class LbPool;

class TestTool {
public:
    TestTool(string config_file_name);
    void load_config();
    void load_downtimes();

    void setup_events();
    void dump_status();
    void configure_bgp();

    void schedule_healthchecks();
    void finalize_healthchecks();
    boost::interprocess::message_queue* pfctl_mq;

private:
    string config_file_name;
    set<string> downtimes;
    std::map<std::string, LbPool*> lb_pools;
    std::set<string*> bird_ips_alive[2];
};

#endif
