#!/bin/bash
# Copyright 2007 Google Inc.
# Author: Jiaying Zhang <jiayingz@google.com>

# keep running full volume replication between source and target

. config_replication

. setup_replication.sh

echo -n Start full-volume replication test...
ssh $source_uml_host "zumastor define target $vol $target_uml_host:$target_port 1 -t fullvolume"
ssh $target_uml_host "zumastor define source $vol $source_uml_host -p $interval"
echo -e "done.\n"
sleep 30

echo Monitoring replication progress...
while true; do
	echo -n "source send  "
	ssh $source_uml_host "cat $VOLUMES/$vol/targets/$target_uml_host/send"
	echo -n "target apply "
	ssh $target_uml_host "cat $VOLUMES/$vol/source/apply"
	sleep 5
done
