#!/bin/bash
# Copyright 2007 Google Inc.
# Author: Jiaying Zhang <jiayingz@google.com>

# test zumastor stop on target

. config_single

. setup_single.sh || { echo UNRESOLVED; exit 1; }

mount_point=/var/run/zumastor/mount/$vol
RUNPATH=/var/run/zumastor
SERVERS=$RUNPATH/servers
AGENTS=$RUNPATH/agents
server=${SERVERS}/$vol
agent=${AGENTS}/$vol

echo "test origin device cleanup"
TOTAL=1000
count=0
while [[ $count -lt $TOTAL ]]; do
	echo count $count
	ssh $uml_host "mkdir -p $mount_point"
	ssh $uml_host "mount /dev/mapper/$vol $mount_point" || { echo "mount origin device error $?"; echo FAIL; exit 1; }
	(ssh $uml_host "dd if=/dev/zero of=$mount_point/zero" &)
	sleep 10
	ssh $uml_host "pkill -f 'ddsnap agent'"
	ssh $uml_host "/etc/init.d/zumastor restart" || { echo "restart in origin test error $?"; echo FAIL; exit 1; }
	count=$(( count+1 ))
done

echo "test snapshot device cleanup"
ssh $uml_host "ddsnap create $server 0"
size=$(ssh $uml_host "ddsnap status $server --size") || echo FAIL

TOTAL=1000
count=0
while [[ $count -lt $TOTAL ]]; do
	echo count $count
	ssh $uml_host "echo 0 $size ddsnap /dev/ubdc /dev/ubdb $agent 0 | dmsetup create $vol\(0\)"
	ssh $uml_host "mkdir -p $mount_point"
	ssh $uml_host "mount /dev/mapper/$vol\(0\) $mount_point" || { echo "mount snapshot device error $?"; echo FAIL; exit 1; }
	(ssh $uml_host "dd if=/dev/zero of=$mount_point/zero" &)
	sleep 10
	ssh $uml_host "pkill -f 'ddsnap agent'"
	ssh $uml_host "dmsetup suspend $vol\(0\)"
	ssh $uml_host "dmsetup resume $vol\(0\)"
	ssh $uml_host "umount /dev/mapper/$vol\(0\)" || echo "umount snapshot device error $?"
	ssh $uml_host "dmsetup remove $vol\(0\)" || echo "remove snapshot device error $?"
	ssh $uml_host "/etc/init.d/zumastor restart" || { echo "restart in snapshot test error $?"; echo FAIL; exit 1; }
	count=$(( count+1 ))
done

ssh $uml_host "zumastor forget volume $vol" || { echo FAIL; exit 1; }
ssh $uml_host "halt" || { echo FAIL; exit 1; }
echo PASS
