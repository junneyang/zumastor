#!/bin/sh

#
# Potentially modifiable parameters.
#
# Name of the main installed Qemu disk image.
TESTIMG=test.img
# Names of the test origin store and snapshot store Qemu disk images.
ORIGIMG=origstore.img
SNAPIMG=snapstore.img
# Name of target Zumastor support directory.  This must match the name in the
# preseed file!
ZUMADIR=zuma
# Where we put the "zumtest" link so it'll be run at boot.
ZRUNDIR=/etc/rc2.d
# Name of the zumastor volume we create for testing.
TESTVOL=testvol
# Name of the directory upon which we mount that volume.
ZTESTFS=ztestfs

usage()
{
	echo "Usage: $0 <path>" 1>&2
	echo "Where <path> is the directory that contains the Zumastor .deb files."
}

if [ $# -ne 1 ]; then
	usage
	#exit 1
fi
SPWD=`pwd`
#
# Extract the installation ISO from the script
#
instiso=${SPWD}/install-$$.iso
trap "rm -f ${instiso}; exit 1" 1 2 3 9
ISOSTART=`grep -n -m 1 "^ISO-START" $0 | cut -f1 -d:`
if [ ! -z "$ISOSTART" ]; then
	ISOSTART=$(expr $ISOSTART + 1)
	tail +${ISOSTART} $0 >${instiso}
else
	# For testing only
	instiso=~/cd/test.iso
fi
PACKAGEDIR=$1
if [ ! -d "$PACKAGEDIR" ]; then
	usage
	exit 1
fi
#
# Find the zumastor install packages
#
cd $PACKAGEDIR
KHDR=`ls kernel-headers-*.deb | tail -1`
KIMG=`ls kernel-image-*.deb | tail -1`
DDSN=`ls ddsnap*.deb | tail -1`
ZUMA=`ls zumastor*.deb | tail -1`
fail=0
if [ ! -f "${KHDR}" ]; then
	echo "No kernel-headers package found!"
	fail=1
fi
if [ ! -f "${KIMG}" ]; then
	echo "No kernel-image package found!"
	fail=1
fi
if [ ! -f "${DDSN}" ]; then
	echo "No ddsnap package found!"
	fail=1
fi
if [ ! -f "${ZUMA}" ]; then
	echo "No zumastor package found!"
	fail=1
fi
#
# Tar up the zumastor packages.
#
debtar=${SPWD}/debtar-$$.tar
trap "rm -f ${instiso} ${debtar} test.img; exit 1" 1 2 3 9
tar cf ${debtar} ${KHDR} ${KIMG} ${DDSN} ${ZUMA}
cd $SPWD
#
# Create the zumastor install script.
#
cat <<EOF_zuminstall.sh >zuminstall.sh
#!/bin/sh

cd /${ZUMADIR}
#
# Install the packages that have already been placed here by the bootstrap
# script and the preseed mechanism.
#
dpkg -i ${KHDR}
if [ $? -ne 0 ]; then
	echo "dpkg -i ${KHDR} failed: $?!"
	exit 1
fi
dpkg -i ${KIMG}
if [ $? -ne 0 ]; then
	echo "dpkg -i ${KIMG} failed: $?!"
	exit 1
fi
dpkg -i ${DDSN}
if [ $? -ne 0 ]; then
	echo "dpkg -i ${DDSN} failed: $?!"
	exit 1
fi
dpkg -i ${ZUMA}
if [ $? -ne 0 ]; then
	echo "dpkg -i ${ZUMA} failed: $?!"
	exit 1
fi
#
# Set 'noapic' on the zumastor kernel since qemu doesn't have one.
#
sed --in-place '/^kernel.*zumastor/s/$/ noapic/' /boot/grub/menu.lst
#
# Install our test startup script that will run when we reboot.
#
cp zumtest.sh /etc/init.d/zumtest
chmod 755 /etc/init.d/zumtest
ln -s ../init.d/zumtest ${ZRUNDIR}/S99zumtest
#
# Install 'tree' dependency
#
/usr/bin/apt-get install tree
#
# Install contents of CD zumastor directory, if any.
#
if [ -d /cdrom/zumastor ]; then
	cp -r /cdrom/zumastor /zuma
fi 
exit 0
EOF_zuminstall.sh
chmod 755 zuminstall.sh
#
# Create the zumastor test startup script.  This script gets run at boot and
# sets up the test environment.
#
cat <<EOF_zumtest.sh >zumtest.sh
#!/bin/sh

# Grab the init shell script functions
. /lib/lsb/init-functions

echo "Zumastor automated test lashup..."

zumastor status
zumastor define volume -i ${TESTVOL} /dev/hdb /dev/hdc
zumastor status
mkfs.ext3 /dev/mapper/${TESTVOL}
mkdir -p /${ZTESTFS}
mount /dev/mapper/${TESTVOL} /${ZTESTFS}

# if zumastor volume exists, assume we're running
# else set up zumastor volume
# run test...

EOF_zumtest.sh
chmod 755 zumtest.sh
#
# Append those scripts to the tarfile.
#
tar -r -f ${debtar} zuminstall.sh zumtest.sh
#
# Create the qemu disk image then boot qemu from our iso to do the install
#
qemu-img create -f qcow2 test.img 10G
qemu -no-reboot -hdb ${debtar} -cdrom ${instiso} -boot d ${TESTIMG}

trap - 1 2 3 9

#
# Build origin and snapstore disk images.
#
qemu-img create -f qcow2 ${ORIGIMG} 2G
qemu-img create -f qcow2 ${SNAPIMG} 2G
#
# Now start the test.
#
qemu -no-reboot -boot c -hdb ${ORIGIMG} -hdc ${SNAPIMG} ${TESTIMG}

exit 0

#ISO-START
