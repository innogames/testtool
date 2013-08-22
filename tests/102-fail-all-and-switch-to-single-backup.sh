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
cat > $CONFIG_FILE << EOF
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

stage "add 3 nodes to master pool and 1 node to backup pool" "4 nodes in respective pools"
launch_testtool
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 $TABLE1_HOST3 || exit_fail
require_pf_table_waitfor_equal $TABLE2 $TABLE2_HOST1 || exit_fail
stage_end

stage "kill 2 nodes in master pool" "single master node in pool"
ssh $TABLE1_HOST1 `firewall add http reject`
ssh $TABLE1_HOST2 `firewall add http reject`
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST3 || exit_fail
require_pf_table_waitfor_equal $TABLE2 $TABLE2_HOST1 || exit_fail
stage_end

stage "kill the last node in master pool" "only backup node in pool"
ssh $TABLE1_HOST3 `firewall add http reject`
require_pf_table_waitfor_equal $TABLE1 $TABLE2_HOST1 || exit_fail
require_pf_table_waitfor_equal $TABLE2 $TABLE2_HOST1 || exit_fail
stage_end

exit_ok
