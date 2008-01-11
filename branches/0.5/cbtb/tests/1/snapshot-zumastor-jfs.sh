#!/bin/sh -x
#
# $Id$
#
# Set up zumastor with origin and snapshot stores on /dev/sdb and /dev/sdc,
# iterate through a few snapshots to verify that each is unique and
# stable when taken.
#
# Copyright 2007-2008 Google Inc.  All rights reserved
# Author: Drake Diedrich (dld@google.com)


set -e

# The required sizes of the sdb and sdc devices in M.
# Read only by the test harness.
HDBSIZE=16
HDCSIZE=16

# Terminate test in 10 minutes.  Read by test harness.
TIMEOUT=600

# wait for file.  The first argument is the timeout, the second the file.
timeout_file_wait() {
  local max=$1
  local file=$2
  local count=0
  while [ ! -e $file ] && [ $count -lt $max ]
  do 
    let "count = count + 1"
    sleep 1
  done
  [ -e $file ]
  return $?
}



echo "1..6"

apt-get update
aptitude install -y jfsutils

mount
ls -l /dev/sdb /dev/sdc
zumastor define volume testvol /dev/sdb /dev/sdc --initialize
mkfs.jfs -q /dev/mapper/testvol
zumastor define master testvol -h 24 -d 7

echo ok 1 - testvol set up

sync
zumastor snapshot testvol hourly 

if timeout_file_wait 30 /var/run/zumastor/snapshot/testvol/hourly.0 ; then
  echo "ok 3 - first snapshot mounted"
else
  ls -lR /var/run/zumastor/
  echo "not ok 3 - first snapshot mounted"
  exit 3
fi

date >> /var/run/zumastor/mount/testvol/testfile

if [ ! -f /var/run/zumastor/snapshot/testvol/hourly.0/testfile ] ; then
  echo "ok 4 - testfile not present in first snapshot"
else
  ls -lR /var/run/zumastor/
  echo "not ok 4 - testfile not present in first snapshot"
  exit 4
fi

sync
zumastor snapshot testvol hourly 


if timeout_file_wait 30 /var/run/zumastor/snapshot/testvol/hourly.1 ; then
  echo "ok 5 - second snapshot mounted"
else
  ls -lR /var/run/zumastor/
  echo "not ok 5 - second snapshot mounted"
  exit 5
fi

  
if diff -q /var/run/zumastor/mount/testvol/testfile \
    /var/run/zumastor/snapshot/testvol/hourly.0/testfile 2>&1 >/dev/null ; then
  echo "ok 6 - identical testfile immediately after second snapshot"
else
  ls -lR /var/run/zumastor/
  echo "not ok 6 - identical testfile immediately after second snapshot"
  exit 5
fi

date >> /var/run/zumastor/mount/testvol/testfile

if ! diff -q /var/run/zumastor/mount/testvol/testfile \
    /var/run/zumastor/snapshot/testvol/hourly.0/testfile 2>&1 >/dev/null ; then
  echo "ok 6 - testfile changed between origin and second snapshot"
else
  ls -lR /var/run/zumastor
  echo "not ok 6 - testfile changed between origin and second snapshot"
  exit 6
fi

exit 0
