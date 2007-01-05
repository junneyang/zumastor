#!/bin/bash
# a bunch of bash variables for the ddsnap scripts

DDSNAP_HOME=/src/linux-2.6.17-rc6/drivers/md/ddraid
META_DEV=
META_DEV=
SNAPSTORE_DEV=/dev/test-snapstore
ORIGIN_DEV=/dev/test-origin
ORIGIN_PHYS_DEV=/dev/sda7
LOGICAL_VOL_NAME=vol
SNAPSHOT_VOL_NAME=snap
SERVER_SOCK_NAME=/server
AGENT_SOCK_NAME=/control
SIZE_LOGICAL_VOL=$(${DDSNAP_HOME}/ddsnap status ${SERVER_SOCK_NAME} --size | tail -n 1)
REMOTE_HOST=victory
MAX_SNAPSHOTS=64 # just for test purposes.. max can be 64 

# command names... it's early in development, so they might change
INIT_STORAGE="ddsnap initialize"
CREATE_SNAPSHOT="ddsnap create"
DELETE_SNAPSHOT="ddsnap delete"
LIST_SNAPSHOT="ddsnap list $SERVER_SOCK_NAME"
CREATE_CHANGELIST="ddsnap delta changelist"
CREATE_DELTA="ddsnap delta create"
APPLY_DELTA="ddsnap delta apply"
