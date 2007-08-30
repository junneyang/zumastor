#!/bin/sh -x
#
# $Id$
#
# Run the full test cycle once.  May be updated, changing the behavior
# of the continuous build between versions.
#
# Copyright 2007 Google Inc.  All rights reserved.
# Author: Drake Diedrich (dld@google.com)
# License: GPLv2
#

set -e

repo=${PWD}
diskimgdir=${HOME}/testenv
[ -x /etc/default/testenv ] && . /etc/default/testenv

TUNBR=tunbr


# Die if more than four hours pass. Really.
( sleep 14400 ; kill -6 $$ ; exit 0 ) & tmoutpid=$!
 


pushd cbtb/tests/1
for f in *-test.sh
do
  if time ${TUNBR} ../../../../test-zuma-dapper-i386.sh $f
  then
    echo PASS $f
  else
    retval=$?
    echo FAIL $f
  fi
done
popd

pushd cbtb/tests/2
for f in *-test.sh
do
  if time ${TUNBR} ${TUNBR} ../../../../test-zuma-dapper-i386.sh $f
  then
    echo PASS $f
  else
    retval=$?
    echo FAIL $f
  fi
done
popd


exit ${retval}
