#!/bin/bash
# a bunch of bash variables for the ddsnap scripts

DDSNAP_HOME=/src/2.6.17-rc6-dd/drivers/md/ddraid
SNAPSTORE_DEV=/dev/test-snapstore
ORIGIN_DEV=/dev/test-origin
LOGICAL_VOL_NAME=lvol
SNAPSHOT_VOL_NAME=snap
SERVER_SOCK_NAME=localhost:8080
SIZE_LOGICAL_VOL=16064936  # number of sectors
REMOTE_HOST=blinkin

# command names... it's early in development, so they might change
INIT_STORAGE=mkddsnap
CREATE_SNAPSHOT="ddsnap create-snapshot"
CREATE_CHANGELIST="ddsnap generate-changelist"
CREATE_DELTA="ddsnap create-delta"
APPLY_DELTA="ddsnap apply-delta"
