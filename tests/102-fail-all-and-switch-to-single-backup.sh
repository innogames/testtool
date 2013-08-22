#!/bin/sh

if [ -f ./testlib.sh ]; then
	. ./testlib.sh
else
	echo launch me from main source directory
	exit 1
fi

# Tables and hosts to be used
TABLE1="kajetan_test_lb1"
TABLE1_HOST1="10.7.0.41"
TABLE1_HOST2="10.7.0.42"
TABLE1_HOST3="10.7.0.43"

TABLE2="kajetan_test_lb2"
TABLE2_HOST1="10.7.0.44"

# Flush all tables and hosts' firewalls
flush_all

# Write the configuration file
cat > $CONFFILE << EOF
pool $TABLE1 3 1 $TABLE2
	node $TABLE1_HOST1
		healthcheck http 80 1 2 /:200
	node $TABLE1_HOST2
		healthcheck http 80 1 2 /:200
	node $TABLE1_HOST3
		healthcheck http 80 1 2 /:200


pool $TABLE2 3
	node $TABLE2_HOST1
		healthcheck http 80 1 2 /:200

EOF

stage "adding 3 hosts to master pool and 1 to backup pool" "4 hosts in respective pools"
launch_testtool
require_pf_table_equals $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 $TABLE1_HOST3 || exit_fail
require_pf_table_equals $TABLE2 $TABLE2_HOST1 || exit_fail
stage_end

stage "killing two nodes" "single master node in pool"
ssh $TABLE1_HOST1 `firewall add http reject`
ssh $TABLE1_HOST2 `firewall add http reject`
require_pf_table_equals $TABLE1 $TABLE1_HOST3 || exit_fail
require_pf_table_equals $TABLE2 $TABLE2_HOST1 || exit_fail
stage_end


stage "killing the last node" "only backup node in pool"
ssh $TABLE1_HOST3 `firewall add http reject`
require_pf_table_equals $TABLE1 $TABLE2_HOST1 || exit_fail
require_pf_table_equals $TABLE2 $TABLE2_HOST1 || exit_fail
stage_end

exit_ok

