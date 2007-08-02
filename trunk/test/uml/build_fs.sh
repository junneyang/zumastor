#!/bin/bash
# Copyright 2007 Google Inc.
# Author: Jiaying Zhang <jiayingz@google.com>

# install all the required libraries and utilities based on the image downloaded from uml website

. config_uml

[[ $# -eq 1 ]] || { echo "Usage: build_fs.sh uml_fs"; exit 1; }
uml_fs=$1

echo -n Getting Debian uml root file system image...
wget -c http://uml.nagafix.co.uk/Debian-3.1/Debian-3.1-x86-root_fs.bz2 || exit $?
echo -e "done.\n"

echo -n Unpacking root file system image...
bunzip2 Debian-3.1-x86-root_fs.bz2 >> $LOG
mv Debian-3.1-x86-root_fs $uml_fs
echo -e "done.\n"

mount -o loop $uml_fs /mnt || exit 1

echo -n Setting up apt-get...
cp /etc/resolv.conf /mnt/etc/resolv.conf || exit 1
echo "deb ftp://ftp.us.debian.org/debian/ stable main contrib non-free" > /mnt/etc/apt/sources.list
chroot /mnt apt-get update
echo -e "done.\n"

echo -n Installing ssh...
chroot /mnt apt-get -y install openssh-client >> $LOG || exit 1
chroot /mnt apt-get -y install openssh-server >> $LOG || exit 1
# to get rid of the need for initial confirmation
echo "    StrictHostKeyChecking no" >> /mnt/etc/ssh/ssh_config
echo "    VerifyHostKeyDNS yes" >> /mnt/etc/ssh/ssh_config
echo -e "done.\n"

# required because the debian version of zlib1g does not work with ddsnap built with ubuntu zlib1g
echo -n Installing required libraries...
apt-get --download-only --reinstall -y install zlib1g
cp /var/cache/apt/archives/zlib1g_*.deb /mnt
chroot /mnt dpkg -i zlib1g_*.deb
rm /mnt/zlib1g_*.deb
echo -e "done.\n"

# dmsetup, tree
echo -n Installing required utilities...
chroot /mnt apt-get -y install dmsetup >> $LOG || exit 1
chroot /mnt apt-get -y install tree >> $LOG || exit 1
echo -e "done.\n"

echo -n Upgrading ddsnap and zumastor...
pushd $ZUMA_REPOSITORY/ddsnap
make install prefix=/mnt
popd
pushd $ZUMA_REPOSITORY/zumastor
make install DESTDIR=/mnt
popd
echo -e "done.\n"

echo -n Setting up ssh...
mkdir -p /mnt/root/.ssh
cat ~/.ssh/id_dsa.pub > /mnt/root/.ssh/authorized_keys
echo -e "done.\n"

echo -n Making nodes for ubdb and ubdc...
mknod /mnt/dev/ubdb b 98 16
mknod /mnt/dev/ubdc b 98 32
echo -e "done.\n"
umount /mnt
