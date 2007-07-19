#!/bin/sh -x
#
# $Id$
#
# Add the br1 bridge to /etc/network/interfaces, as required by tests in
# the tunbr virtualization environment.
#

if egrep "iface +br1" /etc/network/interfaces; then
  echo "iface br1 already found in /etc/network/interfaces."
  echo "Modify by hand if necessary."
  exit 1
else
  aptitude install bridge-utils
  cat >>/etc/network/interfaces <<EOF

auto br1
iface br1 inet static
	pre-up brctl addbr \$IFACE
	address 192.168.23.1
	network 192.268.23.0
	netmask 255.255.255.0
EOF
  ifup br1
fi
