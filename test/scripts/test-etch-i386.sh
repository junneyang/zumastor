#!/bin/sh -x

# Run the etch/i386 image using -snapshot to verify that it works.
# The template should be left unmodified.

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



diskimg=${diskimgdir}/template/etch-i386.img

if [ ! -f ${diskimg} ] ; then

  echo "No template image ${diskimg} exists yet."
  echo "Run tunbr etch-i386.sh first."
  exit 1
fi


${qemu_i386} -snapshot \
  -net nic,macaddr=${MACADDR} \
  -net tap,ifname=${IFACE},script=no \
  -boot c -hda ${diskimg} -no-reboot &

while ! ping -c 1 -w 10 ${IPADDR} 2>/dev/null
do
  echo -n .
done
echo " ping succeeded"

while ! ssh -o StrictHostKeyChecking=no root@${IPADDR} hostname
do
  echo -n .
  sleep 10
done

date

# do what you need to here now
sleep 10

ssh root@${IPADDR} halt

wait

echo "Instance shut down, removing ssh hostkey"
sed -i /^${IPADDR}\ .*\$/d ~/.ssh/known_hosts

