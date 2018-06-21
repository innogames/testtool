#!/bin/sh
#
# Testtool - Tests - More Backup Load Balancing Node Operations
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

TABLE2="kajetan_test_lb2"
TABLE2_HOST1="10.7.0.43"
TABLE2_HOST2="10.7.0.44"

# Flush all tables and hosts' firewalls
flush_all

# Write the configuration file
cat > $CONFIG_FILE << EOF
pool $TABLE1 3 2 $TABLE2
	node $TABLE1_HOST1
		healthcheck http 80 1 2 /:200
	node $TABLE1_HOST2
		healthcheck http 80 1 2 /:200

pool $TABLE2 3
	node $TABLE2_HOST1
		healthcheck http 80 1 2 /:200
	node $TABLE2_HOST2
		healthcheck http 80 1 2 /:200

EOF

stage "add 2 nodes to master pool and 2 nodes to backup pool" "4 hosts in respective pools."
launch_testtool
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 || exit_fail
require_pf_table_waitfor_equal $TABLE2 $TABLE2_HOST1 $TABLE2_HOST2 || exit_fail

stage "kill one master node" "2 backup nodes in master pool, 2 backup nodes in backup pool"
ssh $TABLE1_HOST1 `firewall add http reject`
require_pf_table_waitfor_equal $TABLE1 $TABLE2_HOST1 $TABLE2_HOST2 || exit_fail
require_pf_table_waitfor_equal $TABLE2 $TABLE2_HOST1 $TABLE2_HOST2 || exit_fail

stage "kill 2 backup nodes" "no nodes in any pool"
ssh $TABLE2_HOST1 `firewall add http reject`
ssh $TABLE2_HOST2 `firewall add http reject`
require_pf_table_waitfor_equal $TABLE1 || exit_fail
require_pf_table_waitfor_equal $TABLE2 || exit_fail

stage "restore failed nodes of master pool" "master nodes in master pool, no nodes in backup pool"
ssh $TABLE1_HOST1 `firewall del http reject`
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 || exit_fail
require_pf_table_waitfor_equal $TABLE2 || exit_fail

stage "restore failed nodes of backup pool" "master nodes in master pool, backup nodes in backup pool"
ssh $TABLE2_HOST1 `firewall del http reject`
ssh $TABLE2_HOST2 `firewall del http reject`
require_pf_table_waitfor_equal $TABLE1 $TABLE1_HOST1 $TABLE1_HOST2 || exit_fail
require_pf_table_waitfor_equal $TABLE2 $TABLE2_HOST1 $TABLE2_HOST2 || exit_fail

exit_ok
