/*
 * Testtool - Configuration
 *
 * Copyright (c) 2018 InnoGames GmbH
 */

#ifndef _CONFIG_H
#define _CONFIG_H

int parse_int(YAML::Node node, int def_val);
bool node_defined(YAML::Node node);
std::string parse_string(YAML::Node node, std::string def_val);

#endif
