#!/bin/bash -x

# import all the sweet variables used throughout the ddsnap scripts
. ddsnap_env.sh

cd $DDSNAP_HOME

echo "Creating logical volume locally and remotely"
SERVER_SOCK_NAME=@test # remove this later when we get rid of AF_INET stuff
echo "0 $SIZE_LOGICAL_VOL ddsnap $SNAPSTORE_DEV $ORIGIN_DEV $SERVER_SOCK_NAME -1" | dmsetup create $LOGICAL_VOL_NAME
ssh $REMOTE_HOST "(echo 0 $SIZE_LOGICAL_VOL ddsnap $SNAPSTORE_DEV $ORIGIN_DEV $SERVER_SOCK_NAME -1 | dmsetup create $LOGICAL_VOL_NAME) &> /dev/null < /dev/null"

cd -  # go back to the original directory 
