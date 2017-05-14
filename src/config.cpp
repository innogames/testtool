#include <iostream>
#include <yaml-cpp/yaml.h>

#include "config.h"

using namespace std;

int parse_int(YAML::Node node, int def_val) {
	try {
		return node.as<int>();
	} catch (YAML::BadConversion) {
		return def_val;
	} catch (YAML::InvalidNode) {
		return def_val;
	}
}

bool node_defined(YAML::Node node) {
	if (!node || node.Type() == YAML::NodeType::Null ) {
		return false;
	}
	if (node.Type() == YAML::NodeType::Sequence && node.size() == 0 ) {
		return false;
	}
	return true;
}

std::string parse_string(YAML::Node node, std::string def_val) {
	try {
		return node.as<std::string>();
	} catch (YAML::BadConversion) {
		return def_val;
	} catch (YAML::InvalidNode) {
		return def_val;
	}
}
