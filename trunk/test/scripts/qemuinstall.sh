#!/bin/sh

#set -x

#
# Potentially modifiable parameters.
#
# Working directory, where images and ancillary files live.
WORKDIR=~/local
# The CD image to install.
CDIMAGE=~/cd/test.iso
# The real generic zumastor installation script.
ZUMINSTALL=~/zumastor/test/scripts/zuminstall.sh
# Where the svn output goes.
SVNLOG=svn.log
# Name of the main installed Qemu disk image.
DISKIMG=${WORKDIR}/qimage.img
# Name of the volume to be used for Zumastor testing.
ZUMVOLUME=${WORKDIR}/zumavol.img
# Names of the test origin and snapshop store disk images for the source
# and target.
QLIVEIMG=${WORKDIR}/qlive.img
# Name of the install directory.  This must match the name in the preseed file!
QINSTDIR=/zinst
# Where we put the "zumtest" link so it'll be run at boot.
QRUNDIR=/etc/rc2.d

PACKAGEDIR=""

usage()
{
	echo "Usage: $0 [-p <path>] [-i <image>]" 1>&2
	echo "Where <path> is the directory that contains the Zumastor .deb files"
	echo "and <image> is the CD image to install."
	exit 1
}

PACKAGEDIR=""
while getopts "i:p:" option ; do
	case "$option" in
	i)	CDIMAGE=${OPTARG};;
	p)	PACKAGEDIR="${OPTARG}";;
	*)	usage;;
	esac
done
shift $(($OPTIND - 1))
if [ $# -ge 1 ]; then
	usage
fi

#
# If the CD image path doesn't start with a "/" then it's relative to
# the current directory.
#
if [ "${CDIMAGE:0:1}" != "/" ]; then
	CDIMAGE=`pwd`/${CDIMAGE}
fi
#
# If the package directory doesn't start with a "/" then it's also relative
# to the current directory.
#
if [ "${PACKAGEDIR}" != "" ]; then
	if [ "${PACKAGEDIR:0:1}" != "/" ]; then
		PACKAGEDIR=`pwd`/${PACKAGEDIR}
	fi
	PACKAGEDIR="-p ${PACKAGEDIR}"
fi

cd ${WORKDIR}
#
# Create our own qemu-ifup.sh script.
#
cat <<EOF_qemu-ifup.sh >qemu-ifup.sh
#!/bin/sh

echo "Executing \$0"
echo "Bringing up \$1 for bridged mode..."
sudo /sbin/ifconfig \$1 0.0.0.0 promisc up
echo "Adding \$1 to br0..."
sudo /usr/sbin/brctl addif br0 \$1
EOF_qemu-ifup.sh
chmod 755 qemu-ifup.sh
#
# Create the install script.
#
cat <<EOF_zinstall.sh >zinstall.sh
#!/bin/sh

cd /${QINSTDIR}
#
# Install our test startup script that will run when we reboot.
#
cp zstartup.sh /etc/init.d/zstartup
chmod 755 /etc/init.d/zstartup
ln -s ../init.d/zstartup ${QRUNDIR}/S99zstartup
#
# Install 'tree' dependency as well as openssh-server
#
/usr/bin/apt-get -y install tree
/usr/bin/apt-get -y install openssh-server

#
# Set up ssh
#
mkdir -p /root/.ssh
cat hostkey.pub >>/root/.ssh/authorized_keys

#
# Set 'noapic' on the kernel(s) since qemu doesn't have one.
#
sed --in-place '/^kernel/s/$/ noapic/' /boot/grub/menu.lst

exit 0
EOF_zinstall.sh
chmod 755 zinstall.sh
#
# Create the test startup script.  This script runs at boot, sucks in the
# zumastor test configuration script and runs it.
#
cat <<EOF_zstartup >zstartup.sh
#!/bin/sh

cd /${QINSTDIR}

#
# Pull in the test configuration script and any tests that might be included.
#
tar xf /dev/hdb
grep -q eth1 /etc/network/interfaces
#if [ \$? -ne 0 ]; then
	#
	# Configure our network appropriately.
	#
#	echo "auto eth1" >>/etc/network/interfaces
#	echo "iface eth1 inet dhcp" >>/etc/network/interfaces
#	ifdown eth1 >/dev/null 2>&1
#	ifup eth1
#fi
ifconfig eth0 | grep "inet addr:" >>/dev/ttyS0
echo "**********************************" >>/dev/ttyS0

# Remove the startup script now that it has fulfilled its purpose.
rm /etc/init.d/zstartup
rm ${QRUNDIR}/S99zstartup

exit 0
EOF_zstartup
chmod 755 zstartup.sh
zinstar=${WORKDIR}/zinstar-$$.tar
#
# If we've already built the master image, don't do it again.
#
if [ ! -f ${DISKIMG} ]; then
	trap "rm -f ${CDIMAGE} ${zinstar} ${DISKIMG}; exit 1" 1 2 3 9
	#
	# Generate (if necessary) and grab our ssh key to hand to the virtual
	# machine.
	#
	if [ ! -f ~/.ssh/id_rsa.pub ]; then
		ssh-keygen -t rsa -f ~/.ssh/id_rsa -N "" -C `whoami`@`hostname`
	fi
	cp ~/.ssh/id_rsa.pub hostkey.pub
	#
	# Stuff scripts and keys into a tarfile.
	#
	tar cf ${zinstar} zinstall.sh zstartup.sh hostkey.pub
	#
	# Create the qemu disk image then boot qemu from our iso to do the
	# install.
	#
	qemu-img create -f qcow2 ${DISKIMG} 10G
	qemu -no-reboot -hdb ${zinstar} -cdrom ${CDIMAGE} -boot d ${DISKIMG}
	
	rm ${zinstar}
fi

trap - 1 2 3 9

# Clone the test disk image for our live instance.
cp ${DISKIMG} ${QLIVEIMG} &
rm -f qemu.pid qemu.cons
# Prepare the tarfile and start the source instance...
tar cf zstart.tar zumcfg.sh
mkfifo qemu.cons
# Wait for the test disk image copy to complete.
wait
echo '********** WARNING:  Running sudo.  You may have to type your password!'
sudo qemu --pidfile qemu.pid -no-reboot -serial pipe:qemu.cons -m 512 -cdrom ${CDIMAGE} -hdb zstart.tar -boot c -net nic -net tap,ifname=eth1,script=${WORKDIR}/qemu-ifup.sh -net nic,vlan=1,macaddr=00:e0:10:00:00:01 -net socket,vlan=1,listen=:3333 ${QLIVEIMG} &
QIP=
while [ -z "${QIP}" ]; do
	QIP=`dd if=qemu.cons bs=80 count=2 | grep "inet addr:" | tail -1 | sed -e "s/	/ /g" | sed -e 's/^.*addr:\(.*\).*Bcast:.*$/\1/'`
	echo $QIP
done
sleep 5
rm -f qemu.pid
sh -x ${ZUMINSTALL} ${PACKAGEDIR} ${QIP}
# Create the Zumastor test volume.
qemu-img create -f qcow2 ${ZUMVOLUME} 32G
wait
echo '********** WARNING:  Running sudo (again).  You may have to type your password!'
sudo qemu --pidfile qemu.pid -no-reboot -m 512 -hdb ${ZUMVOLUME} -boot c -net nic -net tap,ifname=eth1,script=${WORKDIR}/qemu-ifup.sh -net nic,vlan=1,macaddr=00:e0:10:00:00:01 -net socket,vlan=1,listen=:3333 ${QLIVEIMG} &

exit 0

#ISO-START
