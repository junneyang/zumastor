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

if [ "x$MACFILE" = "x" -o "x$MACADDR" = "x" -o "x$IFACE" = "x" ] ; then
  echo "Run this script under tunbr"
  exit 1
fi

# Die if more than four hours pass
( sleep 14400 ; kill $$ ; exit 0 ) & tmoutpid=$!
 

REVISION=`svn info | awk '/Revision:/ { print $2; }'`
export REVISION
IMAGE=zuma-dapper-i386
IMAGEDIR=${diskimgdir}/${IMAGE}
diskimg=${IMAGEDIR}/hda.img

rm -f ${diskimg}

pushd test/continuous
${repo}/zuma-dapper-i386.sh
popd

pushd test/scripts
if time ../../../test-zuma-dapper-i386.sh snapshot-test.sh
then
  echo -n "Revision $REVISION is good "
  retval=0
else
  echo -n "Revision $REVISION is bad "
  retval=1
fi
popd

exit ${retval}
