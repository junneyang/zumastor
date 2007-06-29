#!/bin/sh

#set -x

#
# Potentially modifiable parameters.
#
# Number of virtual images to create.
NUMIMAGES=1
# Working directory, where images and ancillary files live.
WORKDIR=~/local
# The CD image to install.
CDIMAGE=~/cd/test.iso
# The real generic zumastor installation script.
ZUMINSTALL=~/zumastor/test/scripts/zuminstall.sh
# Where the svn output goes.
SVNLOG=svn.log
# Suffix of disk image names.
IMGSUFFIX=.img
# Name of the main installed Qemu disk image.
DISKIMG=${WORKDIR}/qimage${IMGSUFFIX}
# Prefix of the name of the live image.  The image number is appended to this
# along with the suffix ".img", e.g. "qlive-1.img".
QLIVEPREFIX=qlive-
# Prefix of the volume to be used for Zumastor testing.
ZUMVOLUME=${WORKDIR}/zumavol-
# Name of the install directory.  This must match the name in the preseed file!
QINSTDIR=/zinst
# Where we put the "zstartup" link so it'll be run at boot.
QRUNDIR=/etc/rc2.d

#
# These are the list of packages upon which we may be dependent if we run
# Zumastor tests.
#
PACKAGEDEPS="tree dmsetup openssh-server make gcc libc6-dev"

#
# Where we can find the Zumastor package files.
#
PACKAGEDIR=""

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

usage()
{
	echo "Usage: $0 [-p <path>] [-i <image>] [-n <number of images>" 1>&2
	echo "Where <path> is a directory that contains the Zumastor .deb files,"
	echo "<image> is the CD image from which to install the virtual images and"
	echo "<number of images> is the number of images to create, by default one."
	exit 1
}

PACKAGEDIR=""
while getopts "i:n:p:" option ; do
	case "$option" in
	i)	CDIMAGE=${OPTARG};;
	n)	NUMIMAGES=${OPTARG};;
	p)	PACKAGEDIR="${OPTARG}";;
	*)	usage;;
	esac
done
shift $(($OPTIND - 1))
if [ $# -ge 1 ]; then
	usage
fi

echo "Building ${NUMIMAGES} qemu image(s)..."
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

set -x

cd /${QINSTDIR}
#
# Install our test startup script that will run when we reboot.
#
cp zstartup.sh /etc/init.d/zstartup
chmod 755 /etc/init.d/zstartup
ln -s ../init.d/zstartup ${QRUNDIR}/S99zstartup
#
# Install all potential dependencies.
#
/usr/bin/apt-get -y install ${PACKAGEDEPS}
#
# Set up ssh
#
mkdir -p /root/.ssh
cat hostkey.pub >>/root/.ssh/authorized_keys
#
# Set 'noapic' on the kernel(s) since qemu doesn't have one.
#
sed --in-place '/^kernel/s/$/ noapic/' /boot/grub/menu.lst >/dev/console 2>&1
#
# Add extra interfaces to /etc/network/interfaces
#
cat <<EOF_network >>/etc/network/interfaces
auto eth1
iface eth1 inet dhcp
auto eth2
iface eth2 inet dhcp

exit 0
EOF_zinstall.sh
chmod 755 zinstall.sh
#
# Create the test startup script.  This script runs at boot and performs any
# necessary post-installation setup, then removes itself.
#
cat <<EOF_zstartup >zstartup.sh
#!/bin/sh

#
# All we do here is send our configured IP address out our console.
#
echo "**********************************" >>/dev/ttyS0
echo "**********************************" >>/dev/ttyS0
ifconfig eth1 | grep "inet addr:" >>/dev/ttyS0
echo "**********************************" >>/dev/ttyS0
echo "**********************************" >>/dev/ttyS0
echo "**********************************" >>/dev/ttyS0

# Remove the startup script now that it has fulfilled its purpose.
rm /etc/init.d/zstartup
rm ${QRUNDIR}/S99zstartup

# Shut the virtual system down so we can do the next one.
#shutdown -hP now

exit 0
EOF_zstartup
chmod 755 zstartup.sh
zinstar=${WORKDIR}/zinstar-$$.tar
#
# If we've already built the master image, don't do it again.
#
if [ ! -f ${DISKIMG} ]; then
	trap "rm -f ${CDIMAGE} ${zinstar} ${DISKIMG}; exit 1" 1 2 3 9
	echo "Building master..."
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

#
# Now clone the base image, boot the new image and collect its IP address.
#
imagenum=1
while [ ${imagenum} -le ${NUMIMAGES} ]; do
	echo "Cloning image ${imagenum}..."
	# Clone the test disk image for our live instance.
	thisimg=${WORKDIR}/${QLIVEPREFIX}${imagenum}${IMGSUFFIX}
	thisname=${QLIVEPREFIX}${imagenum}
	cp ${DISKIMG} ${thisimg} &
	rm -f qemu-${imagenum}.pid qemu-${imagenum}.cons
	mkfifo qemu-${imagenum}.cons
	# Create the Zumastor test volume.
	zvol=${ZUMVOLUME}${imagenum}${IMGSUFFIX}
	qemu-img create -f qcow2 ${zvol} 32G
	# Wait for the test disk image copy to complete.
	wait
	#
	# Run qemu once to do the post-boot configuration and so we can get
	# the IP address of the virtual machine.  This one will try to reboot;
	# the "no-reboot" option means that it just shuts down.
	#
	echo '********** WARNING:  Running sudo.  You may have to type your password!'
	sudo qemu --pidfile qemu-${imagenum}.pid -serial pipe:qemu-${imagenum}.cons -m 512 -hdb ${zvol} -cdrom ${CDIMAGE} -boot c -net nic,macaddr=00:e0:10:00:00:0${imagenum} -net tap,ifname=eth${imagenum},script=${WORKDIR}/qemu-ifup.sh ${thisimg} &
	QIP=
	while [ -z "${QIP}" ]; do
		QIP=`dd if=qemu-${imagenum}.cons bs=80 count=2 | grep "inet addr:" | tail -1 | sed -e "s/	/ /g" | sed -e 's/^.*addr:\(.*\).*Bcast:.*$/\1/'`
	done
	echo "${QIP}	${thisname}"
	sleep 5
	#sh -x ${ZUMINSTALL} ${PACKAGEDIR} ${QIP}
#	wait
#	rm -f qemu-${imagenum}.pid
	#
	# Now run qemu for the last time, this time in the background.  We
	# leave this instance running after we ssh in and set its hostname.
	#
#	echo '********** WARNING:  Running sudo (again).  You may have to type your password!'
#	sudo qemu --pidfile qemu-${imagenum}.pid -serial pipe:qemu-${imagenum}.cons -m 512 -hdb ${zvol} -boot c -net nic,macaddr=00:e0:10:00:00:0${imagenum} -net tap,ifname=eth${imagenum},script=${WORKDIR}/qemu-ifup.sh ${thisimg} &
#	sleep 60
	disown -a
	grep -v ${QIP} ~/.ssh/known_hosts >/tmp/kn.$$
	cat /tmp/kn.$$ >~/.ssh/known_hosts
	rm /tmp/kn.$$
	ssh-keyscan ${QIP}
	hostname=`canonicalize_hostname ${QIP}`
	if [ "${hostname}" = "" ]; then
		hostname=${thisname}
	fi
	echo ${hostname} | ssh root@${QIP} "cat >/etc/hostname"
	echo ${hostname} | ssh root@${QIP} "cat >/etc/hostname"
	ssh root@${QIP} "hostname ${hostname}"
	echo "${QIP}	${hostname}" | ssh root@${QIP} "cat >>/etc/hosts"
	imagenum=`expr ${imagenum} + 1`
done

exit 0

#ISO-START
