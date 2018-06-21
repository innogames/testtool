#!/bin/sh
#
# Testtool - Tests - Ping Health Check
#
# Copyright (c) 2018 InnoGames GmbH
#

if [ -f ./testlib.sh ]; then
	. ./testlib.sh
else
	echo launch me from main source directory
	exit 1
fi

# Tables and nodes to be used
TABLE1="kajetan_test_lb1"
TABLE1_HOST1="10.7.0.41"
TABLE1_HOST2="10.7.0.42"
TABLE1_HOST3="10.7.0.43"

# Flush all tables and nodes' firewalls
flush_all

# Write the configuration file
cat > $CONFIG_FILE << EOF
pool $TABLE1 3
	node $TABLE1_HOST1
		healthcheck dns 53 1 2 control.innogames.net
	node $TABLE1_HOST2
		healthcheck dns 53 1 2 control.innogames.net
	node $TABLE1_HOST3
		healthcheck dns 53 1 2 control.innogames.net
EOF

stage "add 3 nodes to pool" "3 nodes in pool"
launch_testtool
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 $TABLE1_HOST3 || exit_fail
stage_end

stage "fail a node with reject" "2 nodes in pool"
ssh $TABLE1_HOST2 `firewall add dns reject`
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1               $TABLE1_HOST3 || exit_fail
stage_end

stage "restore the node" "3 nodes in pool"
ssh $TABLE1_HOST2 `firewall del dns reject`
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 $TABLE1_HOST3 || exit_fail
stage_end

stage "fail a node with timeout" "2 nodes in pool"
ssh $TABLE1_HOST2 `firewall add dns drop`
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1               $TABLE1_HOST3 || exit_fail
stage_end

stage "restore the node" "3 nodes in pool"
ssh $TABLE1_HOST2 `firewall del dns drop`
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 $TABLE1_HOST3 || exit_fail
stage_end

exit_ok
