#!/bin/bash
# a bunch of bash variables for the ddsnap scripts

DDSNAP_HOME=/src/2.6.17-rc6-dd/drivers/md/ddraid
META_DEV=
SNAPSTORE_DEV=/dev/test-snapstore
ORIGIN_DEV=/dev/test-origin
ORIGIN_PHYS_DEV=/dev/sda7
LOGICAL_VOL_NAME=vol
SNAPSHOT_VOL_NAME=snap
SERVER_SOCK_NAME=/tmp/server
AGENT_SOCK_NAME=/tmp/control
SIZE_LOGICAL_VOL=$(( 2 * $(egrep $(echo $ORIGIN_PHYS_DEV | sed -e 's@^.*/@@') /proc/partitions | awk '{print $3}'))) 
REMOTE_HOST=victory
MAX_SNAPSHOTS=64 # just for test purposes.. max can be 64 

# command names... it's early in development, so they might change
INIT_STORAGE=mkddsnap
CREATE_SNAPSHOT="ddsnap create-snap"
DELETE_SNAPSHOT="ddsnap delete-snap"
LIST_SNAPSHOT="ddsnap list $SERVER_SOCK_NAME"
CREATE_CHANGELIST="ddsnap create-cl"
CREATE_DELTA="ddsnap create-delta"
APPLY_DELTA="ddsnap apply-delta"
