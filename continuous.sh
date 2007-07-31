#!/bin/sh -x
#
# $Id$
#

# Continuously svn update, run the encapsulated build, and the test
# suite.  Copy this and the build-dapper.sh and runtests.sh scripts to
# the parent directory to avoid running unsafe code on your host
# machine.  Code direct from the repository is only run on virtual instances.

if [ ! -f zumastor/Changelog ] ; then
  echo "cp $0 to the parent directory of the zumastor repository and "
  echo "run from that location.  Periodically inspect the three "
  echo "host scripts for changes and redeploy them."
  exit 1
fi

cd zumastor

oldrevision=""

while true
do
  svn update
  revision=`svn info | awk '/Revision:/ { print $2; }'`
  if [ "$revision" = "$oldrevision" ]
  then
    sleep 300
  else
    ../dapper-build.sh
    ../runtests.sh
  fi
  oldrevision=$revision
done
