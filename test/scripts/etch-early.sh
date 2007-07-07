#!/bin/sh

# run early in the dapper install, from the preseed

# $Id$
# Copyright 2007 Google Inc.
# Author: Drake Diedrich <dld@google.com>
# License: GPLv2

set -e

touch /early-running

#mkdir -p /target/etc/apt/
#echo 'Acquire::http::Pipeline-Depth "0";' >> /target/etc/apt/apt.conf

touch /early-ran

