#include <iostream>
#include <set>
#include <string>
#include <sstream>
#include <boost/algorithm/string.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio.hpp>
#include <fmt/format.h>

#include <stdarg.h>
#include <stdio.h>

#include "msg.h"
#include "pfctl.h"

using namespace std;

extern bool	 pf_action;
extern int	 verbose_pfctl;

bool pfctl_run_command(vector<string> *args, vector<string> *lines) {
	int   ret = 0;
	FILE *fp;
	char  buffer[1024];

	string cmd = "/sbin/pfctl -q";

	for (auto arg: *args) {
		cmd += " " + arg;
	}

	if (!pf_action)
		return true;

	if (verbose_pfctl) {
		log(MSG_INFO, cmd);
	}

	fp = popen(cmd.c_str(), "r");
	if (!fp) {
		log(MSG_CRIT, "Can't spawn pfctl process: " + cmd);
		return false;
	}

	if (lines != NULL) {
		string strbuffer;
		while (fgets(buffer, 1024, fp) != NULL) {
			strbuffer.append(buffer);
		}
		istringstream istrbuffer(strbuffer);
		for (string line; getline(istrbuffer, line);) {
			lines->push_back(line);

		}
	}
	pclose(fp);

	if (ret != 0) {
		log(MSG_CRIT, "pfctl failed with error code: " + cmd);
		return false;
	}
	return true;
}


bool pf_table_add(string *table, set<string> *addresses) {
	if (addresses->size() == 0)
		return true;
	vector<string> cmd;
	cmd.push_back("-t");
	cmd.push_back(*table);
	cmd.push_back("-T");
	cmd.push_back("add");
	for (auto address: *addresses) {
		cmd.push_back(address);
	}
	return pfctl_run_command(&cmd, NULL);
}

bool pf_table_del(string *table, set<string> *addresses) {
	if (addresses->size() == 0)
		return true;
	vector<string> cmd;
	cmd.push_back("-t");
	cmd.push_back(*table);
	cmd.push_back("-T");
	cmd.push_back("del");
	for (auto address: *addresses) {
		cmd.push_back(address);
	}
	return pfctl_run_command(&cmd, NULL);
}

bool pf_kill_src_nodes_to(string *table, string *address, bool with_states) {
	vector<string> cmd;
	cmd.push_back("-K");
	cmd.push_back("table");
	cmd.push_back("-K");
	cmd.push_back(*table);
	cmd.push_back("-K");
	cmd.push_back("dsthost");
	cmd.push_back("-K");
	cmd.push_back(*address);
	if (with_states) {
		cmd.push_back("-K");
		cmd.push_back("kill");
		cmd.push_back("-K");
		cmd.push_back("rststates");
	}

	return pfctl_run_command(&cmd, NULL);
}

bool pf_kill_states_to_rdr(string *table, string *address, bool with_states) {
	vector<string> cmd;
	cmd.push_back("-k");
	cmd.push_back("table");
	cmd.push_back("-k");
	cmd.push_back(*table);
	cmd.push_back("-k");
	cmd.push_back("rdrhost");
	cmd.push_back("-k");
	cmd.push_back(*address);
	if (with_states) {
		cmd.push_back("-k");
		cmd.push_back("kill");
		cmd.push_back("-k");
		cmd.push_back("rststates");
	}

	return pfctl_run_command(&cmd, NULL);
}

bool pf_get_table(string *table, set<string> *result) {
	vector<string> cmd;
	cmd.push_back("-t");
	cmd.push_back(*table);
	cmd.push_back("-T");
	cmd.push_back("show");

	vector<string> out;
	bool ret = pfctl_run_command(&cmd, &out);
	if (! ret){
		return false;
	}
	boost::system::error_code ec;
	if (verbose_pfctl) {
		log(MSG_INFO, "IP addresses in table");
	}
	for (auto line: out) {
		boost::trim(line);
		boost::asio::ip::address::from_string(line, ec);
		if (ec)
			log(MSG_CRIT, fmt::sprintf("Not an IP Address: '%s'", line));
		else {
			if (verbose_pfctl) {
				log(MSG_INFO, line);
			}
			result->insert(line);
		}
	}
	return true;
}


/*
   Check if an IP address is in the given table.
*/
bool pf_is_in_table(string *table, string *address, bool *answer) {
	set<string> lines;
	bool ret = pf_get_table(table, &lines);
	if (! ret) {
		return false;
	}

	*answer = false;
	for (auto line: lines ) {
		if (line == *address)
			*answer = true;
	}

	return true;
}


/*
   Remove src_nodes to all IPs in the given table apart from the specified ones.
   States of existing connections will not be killed, only src_nodes.
*/
bool pf_table_rebalance(string *table, set<string> *skip_addresses) {
	bool ret;
	set<string> addresses;
	ret = pf_get_table(table, &addresses);
	if (! ret) {
		return false;
	}

	for (auto address: addresses) {
		if (skip_addresses->find(address) == skip_addresses->end()) {
			ret = pf_kill_src_nodes_to(table, &address, false);
			if (! ret) {
				return false;
			}
		}
	}
	return true;
}

bool pf_sync_table(string *table, set<string> *want_set) {
	set<string> cur_set;

	if (!pf_get_table(table, &cur_set))
		return false;

	std::set<string> to_add;
	std::set_difference(
		want_set->begin(), want_set->end(),
		cur_set.begin(), cur_set.end(),
		std::inserter(to_add, to_add.end())
	);

	/* Add wanted nodes to table */
	if (!pf_table_add(table, &to_add))
		return false;
	/* Kill src_nodes to old entries from table so that connections get rebalanced. */
	pf_table_rebalance(table, &to_add);

	std::set<string> to_del;
	std::set_difference(
		cur_set.begin(), cur_set.end(),
		want_set->begin(), want_set->end(),
		std::inserter(to_del, to_del.end())
	);
	/* Remove unwanted nodes from table. */
	if (!pf_table_del(table, &to_del))
		return false;
	for (auto del: to_del) {
		/* Kill all src_nodes, linked states and unlinked states. */
		pf_kill_src_nodes_to(table, &del, true);
		pf_kill_states_to_rdr(table, &del, true);
		/* Kill nodes again, there might be some which were created after last
		 * kill due to belonging to states with deferred src_nodes.
		 * See TECH-6711 and around. */
		pf_kill_src_nodes_to(table, &del, true);
	}

	return true;
}
