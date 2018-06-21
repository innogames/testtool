#!/bin/sh
#
# Testtool - Tests - Load Balancing Node Operations
#
# Copyright (c) 2018 InnoGames GmbH
#

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

# Flush all tables and hosts' firewalls
flush_all

# Write the configuration file
cat > $CONFIG_FILE << EOF
pool $TABLE1 3
	node $TABLE1_HOST1
		healthcheck http 80 1 2 /:200
	node $TABLE1_HOST2
		healthcheck http 80 1 2 /:200
	node $TABLE1_HOST3
		healthcheck http 80 1 2 /:200
EOF

stage "add 3 nodes to pool" "3 hosts in pool"
launch_testtool
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 $TABLE1_HOST3 || exit_fail
stage_end

stage "fail the 1st node" "2 nodes left in pool"
ssh $TABLE1_HOST1 `firewall add http reject`
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST2 $TABLE1_HOST3 || exit_fail
stage_end

stage "fail the 2nd node" "1 node left in pool"
ssh $TABLE1_HOST2 `firewall add http reject`
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST3 || exit_fail
stage_end

stage "fail the 3rd node" "pool is empty"
ssh $TABLE1_HOST3 `firewall add http reject`
require_pf_table_waitfor_equal $TABLE1 || exit_fail
stage_end

stage "restore 3 nodes" "all 3 nodes back in pool"
ssh $TABLE1_HOST1 `firewall del http reject`
ssh $TABLE1_HOST2 `firewall del http reject`
ssh $TABLE1_HOST3 `firewall del http reject`
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 $TABLE1_HOST3 || exit_fail
stage_end

exit_ok
