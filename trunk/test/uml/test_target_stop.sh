#!/bin/bash
# Copyright 2007 Google Inc.
# Author: Jiaying Zhang <jiayingz@google.com>

# test zumastor stop on target

. config_replication

. setup_replication.sh

echo -n Start replication ...
ssh $source_uml_host "zumastor define target $vol $target_uml_host:$target_port 1"
ssh $target_uml_host "zumastor define source $vol $source_uml_host -p $interval"
echo -e "done.\n"
sleep 30

last_mod_time=$(ssh $source_uml_host stat -c %Y $VOLUMES/$vol/source)
while true; do
	ssh $target_uml_host "/etc/init.d/zumastor stop"
	left=$(ssh $target_uml_host "ls /dev/mapper | grep $vol")
	if [[ $left ]]; then
		echo "zumastor stop did not stop everything"
		ssh $target_uml_host "dmsetup ls"
		ssh $target_uml_host "ps -ef | grep ddsnap"
		exit 1
	fi
	ssh $target_uml_host "/etc/init.d/zumastor start"
	sleep 30
	mod_time=$(ssh $target_uml_host stat -c %Y $VOLUMES/$vol/source/apply)
        [[ $? -ne 0 ]] && mod_time=$(ssh $target_uml_host stat -c %Y $VOLUMES/$vol/source)
       	if [[ $mod_time -eq $last_mod_time ]]; then
               	echo "no progress after network resuming, $mod_time $last_mod_time"
               	ssh $target_uml_host "cat $VOLUMES/$vol/source/apply"
               	ssh $target_uml_host "ps axo stat,comm,pid,pgid,comm,etime,wchan | grep ddsnap"
       	fi
        last_mod_time=$mod_time
done
