#!/bin/bash 

# import all the sweet variables used throughout the ddsnap scripts
. ddsnap_env.sh

cd $DDSNAP_HOME

echo "Creating logical volume locally"
echo "0 $SIZE_LOGICAL_VOL ddsnap $SNAPSTORE_DEV $ORIGIN_DEV $AGENT_SOCK_NAME -1" | dmsetup create $LOGICAL_VOL_NAME
