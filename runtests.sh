#!/bin/sh

. ./testlib.sh

if [ "x$1" == "x-v" ]; then
	export VERBOSE="yes"
fi

export ALL_TESTS=0
export OK_TESTS=0

killall testtool 2> /dev/null

for i in tests/*sh; do
	ALL_TESTS=$(($ALL_TESTS+1))
	echo -e "${CCYAN}Running $i ${NOCOL}"
	$i
	if [ $? -eq 0 ]; then
		OK_TESTS=$(($OK_TESTS+1))
	fi
done

echo Final verdict: $OK_TESTS/$ALL_TESTS tests succeeded

