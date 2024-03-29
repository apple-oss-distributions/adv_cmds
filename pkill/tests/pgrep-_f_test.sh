#!/bin/sh
# $FreeBSD$

base=`basename $0`

echo "1..1"

#ifdef __APPLE__
fails=0
#endif
name="pgrep -F <pidfile>"
pidfile=$(pwd)/pidfile.txt
sleep=$(pwd)/sleep.txt
ln -sf /bin/sleep $sleep
$sleep 5 &
sleep 0.3
chpid=$!
echo $chpid > $pidfile
pid=`pgrep -f -F $pidfile $sleep`
if [ "$pid" = "$chpid" ]; then
	echo "ok - $name"
else
#ifdef __APPLE__
	fails=$((fails + 1))
#endif
	echo "not ok - $name"
fi
kill "$chpid"
rm -f $pidfile
rm -f $sleep
#ifdef __APPLE__
exit $fails
#enif
