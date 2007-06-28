#!/bin/sh

# Set up the initial Ubuntu/dapper template image, for use when duplicating
# the install to multiple server/client tests.  Makes use of tunbr and
# a presetup br1, squid, and dnsmasq.

# $Id$
# Copyright 2007 Google Inc.
# Author: Drake Diedrich <dld@google.com>
# License: GPLv2

set -e

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

diskimg=${diskimgdir}/template/dapper-i386.img

if [ ! -f ${diskimg} ] ; then

  # extract and repack the initrd with the desired preseed file
  tmpdir=`mktemp -d`
  mkdir ${tmpdir}/initrd
  cp dapper.cfg ${tmpdir}/initrd/preseed.cfg
  cp dapper-early.sh ${tmpdir}/initrd/early.sh
  cp dapper-late.sh ${tmpdir}/initrd/late.sh
  passwd=`pwgen 8 1`
  pwhash=`echo ${passwd} | mkpasswd -s --hash=md5`
  cat >>${tmpdir}/initrd/preseed.cfg <<EOF
d-i     passwd/root-password-crypted    password ${pwhash}
d-i     passwd/user-password-crypted    password ${pwhash}
d-i	passwd/user-fullname            string ${USER}
d-i	passwd/username                 string ${USER}
EOF

  cat ~/.ssh/*.pub > ${tmpdir}/initrd/authorized_keys
  
  fakeroot <<EOF
cd ${tmpdir}/initrd
zcat ${tftpdir}/ubuntu-installer/i386/initrd.gz | cpio -i
find . -print0 | cpio -0 -o -H newc | gzip -9 > ${tftpdir}/${USER}/ubuntu-installer/i386/initrd.gz
EOF
  rm -rf ${tmpdir}
  chmod ugo+r ${tftpdir}/${USER}/ubuntu-installer/i386/initrd.gz
  
  qemu-img create -f qcow2 ${diskimg} ${size}

  cat >${MACFILE} <<EOF
DEFAULT server
LABEL server
	kernel ubuntu-installer/i386/linux
	append base-config/package-selection= base-config/install-language-support=false vga=normal initrd=${USER}/ubuntu-installer/i386/initrd.gz ramdisk_size=13531 root=/dev/rd/0 rw preseed/file=/preseed.cfg DEBCONF_DEBUG=5 --
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
  