#include <iostream>
#include <string>
#include <sstream>

#include "msg.h"
#include "pfctl.h"

using namespace std;

extern bool	 pf_action;
extern int	 verbose_pfctl;

/*
   Kill states created by pf rules using given table and IP address for redirection.
*/
void pf_kill_states_to_rdr(string &pool, string &rdr_ip, bool with_states) {
	FILE	*fp;
	int	 ret;
	char	 cmd[1024];

	if (with_states) {
		log_txt(MSG_TYPE_PFCTL, "%s %s - killing all states with RST", pool.c_str(), rdr_ip.c_str());
		snprintf(cmd, sizeof(cmd), "pfctl -q -k table -k '%s' -k rdrhost -k '%s' -k kill -k rststates", pool.c_str(), rdr_ip.c_str());
	} else {
		log_txt(MSG_TYPE_PFCTL, "%s %s - killing all states", pool.c_str(), rdr_ip.c_str());
		snprintf(cmd, sizeof(cmd), "pfctl -q -k table -k '%s' -k rdrhost -k '%s'", pool.c_str(), rdr_ip.c_str());
	}

	if (verbose_pfctl)
		log_txt(MSG_TYPE_PFCTL, "command: %s", cmd);

	if(!pf_action)
		return;

	fp = popen(cmd, "r");
	if(fp == NULL){
		log_txt(MSG_TYPE_PFCTL, "cannot spawn pfctl process to kill states %s %s", pool.c_str(), rdr_ip.c_str());
		return;
	}

	ret = pclose(fp);
	if(ret == -1 || ret != 0){

		log_txt(MSG_TYPE_PFCTL, "pf_kill_states_rdr('%s', '%s'): returned bad status (%d)", pool.c_str(), rdr_ip.c_str(), ret);
	}

}//end: pf_states_kill


/*
   Add an IP address to the specified table.
*/
void pf_table_add(string &table, string &ip){
	FILE	*fp;
	char	 cmd[1024];
	int	 ret;

	log_txt(MSG_TYPE_PFCTL, "%s %s - adding node", table.c_str(), ip.c_str());

	snprintf(cmd, sizeof(cmd), "pfctl -q -t '%s' -T add '%s'", table.c_str(), ip.c_str());

	if (verbose_pfctl)
		log_txt(MSG_TYPE_PFCTL, "command: %s", cmd);

	if(!pf_action)
		return;

	fp = popen(cmd, "r");
	if(fp == NULL){
		log_txt(MSG_TYPE_PFCTL, "cannot spawn pfctl process to add ip '%s' to table '%s'", ip.c_str(), table.c_str());
		return;
	}

	ret = pclose(fp);

	if(ret == -1 || ret != 0){
		log_txt(MSG_TYPE_PFCTL, "pf_table_add('%s', '%s'): returned bad status (%d)", table.c_str(), ip.c_str(), ret);
	}

}//end: pf_table_add


/*
   Remove an IP address from the specified table.
*/
void pf_table_del(string &table,  string &ip){
	FILE	*fp;
	char	 cmd[1024];
	int	 ret;

	log_txt(MSG_TYPE_PFCTL, "%s %s - removing node", table.c_str(), ip.c_str());

	snprintf(cmd, sizeof(cmd), "pfctl -q -t '%s' -T del '%s'", table.c_str(), ip.c_str());

	if (verbose_pfctl)
		log_txt(MSG_TYPE_PFCTL, "command: %s", cmd);

	if(!pf_action)
		return;

	fp = popen(cmd, "r");
	if(fp == NULL){
		log_txt(MSG_TYPE_PFCTL, "cannot spawn pfctl process to del ip '%s' from table '%s'", ip.c_str(), table.c_str());
		return;
	}

	ret = pclose(fp);

	if(ret == -1 || ret != 0){
		log_txt(MSG_TYPE_PFCTL, "pf_table_del('%s', '%s'): returned bad status (%d)", table.c_str(), ip.c_str(), ret);
	}

}//end: pf_table_del


/*
   Kill src_nodes pointing to a given gateway IP in given pool
   Optionally kill pf states using those src_nodes.
*/
void pf_kill_src_nodes_to(string &pool, string &ip, bool with_states){
	FILE	*fp;
	char	 cmd[1024];
	int	 ret;

	if (with_states) {
		log_txt(MSG_TYPE_PFCTL, "%s %s - killing src_nodes and states to node with RST", pool.c_str(), ip.c_str());
		snprintf(cmd, sizeof(cmd), "pfctl -q -K table -K '%s' -K dsthost -K '%s' -K kill -K rststates", pool.c_str(), ip.c_str());
	} else {
		log_txt(MSG_TYPE_PFCTL, "%s %s - killing src_nodes to node", pool.c_str(), ip.c_str());
		snprintf(cmd, sizeof(cmd), "pfctl -q -K table -K '%s' -K dsthost -K '%s'", pool.c_str(), ip.c_str());
	}

	if (verbose_pfctl)
		log_txt(MSG_TYPE_PFCTL, "command: %s", cmd);

	if(!pf_action)
		return;

	fp = popen(cmd, "r");
	if(fp == NULL){
		log_txt(MSG_TYPE_PFCTL, "cannot spawn pfctl process to kill src_nodes to '%s'", ip.c_str());
		return;
	}

	ret = pclose(fp);

	if(ret == -1 || ret != 0){
		log_txt(MSG_TYPE_PFCTL, "pf_kill_src_nodes_to('%s'): returned bad status (%d)", ip.c_str(), ret);
	}

}


/*
   Check if an IP address is in the given table.
*/
int pf_is_in_table(string &table, string &ip){
	FILE	*fp;
	char	 cmd[1024];
	int	 ret;

	/* Do not skip this function even if pf_action is false, this one is "passive". */

	snprintf(cmd, sizeof(cmd), "pfctl -q -t '%s' -T show", table.c_str());

	if (verbose_pfctl)
		log_txt(MSG_TYPE_PFCTL, "command: %s", cmd);

	fp = popen(cmd, "r");
	if(fp == NULL){
		log_txt(MSG_TYPE_PFCTL, "cannot spawn pfctl process to show table contents.");
		return 0; // not in table; error
	}

	string output;
	char buffer[1024];
	while (fgets(buffer, 1024, fp) != NULL) {
		output.append(buffer);
	}
	ret = pclose(fp);

	if (ret != 0) {
		log_txt(MSG_TYPE_PFCTL, "pf_is_in_table('%s', '%s'): returned bad status (%d)", table.c_str(), ip.c_str(), ret);
		return 0;
	}

	istringstream istr_output(output);
	string found_ip;
	while (istr_output >> found_ip ) {
		if (found_ip == ip)
			return 1;
	}

	return 0;
}


/*
   Remove src_nodes to all IPs in the given table apart from the specified one.
   States of existing connections will not be killed, only src_nodes.
*/
void pf_table_rebalance(string &table, string &skip_ip) {
	FILE	*fp;
	char	 cmd[1024];
	int	 ret;

	log_txt(MSG_TYPE_PFCTL, "%s %s - rebalancing table", table.c_str(), skip_ip.c_str());

	/* Do not skip this function even if pf_action is false, this one is passive, it calls active ones. */

	snprintf(cmd, sizeof(cmd), "pfctl -q -t '%s' -T show", table.c_str());

	if (verbose_pfctl)
		log_txt(MSG_TYPE_PFCTL, "command: %s", cmd);

	fp = popen(cmd, "r");
	if(fp == NULL){
		log_txt(MSG_TYPE_PFCTL, "cannot spawn pfctl process to show table contents.");
		return;
	}

	string output;
	char buffer[1024];
	while (fgets(buffer, 1024, fp) != NULL) {
		output.append(buffer);
	}
	ret = pclose(fp);

	if (ret != 0) {
		log_txt(MSG_TYPE_PFCTL, "pf_table_rebalance('%s', '%s'): returned bad status (%d)", table.c_str(), skip_ip.c_str(), ret);
		return;
	}

	istringstream istr_output(output);
	string found_ip;
	while (istr_output >> found_ip ) {
		if (verbose_pfctl)
			log_txt(MSG_TYPE_PFCTL, "%s %s - node found in table", table.c_str(), found_ip.c_str());
		if (found_ip != skip_ip)
			pf_kill_src_nodes_to(table, found_ip, false);
	}
}

