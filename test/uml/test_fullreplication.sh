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

echo Monitoring replication progress...
count=0
TOTAL=10000
while [[ $count -lt $TOTAL ]]; do
	echo -n "source send  "
	ssh $source_uml_host "cat $VOLUMES/$vol/targets/$target_uml_host/send" || { echo FAIL; exit 1; }
	echo -n "target apply "
	ssh $target_uml_host "cat $VOLUMES/$vol/source/apply" || { echo FAIL; exit 1; }
	sleep 5
	count=$(( count+1 ))
done

ssh $source_uml_host "zumastor forget volume $vol" || { echo FAIL; exit 1; }
ssh $target_uml_host "zumastor forget volume $vol" || { echo FAIL; exit 1; }

ssh $source_uml_host "halt" || { echo FAIL; exit 1; }
ssh $target_uml_host "halt" || { echo FAIL; exit 1; }

echo PASS
