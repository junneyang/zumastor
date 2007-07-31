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

diskimgdir=${HOME}/.testenv
[ -x /etc/default/testenv ] && . /etc/default/testenv

REVISION=`svn info | awk '/Revision:/ { print $2; }'`
export REVISION
rm -f ${diskimgdir}/zuma/dapper-i386.img

cd test/scripts
tunbr ./zuma-dapper-i386.sh
if time tunbr ./test-zuma-dapper-i386.sh snapshot-test.sh
then
  echo -n "Revision $REVISION is good " >> build.log
else
  echo -n "Revision $REVISION is bad " >> build.log
fi
date >> build.log
