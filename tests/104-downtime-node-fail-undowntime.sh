#!/bin/sh
#
# Testtool - Tests - Downtiming
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
pool $TABLE1 3 1 $TABLE2
	node $TABLE1_HOST1
		healthcheck http 80 1 2 /:200
	node $TABLE1_HOST2
		healthcheck http 80 1 2 /:200
	node $TABLE1_HOST3
		healthcheck http 80 1 2 /:200

EOF

stage "add 3 hosts to pool" "3 hosts in the pool"
rm -f $DOWNTIMES_FILE
launch_testtool
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 $TABLE1_HOST3 || exit_fail
stage_end

stage "downtime 2nd node" "nodes 1 and 3 in pool"
cat > $DOWNTIMES_FILE << EOF
$TABLE1 $TABLE1_HOST2
EOF
reload_testtool_downtimes
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST3 || exit_fail
stage_end

stage "fail the 2nd node" "keep only nodes 1 and 3 in pool"
ssh $TABLE1_HOST2 `firewall add http drop`
require_pf_table_stays_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST3 || exit_fail
stage_end

stage "end downtime for 2nd node" "keep only nodes 1 and 3 in pool"
rm -f $DOWNTIMES_FILE
reload_testtool_downtimes
require_pf_table_stays_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST3 || exit_fail
stage_end

stage "restore 2nd node" "all 3 nodes in pool"
ssh $TABLE1_HOST2 `firewall del http drop`
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 $TABLE1_HOST3 || exit_fail
stage_end

exit_ok
