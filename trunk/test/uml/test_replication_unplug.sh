#!/bin/bash
# Copyright 2007 Google Inc.
# Author: Jiaying Zhang <jiayingz@google.com>

# test replication resuming after network goes down and up

. config_replication

. setup_replication.sh

echo -n Start replication ...
ssh $source_uml_host "zumastor define target $vol $target_uml_host:$target_port 1"
ssh $target_uml_host "zumastor define source $vol $source_uml_host -p $interval"
echo -e "done.\n"
sleep 30

up_time=60
DOWN_TIME="10 20 30 40 50 60 120 180 300 600 900 1200 1800 2400"

last_mod_time=$(ssh $source_uml_host stat -c %Y $VOLUMES/$vol/targets/$target_uml_host)
for down_time in $DOWN_TIME; do
        while true; do
        	sleep $up_time
		mod_time=$(ssh $source_uml_host stat -c %Y $VOLUMES/$vol/targets/$target_uml_host/send)
        	[[ $? -ne 0 ]] && mod_time=$(ssh $source_uml_host stat -c %Y $VOLUMES/$vol/targets/$target_uml_host)
        	if [[ $mod_time -eq $last_mod_time ]]; then
                	echo "no progress after network resuming, $mod_time $last_mod_time"
                	ssh $source_uml_host "cat $VOLUMES/$vol/targets/$target_uml_host/send"
                	ssh $source_uml_host "ps axo stat,comm,pid,pgid,comm,etime,wchan | grep ssh"
        	else
                	break
        	fi
        done
        last_mod_time=$mod_time
        echo unplug $down_time seconds
	echo 0 > /proc/sys/net/ipv4/ip_forward
        sleep $down_time
	echo resume
	echo 1 > /proc/sys/net/ipv4/ip_forward
done

ssh $source_uml_host "zumastor forget volume $vol"
ssh $target_uml_host "zumastor forget volume $vol"

ssh $source_uml_host "halt"
ssh $target_uml_host "halt"
