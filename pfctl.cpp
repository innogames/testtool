#include <iostream>
#include <string>
#include <sstream>

#include "msg.h"
#include "pfctl.h"

using namespace std;

extern bool	 pf_action;
extern int	 verbose_pfctl;

/*
   Kill states between given IPs, or from the first given IP (with second one being an empty string).
*/
void pf_states_kill(string &from, string &to){
	FILE	*fp;
	int	 ret;
	char	 cmd[1024];

	if(to.length() == 0)
		snprintf(cmd, sizeof(cmd), "pfctl -q -k '%s'", from.c_str());
	else
		snprintf(cmd, sizeof(cmd), "pfctl -q -k '%s' -k '%s'", from.c_str(), to.c_str());

	if (verbose_pfctl)
		cout << cmd << endl;

	if(!pf_action)
		return;

	fp = popen(cmd, "r");
	if(fp == NULL){
		showError("cannot spawn pfctl process to kill states %s => %s\n", from.c_str(), to.c_str());
		return;
	}
	
	ret = pclose(fp);
	if(ret == -1 || ret != 0){
			
		showWarning("pf_states_kill('%s', '%s'): returned bad status (%d)\n", from.c_str(), to.c_str(), ret);
	}
	
}//end: pf_states_kill


/*
   Add an IP address to the specified table.
*/
void pf_table_add(string &table, string &ip){
	FILE	*fp;
	char	 cmd[1024];
	int	 ret;

	snprintf(cmd, sizeof(cmd), "pfctl -q -t '%s' -T add '%s'", table.c_str(), ip.c_str());
	
	if (verbose_pfctl)
		cout << cmd << endl;

	if(!pf_action)
		return;
	
	fp = popen(cmd, "r");
	if(fp == NULL){
		showError("cannot spawn pfctl process to add ip '%s' to table '%s'\n", ip.c_str(), table.c_str());
		return;
	}
	
	ret = pclose(fp);

	if(ret == -1 || ret != 0){
		showWarning("pf_table_add('%s', '%s'): returned bad status (%d)\n", table.c_str(), ip.c_str(), ret);
	}

}//end: pf_table_add


/*
   Remove an IP address from the specified table.
*/
void pf_table_del(string &table,  string &ip){
	FILE	*fp;
	char	 cmd[1024];
	int	 ret;

	snprintf(cmd, sizeof(cmd), "pfctl -q -t '%s' -T del '%s'", table.c_str(), ip.c_str());

	if (verbose_pfctl)
		cout << cmd << endl;

	if(!pf_action)
		return;

	fp = popen(cmd, "r");
	if(fp == NULL){
		showError("cannot spawn pfctl process to del ip '%s' from table '%s'\n", ip.c_str(), table.c_str());
		return;
	}
	
	ret = pclose(fp);
	
	if(ret == -1 || ret != 0){
		showWarning("pf_table_del('%s', '%s'): returned bad status (%d)\n", table.c_str(), ip.c_str(), ret);
	}
	
}//end: pf_table_del


/*
   Kill src_nodes pointing to a given gateway IP.
   Optionally kill pf states using those src_nodes.
*/
void pf_kill_src_nodes_to(string &ip, bool with_states){
	FILE	*fp;
	char	 cmd[1024];
	int	 ret;

	snprintf(cmd, sizeof(cmd), "pfctl -q -K 0.0.0.0/0 -K '%s' %s", ip.c_str(), with_states?"-c -S":"" );

	if (verbose_pfctl)
		cout << cmd << endl;

	if(!pf_action)
		return;
	
	fp = popen(cmd, "r");
	if(fp == NULL){
		showError("cannot spawn pfctl process to kill src_nodes to '%s'\n", ip.c_str());
		return;
	}
	
	ret = pclose(fp);
	
	if(ret == -1 || ret != 0){
		showWarning("pf_kill_src_nodes_to('%s'): returned bad status (%d)\n", ip.c_str(), ret);
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
		cout << cmd << endl;
	
	fp = popen(cmd, "r");
	if(fp == NULL){
		showError("cannot spawn pfctl process to show table contents.\n");
		return 0; // not in table; error	
	}

	string output;
	char buffer[1024];
	while (fgets(buffer, 1024, fp) != NULL) {
		output.append(buffer);
	}
	ret = pclose(fp);

	if (ret != 0) {
		showWarning("pf_is_in_table('%s', '%s'): returned bad status (%d)\n", table.c_str(), ip.c_str(), ret);
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

	/* Do not skip this function even if pf_action is false, this one is passive, it calls active ones. */

	snprintf(cmd, sizeof(cmd), "pfctl -q -t '%s' -T show", table.c_str());

	if (verbose_pfctl)
		cout << cmd << endl;

	fp = popen(cmd, "r");
	if(fp == NULL){
		showError("cannot spawn pfctl process to show table contents.\n");
		return;
	}

	string output;
	char buffer[1024];
	while (fgets(buffer, 1024, fp) != NULL) {
		output.append(buffer);
	}
	ret = pclose(fp);

	if (ret != 0) {
		showWarning("pf_table_rebalance('%s', '%s'): returned bad status (%d)\n", table.c_str(), skip_ip.c_str(), ret);
		return;
	}

	istringstream istr_output(output);
	string found_ip;
	while (istr_output >> found_ip ) {
		if (verbose_pfctl)
			cout << found_ip << endl;
		if (found_ip != skip_ip)
			pf_kill_src_nodes_to(found_ip, false);
	}
}

