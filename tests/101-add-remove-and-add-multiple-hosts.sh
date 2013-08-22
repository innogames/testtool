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

# Flush all tables and hosts' firewalls
flush_all

# Write the configuration file
cat > $CONFFILE << EOF
pool $TABLE1 3
	node $TABLE1_HOST1
		healthcheck http 80 1 2 /:200
	node $TABLE1_HOST2
		healthcheck http 80 1 2 /:200
	node $TABLE1_HOST3
		healthcheck http 80 1 2 /:200
EOF

stage "add 3 hosts" "3 hosts in pool"
launch_testtool
require_pf_table_equals $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 $TABLE1_HOST3 || exit_fail
stage_end

stage "removing 1st host" "2 hosts left in pool"
ssh $TABLE1_HOST1 `firewall add http reject`
require_pf_table_equals $TABLE1 $TABLE1_HOST2 $TABLE1_HOST3 || exit_fail
stage_end

stage "removing 2nd host" "1 host left in pool"
ssh $TABLE1_HOST2 `firewall add http reject`
require_pf_table_equals $TABLE1 $TABLE1_HOST3 || exit_fail
stage_end

stage "removing 3rd host" "pool is empty"
ssh $TABLE1_HOST3 `firewall add http reject`
require_pf_table_equals $TABLE1 || exit_fail
stage_end

stage "adding 3 nodes back to pool" "all 3 nodes are in pool"
ssh $TABLE1_HOST1 `firewall del http reject`
ssh $TABLE1_HOST2 `firewall del http reject`
ssh $TABLE1_HOST3 `firewall del http reject`
require_pf_table_equals $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 $TABLE1_HOST3 || exit_fail
stage_end


exit_ok

