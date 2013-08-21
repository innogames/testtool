#!/bin/sh

CONFFILE="/tmp/testtool.test.conf"

CRED='\e[0;31m'
CGREEN='\e[0;32m'
CCYAN='\e[0;36m'
NOCOL='\e[0m'

firewall () {
	# $1 = add, del, flush
	# $2 = protocol (ping, dns, http, https)
	# $3 = drop or reject

	CMD="iptables"
		
	case "$1" in
		add)
			CMD="$CMD -A INPUT"
			;;
		del)
			CMD="$CMD -D INPUT"
			;;
		flush)
			CMD="$CMD -F INPUT"
			;;
	esac

	case "$2" in
		ping)
			CMD="$CMD -p icmp --icmp echo-reques"
			;;
		dns)
			CMD="$CMD -p udp --dport 53"
			;;
		http)
			CMD="$CMD -p tcp --dport 80"
			;;
		https)
			CMD="$CMD -p tcp --dport 443"
			;;
	esac

	case "$3" in
		drop)
			CMD="$CMD -j DROP"
			;;
		reject)
			CMD="$CMD -j REJECT"
			;;
	esac

	echo $CMD
}


pf_table_show () {
	pfctl -q -t $1 -T show
}

require_ip_in_pf_table () {
	pf_table_show $1 | grep -q $2
}


require_ip_not_in_pf_table () {
	pf_table_show $1 | grep -q $2
}

require_pf_table_equals () {
	table=$1
	shift
	HOSTS=$@

	cnt=0
	while [ $cnt -le 20 ]; do
		sleep 0.25
		cnt=$(($cnt+1))
		TABLE=`pf_table_show $table`

		echo > /tmp/testtooltests_pfctl_table
		for HOST in $TABLE; do
			echo $HOST >> /tmp/testtooltests_pfctl_table
		done

		echo > /tmp/testtooltests_expected_hosts
		for HOST in $HOSTS; do
			echo $HOST >> /tmp/testtooltests_expected_hosts
		done

		sort /tmp/testtooltests_pfctl_table > /tmp/testtooltests_pfctl_table1
		mv /tmp/testtooltests_pfctl_table1 /tmp/testtooltests_pfctl_table
		sort /tmp/testtooltests_expected_hosts > /tmp/testtooltests_expected_hosts1
		mv /tmp/testtooltests_expected_hosts1 /tmp/testtooltests_expected_hosts

		if [ $VERBOSE ]; then
			diff -u /tmp/testtooltests_expected_hosts /tmp/testtooltests_pfctl_table 2>&1 >/tmp/testtooltests_pfctl_tablediff
			ret=$?
		else
			diff -u /tmp/testtooltests_expected_hosts /tmp/testtooltests_pfctl_table 2>&1 >/tmp/testtooltests_pfctl_tablediff
			ret=$?
		fi

		[ $ret -eq 0 ] && return 0
	done

	[ $VERBOSE ] && ( echo "diff for table $table said:" ; cat /tmp/testtooltests_pfctl_tablediff )

	return 1
}


pf_table_flush () {
	pfctl -q -t $1 -T flush
}


flush_all () {
	for table in `set | grep -E 'TABLE[0-9]+=' | cut -d '=' -f 2`; do
		pfctl -q -t $table -T flush
	done

	for host in `set | grep -E 'TABLE[0-9]+_HOST[0-9]+=' | cut -d '=' -f 2`; do
		ssh $host `firewall flush`
		ssh $host /etc/init.d/lighttpd restart > /dev/null
	done
}


exit_fail () {
	kill $TTPID
	wait
	flush_all
	echo -e "${CRED}Failed at: $STAGE_END${NOCOL}"
	exit 1
}


exit_ok () {
	kill $TTPID
	wait
	flush_all
	echo -e "${CGREEN}Success${NOCOL}"
	exit 0
}


stage () {
	STAGE_START="$1"
	STAGE_END="$2"
	if [ $VERBOSE ]; then
		echo -e "${CCYAN}Starting stage:${NOCOL}"
		echo             " * Conditions: $STAGE_START"
		echo             " * Expecting: $STAGE_END"
	fi
}

stage_end () {
	if [ $VERBOSE ]; then
		echo -e "${CGREEN}Stage successful${NOCOL}"
	fi
}



launch_testtool () {
	if [ $VERBOSE ]; then
		./testtool -f $CONFFILE &
	else
		./testtool -f $CONFFILE > /dev/null &
	fi

	export TTPID="$!"
}

