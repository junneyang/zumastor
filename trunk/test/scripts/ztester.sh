#!/bin/sh

#set -x

# Name of the volume group that has the origin and snap store volumes.
VGNAME=sysvg
# Name and size in megabytes of the origin volume.
ORIGNAME=ztestorig
ORIGSIZE=1024
# Name and size in megabytes of the snap store volume.
SNAPNAME=ztestsnap
SNAPSIZE=1024
# Name of the Zumastor volume.
TESTVOL=testvol
# Where the test support scripts are located, so we can find volcreate,
# zinstalltet and zruntet.
SCRIPTS=~/zumastor/test/scripts

usage()
{
	echo "Usage: $0 [-s <script directory>] [-t <test directory>] [-d <TET directory>] [-v <test volume>] <hostname/IP address>" 1>&2
	echo "Where <script directory> is the directory that contains support scripts,"
	echo "<test directory> is the directory that contains the TET-based tests,"
	echo "<TET directory> is the directory that contains the TET distribution,"
	echo "<test volume> is the name of the volume that will be used for testing and"
	echo "<hostname/IP address> identifies the system or systems upon which to install"
	echo "and run the tests."
	exit 1
}

TESTDIR="TETtests"
TETDIST="TET"
while getopts "d:s:t:v:" option ; do
	case "$option" in
	d)	TETDIST="$OPTARG";;
	s)	SCRIPTS="$OPTARG";;
	t)	TESTDIR="$OPTARG";;
	v)	TESTVOL="$OPTARG";;
	*)	usage;;
	esac
done
shift $(($OPTIND - 1))
if [ $# -lt 1 ]; then
	usage
fi
#
# If the either test or TET directory doesn't start with a "/" then it's
# relative to the current directory.
#
if [ "${TESTDIR:0:1}" != "/" ]; then
	TESTDIR=`pwd`/${TESTDIR}
fi
#
# If the TET directory doesn't start with a "/" then it's relative to
# the current directory.
#
if [ "${TETDIST}" != "" -a "${TETDIST:0:1}" != "/" ]; then
	TETDIST=`pwd`/${TETDIST}
fi
# We need both a test and TET distribution directory to run.
if [ ! -d ${TESTDIR} -o ! -d ${TETDIST} -o ! -d ${TETDIST}/src/tet3 ]; then
	usage
fi

for TARGET in $@
do
	#
	# Tar the tests and TET hierarchy to the target machine.
	#
	(cd ${TETDIST}; tar cf - . | ssh root@${TARGET} "cat >/tmp/tet.tar" )
	(cd ${TESTDIR}; tar cf - . | ssh root@${TARGET} "cat >/tmp/test.tar" )
	#
	# Copy our support scripts over.
	#
	scp ${SCRIPTS}/volcreate.sh root@${TARGET}:/tmp
	scp ${SCRIPTS}/zinstalltet.sh root@${TARGET}:/tmp
	scp ${SCRIPTS}/zruntet.sh root@${TARGET}:/tmp
	#
	# Create the test volumes.
	#
	ssh root@${TARGET} "sh /tmp/volcreate.sh -n ${VGNAME} -o ${ORIGNAME} -s ${SNAPNAME} -O ${ORIGSIZE} -S ${SNAPSIZE}"
	#
	# Run the zinstalltet script to install and build TET.
	#
	ssh root@${TARGET} "sh /tmp/zinstalltet.sh"
	#
	# Run the zruntet script to run the tests.
	#
	ssh root@${TARGET} "sh /tmp/zruntet.sh -n ${VGNAME} -o ${ORIGNAME} -s ${SNAPNAME} -v ${TESTVOL}"
done
