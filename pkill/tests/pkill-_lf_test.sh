#!/bin/sh
# $FreeBSD$

base=`basename $0`

echo "1..2"

#ifdef __APPLE__
fails=0
#endif
name="pkill -LF <pidfile>"
pidfile=$(pwd)/pidfile.txt
sleep=$(pwd)/sleep.txt
ln -sf /bin/sleep $sleep
daemon -p $pidfile $sleep 5
sleep 0.3
pkill -f -L -F $pidfile $sleep
ec=$?
case $ec in
0)
	echo "ok 1 - $name"
	;;
*)
	echo "not ok 1 - $name"
#ifdef __APPLE__
	fails=$((fails + 1))
#endif
	;;
esac

# Be sure we cannot kill process which pidfile is not locked.
$sleep 5 &
sleep 0.3
chpid=$!
echo $chpid > $pidfile
pkill -f -L -F $pidfile $sleep 2>/dev/null
ec=$?
case $ec in
0)
	echo "not ok 2 - $name"
#ifdef __APPLE__
	fails=$((fails + 1))
#endif
	;;
*)
	echo "ok 2 - $name"
	;;
esac

kill "$chpid"
rm -f $pidfile
rm -f $sleep
#ifdef __APPLE__
exit $fails
#endif
