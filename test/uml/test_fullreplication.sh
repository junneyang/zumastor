#!/bin/bash
# Copyright 2007 Google Inc.
# Author: Jiaying Zhang <jiayingz@google.com>

# keep running full volume replication between source and target

. config_replication

. setup_replication.sh || { echo UNRESOLVED; exit 1; }

echo -n Start full-volume replication test...
ssh $source_uml_host "zumastor define target $vol $target_uml_host:$target_port -p 1 -t fullvolume" || { echo FAIL; exit 1; }
ssh $target_uml_host "zumastor define source $vol $source_uml_host -p $interval" || { echo FAIL; exit 1; }
echo -e "done.\n"
sleep 30

# test program that keeps allocating/de-allocating memory
gcc -o cyclic-anon cyclic-anon.c
scp cyclic-anon $target_uml_host:/root/cyclic-anon
ssh $target_uml_host "/root/cyclic-anon 64M" &

# rasing dirty_ratio to trigger problem faster
ssh $target_uml_host "echo 50 > /proc/sys/vm/dirty_background_ratio" || { echo FAIL; exit 1; }
ssh $target_uml_host "echo 95 > /proc/sys/vm/dirty_ratio" || { echo FAIL; exit 1; }

echo Monitoring replication progress...
count=0
TOTAL=10000
while [[ $count -lt $TOTAL ]]; do
	echo -n "source send  "
	ssh $source_uml_host "cat $VOLUMES/$vol/targets/$target_uml_host/send"
	ssh $target_uml_host "echo" > /dev/null || { echo "downstream not responding, lockup?"; echo FAIL; exit 1; }
	echo -n "target apply "
	ssh $target_uml_host "cat $VOLUMES/$vol/source/apply"
	sleep 5
	count=$(( count+1 ))
done

ssh $source_uml_host "zumastor forget volume $vol" || { echo FAIL; exit 1; }
ssh $target_uml_host "zumastor forget volume $vol" || { echo FAIL; exit 1; }

ssh $source_uml_host "halt" || { echo FAIL; exit 1; }
ssh $target_uml_host "halt" || { echo FAIL; exit 1; }

echo PASS
