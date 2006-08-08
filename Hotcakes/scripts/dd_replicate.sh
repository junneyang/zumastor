#!/bin/bash -x

# import all the sweet variables used throughout the ddsnap scripts
. ddsnap_env.sh

if [ $# -lt 2  ]
then
   echo "usage: `basename $0` <older_snapshot_id> <newer_snapshot_id> "
   exit 1
fi

cd $DDSNAP_HOME

echo "Creating changelist file: changelist${1}-${2}"
./${CREATE_CHANGELIST} $SERVER_SOCK_NAME changelist${1}-${2} $1 $2
echo "Creating delta file: deltafile${1}-${2}"
./${CREATE_DELTA} $SERVER_SOCK_NAME deltafile${1}-${2} /dev/mapper/${SNAPSHOT_VOL_NAME}$1 /dev/mapper/${SNAPSHOT_VOL_NAME}$2
echo "Copying deltafile over to $REMOTE_HOST"
scp deltafile${1}-${2} ${REMOTE_HOST}:${DDSNAP_HOME}/
echo "Applying deltafile to $REMOTE_HOST"
ssh $REMOTE_HOST ${DDSNAP_HOME}/${APPLY_DELTA} ${DDSNAP_HOME}/deltafile${1}-${2} /dev/mapper/${LOGICAL_VOL_NAME}

echo "Creating snapshot $2 on remote host $REMOTE_HOST"
ssh $REMOTE_HOST ${DDSNAP_HOME}/${CREATE_SNAPSHOT} $SERVER_SOCK_NAME $2

ssh $REMOTE_HOST "(echo 0 $SIZE_LOGICAL_VOL ddsnap $SNAPSTORE_DEV $ORIGIN_DEV $SERVER_SOCK_NAME $2 | dmsetup create ${SNAPSHOT_VOL_NAME}$2) &> /dev/null < /dev/null"

cd -
