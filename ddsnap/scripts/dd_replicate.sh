#/bin/bash 

# import all the sweet variables used throughout the ddsnap scripts
. ddsnap_env.sh

if [ ! $# -eq 3  ]
then
   echo "usage: `basename $0` [-x|-r|-t] <older_snapshot_id> <newer_snapshot_id> "
   exit 1
fi

cd $DDSNAP_HOME

echo "Creating changelist file: /tmp/changelist${2}-${3}"
./${CREATE_CHANGELIST} $SERVER_SOCK_NAME /tmp/changelist${2}-${3} $2 $3
echo "Creating delta file: deltafile${2}-${3}"
./${CREATE_DELTA} $1 /tmp/changelist${2}-${3} /tmp/deltafile${2}-${3} /dev/mapper/${SNAPSHOT_VOL_NAME}$2 /dev/mapper/${SNAPSHOT_VOL_NAME}$3
#echo "Copying deltafile over to $REMOTE_HOST"
#scp /tmp/deltafile${2}-${3} root@${REMOTE_HOST}:/tmp/
#echo "Applying deltafile to $REMOTE_HOST"
#ssh -l root $REMOTE_HOST ${DDSNAP_HOME}/${APPLY_DELTA} /tmp/deltafile${2}-${3} /dev/mapper/${LOGICAL_VOL_NAME}

#echo "Creating snapshot $3 on remote host $REMOTE_HOST"
#ssh -l root $REMOTE_HOST ${DDSNAP_HOME}/${CREATE_SNAPSHOT} $SERVER_SOCK_NAME $3
#echo "Creating logical device ${SNAPSHOT_VOL_NAME}$3"
#ssh -l root $REMOTE_HOST "(echo 0 $SIZE_LOGICAL_VOL ddsnap $SNAPSTORE_DEV $ORIGIN_DEV $AGENT_SOCK_NAME $3 | dmsetup create ${SNAPSHOT_VOL_NAME}$3) &> /dev/null < /dev/null"
