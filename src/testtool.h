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
    void load_config(string config_file);
    void load_downtimes();

    void setup_events();
    void dump_status();
    void configure_bgp();

    void schedule_healthchecks();
    void finalize_healthchecks();
    boost::interprocess::message_queue* pfctl_mq;

private:
    set<string> downtimes;
    std::map<std::string, LbPool*> lb_pools;
    std::set<string*> bird_ips_alive[2];
};

#endif
