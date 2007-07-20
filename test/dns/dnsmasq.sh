#!/bin/sh
#
# $Id$
#
#    Install and configure dnsmasq to dynamically serve DNS and DHCP
# for the range of addresses managed by tunbr, part of the zumastor
# virtual test environment.
#    The default /etc/dnsmasq.conf is locally diverted and replaced
# with the configuration file inline below, with instructions on how
# to remove the diversion if the test environment is removed or
# needs to be reinstalled.
#
# Copyright Google Inc. All rights reserved.
# Author: Drake Diedrich (dld@google.com)

aptitude install dnsmasq

# only divert /etc/dnsmasq.conf once
if [ ! -f /etc/dnsmasq.conf.distrib ] ; then
  dpkg-divert --local --rename --add /etc/dnsmasq.conf

  # Write a new configuration file to /etc/dnsmasq.conf
  cat >/etc/dnsmasq.conf <<EOF
#
# dnsmasq configuration file for use hosting a zumastor test environment.
# dhcp-range must include at least the range that tunbr is compiled to use.
# dhcp-leasefile must match the lease file that tunbr will also manage.
#
# The default configuration file for dnsmasq (/etc/dnsmasq.conf) as
# distributed by the dnsmasq package was diverted, see man dpkg-divert.
# To put things back in their original state, and to return to normal
# Debian conffile behavior, remove this diversion as follows:
#    rm /etc/dnsmasq.conf
#    dpkg-divert --remove /etc/dnsmasq.conf
#

interface=br1
interface=lo
bind-interfaces
dhcp-range=192.168.23.50,192.168.23.253,12h
dhcp-leasefile=/var/lib/misc/dnsmasq.leases
log-queries
dhcp-boot=/pxelinux.0,boothost,192.168.23.1
domain-needed
bogus-priv
EOF
  /etc/init.d/dnsmasq restart
fi
