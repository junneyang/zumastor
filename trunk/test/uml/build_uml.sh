#!/bin/bash
# Copyright 2007 Google Inc.
# Author: Jiaying Zhang <jiayingz@google.com>

# build kernel linux uml with the KERNEL_VERSION specified in config_uml and with ddsnap kerenel patches

. config_uml

dpkg -s uml-utilities >& $LOG || apt-get -y install uml-utilities || exit $?

echo -n Getting kernel sources from kernel.org ...
wget -c http://www.kernel.org/pub/linux/kernel/v2.6/linux-${KERNEL_VERSION}.tar.bz2 >> $LOG || exit $?
echo -e "done.\n"

echo -n Unpacking kernel...
tar xjf linux-${KERNEL_VERSION}.tar.bz2 || exit 1
echo -e "done.\n"

echo Applying patches...
cd linux-${KERNEL_VERSION} || exit 1
cp $ZUMA_REPOSITORY/ddsnap/kernel/dm-ddsnap.* drivers/md/
for patch in $ZUMA_REPOSITORY/zumastor/patches/${KERNEL_VERSION}/* $ZUMA_REPOSITORY/ddsnap/patches/${KERNEL_VERSION}/*; do
        echo "   $patch"
        < $patch patch -p1 >> $LOG || exit 1
done
echo -e "done.\n"

echo -n Building kernel...
cp ../uml.config-${KERNEL_VERSION} .config
make oldconfig ARCH=um >> $LOG || exit 1
make linux ARCH=um >> $LOG || exit 1
cd ..
echo -e "done.\n"
