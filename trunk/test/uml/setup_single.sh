#!/bin/bash

. config_uml
. config_single

# build linux uml kernel
[[ -e linux-${KERNEL_VERSION} ]] || . build_uml.sh

# setup source and target file system and network
initial=1
[[ -e $uml_fs ]] && initial=0 || . build_fs.sh $uml_fs
[[ $initial -eq 1 ]] && . setup_network.sh $uml_ip $uml_host $uml_fs

# load source and target
echo -n Bring up uml...
cd linux-${KERNEL_VERSION}
screen -d -m ./linux ubda=../$uml_fs ubdb=$ubdb_dev ubdc=$ubdc_dev eth0=tuntap,,,$host_tap_ip mem=64M
cd ..
echo -e "done.\n"
sleep 30

echo -n Setting up volume...
ssh $uml_host "/etc/init.d/zumastor start"
ssh $uml_host "zumastor forget volume $vol"
ssh $uml_host "echo y | zumastor define volume -i $vol /dev/ubdb /dev/ubdc"
ssh $uml_host "mkfs.ext3 /dev/mapper/$vol"
echo -e "done.\n"
