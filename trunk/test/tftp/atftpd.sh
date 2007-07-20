#!/bin/sh
#
# $Id$
#
# Install and configure atftpd for use with the virtualization test
# environment for zumastor.
#
# Copyright 2007 Google Inc.  All rights reserved.
# Author: Drake Diedrich (dld@google.com)

# Create /tftpboot if it doesn't exist.  May already be a symlink or
# directory.
[ -e /tftpboot ] || mkdir /tftpboot

# preferred over apt-get, remembers what was a dependency and what was
# actually requested.
aptitude -y install atftpd

# This default config file is always installed by the atftpd package
. /etc/default/atftpd

# inetd doesn't know how to bind to individual interfaces or addresses,
# and some hosts use xinetd instead.  Just run atftpd standalone.
if [ "$USE_INETD" = "true" ] ; then
  sed -i s/USE_INETD=true/USE_INETD=false/ /etc/default/atftpd
fi

# Make sure the atftpd options include --daemon
if ! echo $OPTIONS | egrep daemon ; then
  sed -i s/OPTIONS=\"/OPTIONS=\"--daemon / /etc/default/atftpd
fi

# make sure atftpd only binds to the host-only IP address 192.168.23.1,
# so it's only serving to local instances, not the rest of the world.
if ! echo $OPTIONS | egrep bind-address ; then
  sed -i 's/=\"/=\"--bind-address 192.168.23.1 /' /etc/default/atftpd
fi

# restart atftpd now that it's been reconfigured as a daemon
/etc/init.d/atftpd restart

# Populate /tftpboot with the Debian and Ubuntu network installers.
# Allow any user (particularly the atftpd user) to access them.
cd /tftpboot
wget -O - http://archive.ubuntu.com/ubuntu/dists/dapper/main/installer-i386/current/images/netboot/netboot.tar.gz | tar zxvf -
wget -O - http://ftp.us.debian.org/debian/dists/etch/main/installer-amd64/current/images/netboot/netboot.tar.gz | tar zxvf -
wget -O - http://ftp.us.debian.org/debian/dists/etch/main/installer-i386/current/images/netboot/netboot.tar.gz | tar zxvf -
chmod -R o+rX .
