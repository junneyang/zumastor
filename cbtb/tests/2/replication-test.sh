#!/bin/sh -x
#
# $Id$
#
# Set up sysvg with origin and snapshot store on master and secondary machine.
# Begin replication cycle between machines
# and wait until it arrives and can be verified.
# Modify the origin and verify that the modification also arrives at the
# backup.
#
# Requires that the launch environment (eg. test-zuma-dapper-i386.sh) export
# both $IPADDR and $IPADDR2 to the paramter scripts.
#
# Copyright 2007 Google Inc.  All rights reserved
# Author: Drake Diedrich (dld@google.com)


set -e

# self terminate test in 20 minutes
( sleep 1200 ; kill -9 $$ ; exit 9 ) & tmoutpid=$!

slave=${IPADDR2}

SSH='ssh -o StrictHostKeyChecking=no'

retval=0


# necessary at the moment, looks like a zumastor bug
SLEEP=5

echo "1..4"

echo ${IPADDR} master >>/etc/hosts
echo ${IPADDR2} slave >>/etc/hosts
hostname master
lvcreate --size 1m -n test sysvg
lvcreate --size 2m -n test_snap sysvg
zumastor define volume testvol /dev/sysvg/test /dev/sysvg/test_snap --initialize
mkfs.ext3 /dev/mapper/testvol
zumastor define master testvol -h 24 -d 7
zumastor define target testvol slave:11235 -p 30
echo ok 1 - master testvol set up

echo ${IPADDR} master | ${SSH} root@${slave} "cat >>/etc/hosts"
echo ${IPADDR2} slave | ${SSH} root@${slave} "cat >>/etc/hosts"
${SSH} root@${slave} hostname slave
${SSH} root@${slave} lvcreate --size 1m -n test sysvg
${SSH} root@${slave} lvcreate --size 2m -n test_snap sysvg
${SSH} root@${slave} zumastor define volume testvol /dev/sysvg/test /dev/sysvg/test_snap --initialize
${SSH} root@${slave} zumastor define source testvol master --period 600
echo ok 2 - slave testvol set up
 
zumastor replicate testvol slave
echo ok 3 - replication kicked off

# reasonable wait for these small volumes to finish the initial replication
sleep 120
date >>/var/run/zumastor/mount/testvol/testfile
sync ; zumastor snapshot testvol hourly 
hash=`md5sum /var/run/zumastor/mount/testvol/testfile`

# give it a minute to replicate (on a 30 second cycle), and verify
# that it is there
sleep 60
rhash=`${SSH} root@${slave} md5sum /var/run/zumastor/mount/testvol/testfile`

if [ "$rhash" = "$hash" ] ; then
  echo ok 4 - origin and slave testfiles are in sync
else
  echo not ok 4 - origin and slave testfiles are in sync
  exit 4
fi

kill $tmoutpid

exit 0
