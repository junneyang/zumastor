#!/bin/sh -x
#
# $Id$
#

# Continuously svn update, run the encapsulated build, and the test
# suite.  Copy this and the dapper-build.sh and runtests.sh scripts to
# the parent directory to avoid running unsafe code on your host
# machine.  Code direct from the repository is only run on virtual instances.

sendmail=/usr/sbin/sendmail
TUNBR=tunbr
email_failure="zumastor-buildd@google.com"
email_success="zumastor-buildd@google.com"

if [ ! -f zumastor/Changelog ] ; then
  echo "cp $0 to the parent directory of the zumastor repository and "
  echo "run from that location.  Periodically inspect the three "
  echo "host scripts for changes and redeploy them."
  exit 1
fi

cd zumastor

# build and test the current working directory packages
revision=`svn info | awk '/Revision:/ { print $2; }'`
buildlog=`mktemp`
testlog=`mktemp`
if ${TUNBR} ../dapper-build.sh >${buildlog} 2>&1 ; then
  if ${TUNBR} ${TUNBR} ../runtests.sh >${testlog} 2>&1 ; then
    ( echo "Subject: zumastor r$revision build and test success" ;\
      echo ; cat ${buildlog} ${testlog} ) | \
    ${sendmail} ${email_success}
  else
    ( echo "Subject: zumastor r$revision test failure" ;\
      echo ; cat ${testlog} ) | \
    ${sendmail} ${email_failure}
  fi
else
  ( echo "Subject: zumastor r$revision build failure" ;\
    echo ; cat ${buildlog} ) | \
  ${sendmail} ${email_failure}
fi      
rm -f ${buildlog} ${testlog}

# loop waiting for a new update
oldrevision=${revision}
while true
do
  svn update
  if [ "x$revision" = "x$oldrevision" ]
  then
    sleep 300
  else
    # restart continuous.sh, begining a new build
    exec $0
  fi
done
