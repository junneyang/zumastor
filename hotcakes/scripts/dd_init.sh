#!/bin/bash 

# import all the sweet variables used throughout the ddsnap scripts
. ddsnap_env.sh

cd $DDSNAP_HOME

echo "Initializing devices locally and remotely"
./mkddsnap $SNAPSTORE_DEV $ORIGIN_DEV
ssh $REMOTE_HOST ${DDSNAP_HOME}/mkddsnap $SNAPSTORE_DEV $ORIGIN_DEV

