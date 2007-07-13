#!/bin/sh -x
#
# $Id$
#
# Continuously svn update and run the test suite.

set -e

while true
do
  svn update
  ./runtests.sh
done
