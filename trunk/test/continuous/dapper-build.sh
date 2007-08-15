#!/bin/sh -x
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

# Die if more than four hours pass
( sleep 14400 ; kill $$ ; exit 0 ) & tmoutpid=$!

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
ARCH=i386


if [ "x$MACFILE" = "x" -o "x$MACADDR" = "x" -o "x$IFACE" = "x" ] ; then
  echo "Run this script under tunbr"
  exit 1
fi

SSH='ssh -o StrictHostKeyChecking=no'
SCP='scp -o StrictHostKeyChecking=no'

# defaults, overridden by /etc/default/testenv if it exists
# diskimgdir should be local for reasonable performance
size=10G
diskimgdir=${HOME}/testenv
tftpdir=/tftpboot
qemu_i386=qemu  # could be kvm, kqemu version, etc.  Must be 0.9.0 to net boot.
qemu_threads=1

[ -x /etc/default/testenv ] && . /etc/default/testenv

IMAGE=dapper-i386
IMAGEDIR=${diskimgdir}/${IMAGE}
diskimg=${IMAGEDIR}/hda.img

tmpdir=`mktemp -d /tmp/${IMAGE}.XXXXXX`
SERIAL=${tmpdir}/serial
MONITOR=${tmpdir}/monitor


if [ ! -f ${diskimg} ] ; then
  echo "No $diskimg available.  Run dapper-i386.sh first."
  exit 2
fi

echo IPADDR=${IPADDR}
echo control/tmp dir=${tmpdir}

${qemu_i386} -snapshot -m 512 -smp ${qemu_threads} \
  -nographic \
  -serial unix:${SERIAL},server,nowait \
  -monitor unix:${MONITOR},server,nowait \
  -net nic,macaddr=${MACADDR} -net tap,ifname=${IFACE},script=no \
  -boot c -hda ${diskimg} -no-reboot & qemu=$!

while ! ${SSH} root@${IPADDR} hostname >/dev/null 2>&1
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

${SSH} root@${IPADDR} <<EOF
lvcreate --name home --size 5G sysvg
mke2fs /dev/sysvg/home
mount /dev/sysvg/home /home
useradd build
mkdir -p ~build/.ssh ~build/zumastor/build
cp ~/.ssh/authorized_keys ~build/.ssh/
chown -R build ~build
EOF

tar cf - --exclude build * | ssh build@${IPADDR} tar xf - -C zumastor
${SCP} build/linux-${KERNEL_VERSION}.tar.bz2 build@${IPADDR}:zumastor/build/

${SSH} root@${IPADDR} <<EOF 
cd ~build/zumastor
./builddepends.sh
echo CONCURRENCY_LEVEL := ${threads} >> /etc/kernel-pkg.conf
EOF

# Use the full kernel config unless qemu symlink points to another config file
KERNEL_CONFIG=kernel/config/full
if [ -e kernel/config/qemu ] ; then
  KERNEL_CONFIG=kernel/config/qemu
fi

${SSH} build@${IPADDR} <<EOF
cd zumastor
echo $SVNREV >SVNREV
./buildcurrent.sh $KERNEL_CONFIG
EOF

BUILDSRC="build@${IPADDR}:zumastor/build"
DEBVERS="${VERSION}-r${SVNREV}"
KVERS="${KERNEL_VERSION}-zumastor-r${SVNREV}_1.0"

for f in \
    ${BUILDSRC}/ddsnap_${DEBVERS}_${ARCH}.changes \
    ${BUILDSRC}/ddsnap_${DEBVERS}_${ARCH}.deb \
    ${BUILDSRC}/ddsnap_${DEBVERS}.dsc \
    ${BUILDSRC}/ddsnap_${DEBVERS}.tar.gz \
    ${BUILDSRC}/zumastor_${DEBVERS}_${ARCH}.changes \
    ${BUILDSRC}/zumastor_${DEBVERS}_${ARCH}.deb \
    ${BUILDSRC}/zumastor_${DEBVERS}.dsc \
    ${BUILDSRC}/zumastor_${DEBVERS}.tar.gz \
    ${BUILDSRC}/kernel-headers-${KVERS}_${ARCH}.deb \
    ${BUILDSRC}/kernel-image-${KVERS}_${ARCH}.deb
do
  ${SCP} $f build/
done

${SSH} root@${IPADDR} halt

wait $qemu

rm -rf ${tmpdir}
