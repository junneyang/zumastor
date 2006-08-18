#!/bin/bash -x

# import all the sweet variables used throughout the ddsnap scripts
. ddsnap_env.sh

if [ ! $1 ]
then
   echo "usage: `basename $0` <snapshot_id> "
   exit 1
fi

cd $DDSNAP_HOME

echo "Creating snapshot id $1"
./${CREATE_SNAPSHOT} $SERVER_SOCK_NAME $1
SERVER_SOCK_NAME=@test
echo 0 $SIZE_LOGICAL_VOL ddsnap $SNAPSTORE_DEV $ORIGIN_DEV $SERVER_SOCK_NAME $1 | dmsetup create ${SNAPSHOT_VOL_NAME}$1


cd -
