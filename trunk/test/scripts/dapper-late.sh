#!/bin/sh

# run after the base packages are installed, from inside the d-i environment
# copied into the initrd so it doesn't need to be fetched over network
# connections.

# $Id$
# Copyright 2007 Google Inc.
# Author: Drake Diedrich <dld@google.com>
# License: GPLv2

set -e

mkdir /target/root/.ssh
cp /authorized_keys /target/root/.ssh

echo 'Acquire::http::Pipeline-Depth "0";' > /target/etc/apt/apt.conf.d/01cache

apt-install openssh-server
