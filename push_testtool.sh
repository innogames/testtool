#!/bin/sh

if [ -z "$1" ]; then
	echo "Give me HWLB's hostname!"
	exit 1
fi

ssh root@$1 "pkill -f '^/root/testtool$'"
scp ./testtool root@$1:/root

