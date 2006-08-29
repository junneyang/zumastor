#!/bin/bash 

# import all the sweet variables used throughout the ddsnap scripts
. ddsnap_env.sh

if [ $# -eq 4  ]
then
   echo "usage: `basename $0` [-d|-r|-t] [-c|-n] <older_snapshot_id> <newer_snapshot_id> "
   exit 1
fi

cd $DDSNAP_HOME

echo "Creating changelist file: changelist${3}-${4}"
./${CREATE_CHANGELIST} $SERVER_SOCK_NAME changelist${3}-${4} $3 $4
echo "Creating delta file: deltafile${3}-${4}"
./${CREATE_DELTA} $1 $2 changelist${3}-${4} deltafile${3}-${4} /dev/mapper/${SNAPSHOT_VOL_NAME}$3 /dev/mapper/${SNAPSHOT_VOL_NAME}$4
echo "Copying deltafile over to $REMOTE_HOST"
scp deltafile${3}-${4} ${REMOTE_HOST}:${DDSNAP_HOME}/
echo "Applying deltafile to $REMOTE_HOST"
ssh $REMOTE_HOST ${DDSNAP_HOME}/${APPLY_DELTA} ${DDSNAP_HOME}/deltafile${3}-${4} /dev/mapper/${LOGICAL_VOL_NAME}

echo "Creating snapshot $4 on remote host $REMOTE_HOST"
ssh $REMOTE_HOST ${DDSNAP_HOME}/${CREATE_SNAPSHOT} $SERVER_SOCK_NAME $4

ssh $REMOTE_HOST "(echo 0 $SIZE_LOGICAL_VOL ddsnap $SNAPSTORE_DEV $ORIGIN_DEV $AGENT_SOCK_NAME $4 | dmsetup create ${SNAPSHOT_VOL_NAME}$4) &> /dev/null < /dev/null"

