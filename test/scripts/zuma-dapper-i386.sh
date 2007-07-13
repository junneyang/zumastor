#!/bin/sh

# Build an image with current or provided zumastor debs installed, booted,
# and ready to immediately run single-node tests.
# Inherits from the generic dapper template.

# $Id$
# Copyright 2007 Google Inc.
# Author: Drake Diedrich <dld@google.com>
# License: GPLv2

set -e

if [ "x$MACFILE" = "x" -o "x$MACADDR" = "x" -o "x$IFACE" = "x" \
     -o "x$IPADDR" = "x" ] ; then
  echo "Run this script under tunbr"
  exit 1
fi

# defaults, overridden by /etc/default/testenv if it exists
# diskimgdir should be local for reasonable performance
size=2G
diskimgdir=${HOME}/.testenv
tftpdir=/tftpboot
qemu_i386=qemu  # could be kvm, kqemu version, etc.  Must be 0.9.0 to net boot.

[ -x /etc/default/testenv ] && . /etc/default/testenv



templateimg=${diskimgdir}/template/dapper-i386.img

if [ ! -f ${templateimg} ] ; then

  echo "No template image ${templateimg} exists yet."
  echo "Run tunbr dapper-i386.sh first."
  exit 1
fi

zumaimg=${diskimgdir}/zuma/dapper-i386.img

if [ -f ${zumaimg} ] ; then
  echo Zuma/dapper image already exists, remove if you wish to build a new one
  echo rm ${zumaimg}
  exit 2
fi

if [ ! -d ${diskimgdir}/zuma ] ; then
  mkdir -p ${diskimgdir}/zuma
fi

qemu-img create  -b ${templateimg} -f qcow2 ${zumaimg}

${qemu_i386} \
  -net nic,macaddr=${MACADDR},model=ne2k_pci \
  -net tap,ifname=${IFACE},script=no \
  -boot c -hda ${zumaimg} -no-reboot &
  
# wait for ssh to work
while ! ssh -o StrictHostKeyChecking=no root@${IPADDR} hostname 2>/dev/null
do
  echo -n .
  sleep 10
done

date

# copy the debs that were built in the build directory
# onto the new zuma template instance
scp ../../build/*.deb root@${IPADDR}:

# install the copied debs in the correct order
ssh root@${IPADDR} <<EOF
aptitude install -y tree
dpkg -i kernel-image*.deb
dpkg -i ddsnap*.deb
dpkg -i zumastor*.deb
rm *.deb
apt-get clean
EOF

# halt the new image, and wait for qemu to exit
ssh root@${IPADDR} halt
wait

if false
then

  tmpdir=`mktemp -d`

  mkfifo ${tmpdir}/monitor

  ${qemu_i386} \
    -net nic,macaddr=${MACADDR} -net tap,ifname=${IFACE},script=no \
    -boot c -hda ${diskimg} -no-reboot -monitor pipe:${tmpdir}/monitor &

  while ! ssh -o StrictHostKeyChecking=no root@${IPADDR} hostname 2>/dev/null
  do
    echo -n .
    sleep 10
  done

  cat <<EOF > ${tmpdir}/monitor
stop
savevm booted
quit
EOF

  wait

  rm -rf ${tmpdir}
fi

echo "Instance shut down, removing ssh hostkey"
sed -i /^${IPADDR}\ .*\$/d ~/.ssh/known_hosts
