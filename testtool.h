#ifndef _TESTTOOL_H
#define _TESTTOOL_H

#include <string>
#include <list>

class LbVip;
class LbPool;

class TestTool {
public:
    void load_config(ifstream &config_file);
    void load_downtimes();

    void setup_events();
    void dump_status();
    void configure_bgp();

    void schedule_healthchecks();
    void parse_healthchecks_results();

private:
    std::list<LbVip*> m_vips;
    std::list<LbPool*> m_pools;
    std::set<string*> ips4_alive;
    std::set<string*> ips6_alive;
};

#endif
