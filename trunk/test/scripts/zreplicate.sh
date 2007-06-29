#!/bin/sh

#set -x

# User to ssh as on the source and target systems.
SSHUSER=root
# Where the test support scripts are located, so we can find volcreate,
# zinstalltet and zruntet.
SCRIPTS=~/zumastor/test/scripts

usage()
{
	echo "Usage: $0 [-s <script directory>] [-v <volume list>] <source> <target>" >&2
	echo "Where <script directory> is the directory that contains support scripts," >&2
	echo "<volume list> is a space-delimited list of volumes that should be replicated," >&2
	echo "<source> is the system containing the volume(s) to be replicated, and" >&2
	echo "<target> is the system upon which to replicate the volume(s)." >&2
	exit 1
}

canonicalize_hostname()
{
	oname=$1
	set `host ${oname} | sed -e 's/\.$//'`
	if [ "$3" = "not" -a "$4" = "found:" ]; then
		echo ""
		return 1
	fi
	case "$2" in
	has)	cname=$1;;
	domain)	cname=$5;;
	*)	cname=$oname;;
	esac
	echo $cname
}

VOLLIST=
while getopts "s:v:" option ; do
	case "$option" in
	s)	SCRIPTS="$OPTARG";;
	v)	VOLLIST="$OPTARG";;
	*)	usage;;
	esac
done
shift $(($OPTIND - 1))
if [ $# -lt 2 ]; then
	usage
fi
SOURCE=$1
TARGET=$2
#
# Canonicalize the hostnames.
#
SOURCE=`canonicalize_hostname $1`
#
# Get the volumes defined on the source machine.
#
srcvols="`ssh ${SSHUSER}@${SOURCE} 'ls /var/lib/zumastor/volumes'`"
if [ $? -ne 0 ]; then
	echo "ssh ${SSHUSER}@${SOURCE} failed!"
	exit 5
fi
#
# Get the volumes defined on the source machine.
#
targvols="`ssh ${SSHUSER}@${TARGET} 'ls /var/lib/zumastor/volumes'`"
if [ $? -ne 0 ]; then
	echo "ssh ${SSHUSER}@${TARGET} failed!"
	exit 5
fi
#
# Attempt to copy our public key into the authorized_keys file on the
# target.  We abort if this fails.
#
ssh ${SSHUSER}@${TARGET} "cat >>~/.ssh/authorized_keys" <~/.ssh/id_rsa.pub
if [ $? -ne 0 ]; then
	echo "\"ssh root@${TARGET}\" failed!"
	exit 5
fi
#
# Make sure the systems each have the others key.  They both have keys if they
# were installed with zuminstall.sh.
#
ssh ${SSHUSER}@${SOURCE} "cat ~/.ssh/id_rsa.pub" | ssh ${SSHUSER}@${TARGET} "cat >>~/.ssh/authorized_keys"
ssh ${SSHUSER}@${TARGET} "cat ~/.ssh/id_rsa.pub" | ssh ${SSHUSER}@${SOURCE} "cat >>~/.ssh/authorized_keys"
#
# Also make sure they have the host keys, just to avoid potential problems.
#
ssh ${SSHUSER}@${SOURCE} "ssh-keyscan ${TARGET}"
ssh ${SSHUSER}@${TARGET} "ssh-keyscan ${SOURCE}"
#
# Now replicate the volumes.
#
for vol in ${srcvols}; do
	origdev=`ssh ${SSHUSER}@${SOURCE} "readlink /var/lib/zumastor/volumes/${vol}/device/origin"`
	snapdev=`ssh ${SSHUSER}@${SOURCE} "readlink /var/lib/zumastor/volumes/${vol}/device/snapstore"`
	origsize=`ssh ${SSHUSER}@${SOURCE} "fdisk -l ${origdev}" | grep "^Disk " | cut -f3 -d\ `
	snapsize=`ssh ${SSHUSER}@${SOURCE} "fdisk -l ${snapdev}" | grep "^Disk " | cut -f3 -d\ `
	volsize=`expr ${origsize} + ${snapsize}`
	scp ${SCRIPTS}/volcreate.sh ${SSHUSER}@${TARGET}:/tmp
	voldevs=`ssh ${SSHUSER}@${TARGET} "sh -x /tmp/volcreate.sh -O ${origsize} -S ${snapsize}"`
	if [ $? -ne 0 ]; then
		echo "$0:  Can't create replication volumes."
		exit 5
	fi
	set ${voldevs}
	origdev=$1
	snapdev=$2
	ssh ${SSHUSER}@${TARGET} "/bin/zumastor define volume ${vol} ${origdev} ${snapdev} --initialize"
	ssh ${SSHUSER}@${TARGET} "/bin/zumastor define source ${vol} ${SOURCE} 600"
	ssh ${SSHUSER}@${SOURCE} "/bin/zumastor define target ${vol} ${TARGET}:11235 30"
	ssh ${SSHUSER}@${TARGET} "/bin/zumastor start source ${vol}"
done
