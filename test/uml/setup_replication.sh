#!/bin/bash
# Copyright 2007 Google Inc.
# Author: Jiaying Zhang <jiayingz@google.com>

. config_uml
. config_replication

# build linux uml kernel
[[ -e linux-${KERNEL_VERSION} ]] || . build_uml.sh

# setup source and target file system and network
# if both source_fs and target_fs exists, do nothing assuming everything is setup
# if only source_fs exists (e.g., a copy from a previously configured image), re-config network and copy it to target_fs
# if source_fs does not exist, download the Debian image and build everything needed
initial=1
[[ -e $source_uml_fs ]] && [[ -e $target_uml_fs ]] && initial=0
[[ -e $source_uml_fs ]] || . build_fs.sh $source_uml_fs
[[ $initial -eq 1 ]] && . setup_network.sh $source_uml_ip $source_uml_host $source_uml_fs
[[ -e $target_uml_fs ]] || cp $source_uml_fs $target_uml_fs
[[ $initial -eq 1 ]] && . setup_network.sh $target_uml_ip $target_uml_host $target_uml_fs

# load source and target
echo -n Bring up source uml...
cd linux-${KERNEL_VERSION}
screen -d -m ./linux ubda=../$source_uml_fs ubdb=$source_ubdb_dev ubdc=$source_ubdc_dev eth0=tuntap,,,$host_tap_ip mem=64M
cd ..
echo -e "done.\n"
sleep 30

echo -n Bring up target uml...
cd linux-${KERNEL_VERSION}
screen -d -m ./linux ubda=../$target_uml_fs ubdb=$target_ubdb_dev ubdc=$target_ubdc_dev eth0=tuntap,,,$host_tap_ip mem=64M
cd ..
echo -e "done.\n"
sleep 30

# set up ssh keys for source and target umls to access each other
if [[ $initial ]]; then
	echo -n Setting up ssh-keygen...
	ssh $source_uml_host "rm /root/.ssh/id_dsa; rm /root/.ssh/id_dsa.pub"
	ssh $source_uml_host "ssh-keygen -t dsa -f /root/.ssh/id_dsa -P ''"
	scp $source_uml_host:/root/.ssh/id_dsa.pub /tmp/$source_uml_host.pub
	scp /tmp/$source_uml_host.pub $target_uml_host:/root/.ssh/$source_uml_host.pub
	rm -f /tmp/$source_uml_host.pub
	ssh $target_uml_host "cat /root/.ssh/$source_uml_host.pub >> /root/.ssh/authorized_keys"
	ssh $source_uml_host "echo $target_uml_ip $target_uml_host > /etc/hosts"

	ssh $target_uml_host "rm /root/.ssh/id_dsa; rm /root/.ssh/id_dsa.pub"
	ssh $target_uml_host "ssh-keygen -t dsa -f /root/.ssh/id_dsa -P ''"
	scp $target_uml_host:/root/.ssh/id_dsa.pub /tmp/$target_uml_host.pub
	scp /tmp/$target_uml_host.pub $source_uml_host:/root/.ssh/$target_uml_host.pub
	rm -f /tmp/$target_uml_host.pub
	ssh $source_uml_host "cat /root/.ssh/$target_uml_host.pub >> /root/.ssh/authorized_keys"
	ssh $target_uml_host "echo $source_uml_ip $source_uml_host > /etc/hosts"
	echo -e "done.\n"
fi

# set up source and target volumes according to the configuratiosn in config_replication
echo -n Setting up source volume...
ssh $source_uml_host "/etc/init.d/zumastor start"
ssh $source_uml_host "zumastor forget volume $vol"
ssh $source_uml_host "echo y | zumastor define volume -i $vol /dev/ubdb /dev/ubdc"
ssh $source_uml_host "mkfs.ext3 /dev/mapper/$vol"
ssh $source_uml_host "zumastor define master $vol -h $hourly_snapnum -d 7"
echo -e "done.\n"

echo -n Setting up target volume...
ssh $target_uml_host "/etc/init.d/zumastor start"
ssh $target_uml_host "zumastor forget volume $vol"
ssh $target_uml_host "echo y | zumastor define volume -i $vol /dev/ubdb /dev/ubdc"
echo -e "done.\n"

