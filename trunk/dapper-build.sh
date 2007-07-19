#!/bin/sh
#
# $Id$
#
# Launch a dapper-i386 snapshot
# create a build user in the snapshot
# copy the zumastor source to ~build
# run buildcurrent.sh as user build in the testenv instance
# pull the debs out of the instance
# shut down the instance
#
# Copy this to a directory outside the working directory, and launch
# with the top level of the working directory
#
# Copyright 2007 Google Inc.
# Author: Drake Diedrich <dld@google.com>
# License: GPLv2

# set -e

KERNEL_VERSION=`awk '/^2\.6\.[0-9]+(\.[0-9]+)?$/ { print $1; }' KernelVersion`
if [ "x$KERNEL_VERSION" = "x" ] ; then
  echo "Suspect KernelVersion file"
  exit 1
fi
VERSION=`awk '/[0-9]+\.[0-9]+(\.[0-9]+)?$/ { print $1; }' Version`
if [ "x$VERSION" = "x" ] ; then
  echo "Suspect Version file"
  exit 1
fi
SVNREV=`svn info | grep ^Revision:  | cut -d\  -f2`
ARCH=`dpkg --print-architecture`


if [ "x$MACFILE" = "x" -o "x$MACADDR" = "x" -o "x$IFACE" = "x" ] ; then
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



diskimg=${diskimgdir}/template/dapper-i386.img

if [ ! -f ${diskimg} ] ; then
  echo "No $diskimg available.  Run dapper-i386.sh first."
  exit 2
fi


${qemu_i386} -snapshot -m 512 \
  -net nic,macaddr=${MACADDR} -net tap,ifname=${IFACE},script=no \
  -boot c -hda ${diskimg} -no-reboot &

while ! ssh -o StrictHostKeyChecking=no root@${IPADDR} hostname >/dev/null 2>&1
do
  echo -n .
  sleep 10
done

if [ ! -d build ] ; then
  mkdir build
  pushd build
  wget -c http://www.kernel.org/pub/linux/kernel/v2.6/linux-${KERNEL_VERSION}.tar.bz2
  popd
fi

ssh root@${IPADDR} <<EOF
useradd build
mkdir -p ~build/.ssh ~build/zumastor/build
cp ~/.ssh/authorized_keys ~build/.ssh/
chown -R build ~build
EOF

tar cf - --exclude build * | ssh build@${IPADDR} tar xf - -C zumastor
scp build/linux-${KERNEL_VERSION}.tar.bz2 build@${IPADDR}:zumastor/build/

ssh root@${IPADDR} <<EOF 
cd ~build/zumastor
./builddepends.sh
EOF

ssh build@${IPADDR} <<EOF
cd zumastor
echo $SVNREV >SVNREV
./buildcurrent.sh test/config/qemu-config
EOF

BUILDSRC="build@${IPADDR}:zumastor/build"
DEBVERS="${VERSION}-r${SVNREV}_${ARCH}"
KVERS="${KERNEL_VERSION}-zumastor-r${SVNREV}_1.0"

for f in \
    ${BUILDSRC}/ddsnap_${DEBVERS}_${ARCH}.changes \
    ${BUILDSRC}/ddsnap_${DEBVERS}_${ARCH}.deb \
    ${BUILDSRC}/ddsnap_${DEBVERS}.dsc \
    ${BUILDSRC}/ddsnap_${DEBVERS}.tar.gz \
    ${BUILDSRC}/zumastor_${DEBVERS}_{$ARCH}.changes \
    ${BUILDSRC}/zumastor_${DEBVERS}_{$ARCH}.deb \
    ${BUILDSRC}/zumastor_${DEBVERS}.dsc \
    ${BUILDSRC}/zumastor_${DEBVERS}.tar.gz \
    ${BUILDSRC}/kernel-headers-${KVERS}_${ARCH}.deb \
    ${BUILDSRC}/kernel-image-${KVERS}_${ARCH}.deb
do
  scp $f build/
done

ssh root@${IPADDR} halt

wait

