#ifndef _CONFIG_H
#define _CONFIG_H

int parse_int(YAML::Node node, int def_val);
std::string parse_string(YAML::Node node, std::string def_val);

#endif

