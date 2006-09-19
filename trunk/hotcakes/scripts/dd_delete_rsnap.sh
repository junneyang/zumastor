#!/bin/bash

# import all the sweet variables used throughout the ddsnap scripts
. ddsnap_env.sh

if [ ! $1 ]
then
   echo "usage: `basename $0` <snapshot_id> "
   exit 1
fi

cd $DDSNAP_HOME

echo "Creating snapshot id $1"
ssh -l root $REMOTE_HOST "(/sbin/dmsetup remove ${SNAPSHOT_VOL_NAME}$1) &> /dev/null < /dev/null"
ssh -l root $REMOTE_HOST ${DDSNAP_HOME}/${DELETE_SNAPSHOT} $SERVER_SOCK_NAME $1

