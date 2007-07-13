#!/bin/sh -x
#
# $Id$
#
# Continously svn update zumastor, build packages, install them, and run the
# tests.

set -e

diskimgdir=${HOME}/.testenv

[ -x /etc/default/testenv ] && . /etc/default/testenv


while true
do
  svn update
  REVISION=`svn info | awk '/Revision:/ { print $2; }'`
  export REVISION
  rm -f *.deb
  rm -rf build
  time ./build_packages.sh test/config/qemu-config
  rm -f ${diskimgdir}/zuma/dapper-i386.img
  (cd test/scripts ; tunbr ./zuma-dapper-i386.sh ; \
    if time tunbr ./test-zuma-dapper-i386.sh snapshot-test.sh ; then \
      echo -n "Revision $REVISION is good " >> build.log ; else \
      echo -n "Revision $REVISION is bad " >> build.log ; fi ; \
    date >> build.log
  )
done
