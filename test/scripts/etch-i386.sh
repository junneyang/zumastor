#!/bin/sh

# Set up the initial Debian/etch template image, for use when duplicating
# the install to multiple server/client tests.  Makes use of tunbr and
# a presetup br1, squid, and dnsmasq.

# $Id$
# Copyright 2007 Google Inc.
# Author: Drake Diedrich <dld@google.com>
# License: GPLv2

set -e

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

if [ ! -e ${diskimgdir}/template ]; then
  mkdir -p ${diskimgdir}/template
fi

diskimg=${diskimgdir}/template/etch-i386.img

if [ ! -f ${diskimg} ] ; then

  # extract and repack the initrd with the desired preseed file
  tmpdir=`mktemp -d`
  mkdir -p ${tmpdir}/initrd
  cp etch.cfg ${tmpdir}/initrd/preseed.cfg
  cp etch-early.sh ${tmpdir}/initrd/early.sh
  cp etch-late.sh ${tmpdir}/initrd/late.sh
  passwd=`pwgen 8 1`
  pwhash=`echo ${passwd} | mkpasswd -s --hash=md5`
  cat >>${tmpdir}/initrd/preseed.cfg <<EOF
d-i     passwd/root-password-crypted    password ${pwhash}
d-i     passwd/user-password-crypted    password ${pwhash}
d-i	passwd/user-fullname            string ${USER}
d-i	passwd/username                 string ${USER}
# I really don't know why eth0 becomes eth1 on second boot.
EOF

  cat ~/.ssh/*.pub > ${tmpdir}/initrd/authorized_keys
  
  if [ ! -d ${tftpdir}/${USER} ] ; then
    mkdir -p ${tftpdir}/${USER}
    sudo chown ${USER} ${tftpdir}/${USER}
  fi
  if [ ! -d ${tftpdir}/${USER}/debian-installer/i386 ]; then
    mkdir -p ${tftpdir}/${USER}/debian-installer/i386
  fi

  fakeroot <<EOF
cd ${tmpdir}/initrd
zcat ${tftpdir}/debian-installer/i386/initrd.gz | cpio -i
find . -print0 | cpio -0 -o -H newc | gzip -9 > ${tftpdir}/${USER}/debian-installer/i386/initrd.gz
EOF
  rm -rf ${tmpdir}
  chmod ugo+r ${tftpdir}/${USER}/debian-installer/i386/initrd.gz
  
  qemu-img create -f qcow2 ${diskimg} ${size}

  cat >${MACFILE} <<EOF
DEFAULT auto
LABEL auto
	kernel debian-installer/i386/linux
	append auto=true priority=critical vga=normal initrd=${USER}/debian-installer/i386/initrd.gz preseed/file=/preseed.cfg -- 
PROMPT 0
TIMEOUT 1
EOF
  chmod ugo+r ${MACFILE}

  ${qemu_i386} \
    -net nic,macaddr=${MACADDR} -net tap,ifname=${IFACE},script=no \
    -boot n -hda ${diskimg} -no-reboot
    
  echo "${diskimg} installed.  root and ${USER} passwords are: ${passwd}"

else
  echo "image ${diskimg} already exists."
  echo "rm if you wish to recreate it and all of its derivatives."
fi

