#!/bin/sh

. ./testlib.sh

if [ "x$1" == "x-v" ]; then
	export VERBOSE="yes"
fi

export ALL_TESTS=0
export OK_TESTS=0

# Kill original testtool, should there be any
TT_SCCOMM="screen -dmS testtool -s /root/lb/testtool.sh"
TT_WDCOMM="/bin/sh /root/lb/testtool.sh"

TT_SCPID=`pgrep -f "^${TT_SCCOMM}$"`
TT_WDPID=`pgrep -f "^${TT_WDCOMM}$"`
TT_TTPID=`pgrep -f '^/root(/lb)?/testtool$'`

[ "$TT_SCPID" ] && kill -9 $TT_SCPID
[ "$TT_WDPID" ] && kill -9 $TT_WDPID
[ "$TT_TTPID" ] && kill $TT_TTPID

for i in tests/*sh; do
	ALL_TESTS=$(($ALL_TESTS+1))
	echo -e "${CCYAN}Running $i ${NOCOL}"
	$i
	if [ $? -eq 0 ]; then
		OK_TESTS=$(($OK_TESTS+1))
	fi
done

echo Final verdict: $OK_TESTS/$ALL_TESTS tests succeeded

