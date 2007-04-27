#!/bin/sh

set -x

#
# Potentially modifiable parameters.
#
# Working directory, where images and ancillary files live.
WORKDIR=~/local
# Name of the main installed Qemu disk image.
TESTIMG=${WORKDIR}/test.img
# Names of the working replication source and target disk images.
SOURCEIMG=${WORKDIR}/source.img
TARGETIMG=${WORKDIR}/target.img
# Names of the test origin and snapshop store disk images for the source
# and target.
ZUMSRCIMG=${WORKDIR}/zumsource.img
ZUMTGTIMG=${WORKDIR}/zumtarget.img
# Name of target Zumastor support directory.  This must match the name in the
# preseed file!
ZUMADIR=zuma
# Where we put the "zumtest" link so it'll be run at boot.
ZRUNDIR=/etc/rc2.d
# Name of the zumastor volume we create for testing.
TESTVOL=testvol
# Name of the directory upon which we mount that volume.
ZTESTFS=ztestfs
# The command that actually runs the tests.
RUNTESTS=/zuma/zumastor/tests/runtests
# Name/IP address of replication source virtual machine.
ZSOURCENM=source
ZSOURCEIP=192.168.0.1
# Name/IP address of replication target virtual machine.
ZTARGETNM=target
ZTARGETIP=192.168.0.2

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
# Install 'tree' dependency as well as openssh-server
#
/usr/bin/apt-get -y install tree
/usr/bin/apt-get -y install openssh-server
#
# Install contents of CD zumastor directory, if any.
#
if [ -d /cdrom/zumastor ]; then
	cp -r /cdrom/zumastor /zuma
fi
#
# Stuff our IP/name definitions into /etc/hosts.
#
echo "${ZSOURCEIP}	${ZSOURCENM}" >>/etc/hosts
echo "${ZTARGETIP}	${ZTARGETNM}" >>/etc/hosts

#
# Set up ssh
#
mkdir /root/.ssh
cat sourcekey.pub targetkey.pub >/root/.ssh/authorized_keys

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

# Pull in source/target config from /dev/cdrom
cd /zuma
dd if=/dev/hdd of=config.sh
chmod 755 config.sh
#
# Source the source/target config so we know the mode in which we're running.
#
. ./config.sh

echo "	Configuring mode zmodevar"

if [ ! -b /dev/mapper/${TESTVOL}1 ]; then
	#
	# Configure our network appropriately.
	#
	echo "auto eth1" >>/etc/network/interfaces
	echo "iface eth1 inet static" >>/etc/network/interfaces
	if [ "zmodevar" = "source" ]; then
		echo "	address ${ZSOURCEIP}" >>/etc/network/interfaces
		hostname ${ZSOURCENM}
		echo ${ZSOURCENM} >/etc/hostname
	else
		echo "	address ${ZTARGETIP}" >>/etc/network/interfaces
		hostname ${ZTARGETNM}
		echo ${ZTARGETNM} >/etc/hostname
	fi
	echo "	netmask 255.255.255.0" >>/etc/network/interfaces
	ifdown eth1
	ifup eth1
	#
	# Set up our ssh keys.
	#
	if [ "zmodevar" = "source" ]; then
		cp sourcekey /root/.ssh/id_rsa
		cp sourcekey.pub /root/.ssh/id_rsa.pub
		cp targetkey.pub /root/.ssh/authorized_keys
		ssh-keyscan -t rsa target >/root/.ssh/known_hosts
	else
		cp targetkey /root/.ssh/id_rsa
		cp targetkey.pub /root/.ssh/id_rsa.pub
		cp sourcekey.pub /root/.ssh/authorized_keys
		ssh-keyscan -t rsa source >/root/.ssh/known_hosts
	fi
	#
	# Create the volumes
	#
	parted /dev/hdb mklabel msdos
	parted /dev/hdb mkpart primary 0 32768
	pvcreate /dev/hdb1
	vgcreate sysvg /dev/hdb1
	lvcreate --size 2g -n origstore1 sysvg
	lvcreate --size 8g -n snapstore1 sysvg
	lvcreate --size 2g -n origstore2 sysvg
	lvcreate --size 8g -n snapstore2 sysvg
	lvcreate --size 2g -n origstore3 sysvg
	lvcreate --size 8g -n snapstore3 sysvg
	echo -n "	Creating ${TESTVOL}1..."
	zumastor define volume -i ${TESTVOL}1 /dev/sysvg/origstore1 /dev/sysvg/snapstore1
	echo -n "${TESTVOL}2..."
	zumastor define volume -i ${TESTVOL}2 /dev/sysvg/origstore2 /dev/sysvg/snapstore2
	echo -n "${TESTVOL}3..."
	zumastor define volume -i ${TESTVOL}3 /dev/sysvg/origstore3 /dev/sysvg/snapstore3
	echo "done."
	if [ "zmodevar" = "source" ]; then
		echo -n "	Creating file systems..."
		mkfs.ext3 /dev/mapper/${TESTVOL}1 >/dev/null 2>&1
		mkfs.ext3 /dev/mapper/${TESTVOL}2 >/dev/null 2>&1
		mkfs.ext3 /dev/mapper/${TESTVOL}3 >/dev/null 2>&1
		echo "done."
		zumastor define master ${TESTVOL}1 -h 5
		zumastor define master ${TESTVOL}2 -h 5
		zumastor define master ${TESTVOL}3 -h 5
		mkdir -p /${ZTESTFS}1 /${ZTESTFS}2 /${ZTESTFS}3
		zumastor define target ${TESTVOL}1 ${ZTARGETNM}:11235 30
		zumastor define target ${TESTVOL}2 ${ZTARGETNM}:11236 30
		zumastor define target ${TESTVOL}3 ${ZTARGETNM}:11237 30
	else
		zumastor define source ${TESTVOL}1 ${ZSOURCENM} 600
		zumastor define source ${TESTVOL}2 ${ZSOURCENM} 600
		zumastor define source ${TESTVOL}3 ${ZSOURCENM} 600
		zumastor start source ${TESTVOL}1
		zumastor start source ${TESTVOL}2
		zumastor start source ${TESTVOL}3
	fi
fi

if [ "zmodevar" = "source" ]; then
	echo "	Mounting ${ZTESTFS}1, ${ZTESTFS}2, ${ZTESTFS}3."
	mount /dev/mapper/${TESTVOL}1 /${ZTESTFS}1
	mount /dev/mapper/${TESTVOL}2 /${ZTESTFS}2
	mount /dev/mapper/${TESTVOL}3 /${ZTESTFS}3
fi

if [ -n "${RUNTESTS}" -a -x ${RUNTESTS} ]; then
	${RUNTESTS} &
fi

exit 0

EOF_zumtest.sh
sed --in-place 's/zmodevar/\$\{ZMODE\}/' zumtest.sh

chmod 755 zumtest.sh
#
# Generate ssh keys for the source and target, if necessary.
#
if [ ! -f sourcekey ]; then
	ssh-keygen -t rsa -f sourcekey -N "" -C root@source
	ssh-keygen -t rsa -f targetkey -N "" -C root@target
fi
#
# Append all that to the tarfile.
#
tar -r -f ${debtar} zuminstall.sh zumtest.sh sourcekey* targetkey*

#
# Create the qemu disk image then boot qemu from our iso to do the install
#
qemu-img create -f qcow2 test.img 10G
qemu -no-reboot -hdb ${debtar} -cdrom ${instiso} -boot d ${TESTIMG}

rm ${debtar}

trap - 1 2 3 9

#
# These files are pointed at by the "hdd" virtual device to let the
# virtual machine know the mode in which it should run.  The extra comment
# lines are to pad it out to a reasonable size for a 'raw' disk image.
#
cat <<source-config_EOF >source-config.sh
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
ZMODE=source
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
source-config_EOF

cat <<target-config_EOF >target-config.sh
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
ZMODE=target
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
target-config_EOF
chmod 444 source-config.sh target-config.sh

#
# Build source and target disk images.
#
qemu-img create -f qcow2 ${ZUMSRCIMG} 32G
qemu-img create -f qcow2 ${ZUMTGTIMG} 32G
#
# Rename and clone the test disk image for our two virtual machines.
#
mv ${TESTIMG} ${SOURCEIMG}
cp ${SOURCEIMG} ${TARGETIMG}
#
# Now start the test.
#
rm -f qemu-source.pid qemu-target.pid
# Start the source instance...
qemu -pidfile qemu-source.pid -no-reboot -serial pty -m 512 -hdd source-config.sh -boot c -net nic,macaddr=00:e0:10:00:00:01 -net socket,vlan=1,listen=:3333 -hdb ${ZUMSRCIMG} ${SOURCEIMG} &
# ...wait a minute to let the source instance get going...
sleep 60
# ...and start the target instance.
qemu -pidfile qemu-target.pid -no-reboot -serial pty -m 512 -hdd target-config.sh -boot c -net nic,macaddr=00:e0:10:00:00:02 -net socket,vlan=1,connect=127.0.0.1:3333 -hdb ${ZUMTGTIMG} ${TARGETIMG} &

wait
exit 0

#ISO-START
