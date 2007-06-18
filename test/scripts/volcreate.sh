#!/bin/sh

#set -x

# Default name if we create a new volume group
VGNAME=sysvg
# Name and size in megabytes of volume to be used for origin.
ORIGVOLNAME=ztestorig
ORIGVOLSIZE=2048
# Name and size in megabytes of volume to be used for snap store.
SNAPVOLNAME=ztestsnap
SNAPVOLSIZE=8192

usage()
{
	echo "Usage: $0 [-n <vgname>] [-O <origsize>] [-S <snapsize>] [-o <origin name>] [-s <snapshot name>]" 1>&2
	echo "Where <vgname> is the name of the volume group to use for the volumes,"
	echo "<origsize> is the desired size of the origin volume and <snapsize> is the"
	echo "desired size of the snapshot store, both in megabytes, and <origin name>"
	echo "and <snapshot name> are the names of the origin and snapshot volumes,"
	echo "respectively."
	exit 1
}

function find_storage_devices() {
	# Check for mounted or swap devices
	used_devices=`grep ^/dev /etc/mtab /etc/fstab | awk '{print $1}' | cut -d: -f2 | sort -u`
	echo $used_devices

	# Check for existing physical volumes that are in a volume group
	existing_pvs=`pvdisplay -c 2>/dev/null | awk -F: '$2 != "" {print $1}'`

	# Get list of partitions
	partitions=`tail -n+3 /proc/partitions | awk '{print "/dev/"$NF}'`

	devices=""
	# Try well known device names
	for device in $partitions /dev/sd{a,b,c,d,e,f} /dev/hd{a,b,c,d,e,f,g,h}; do
		[ -b $device ] && devices="$devices $device"
	done

	# Build a list of plausible disk devices
	usable_devices=""
	for device in $devices; do
		if echo $devices | egrep -qw "${device}[0-9]+"; then
			#echo $device is partitioned, don\'t use it
			continue
		elif echo $used_devices $existing_pvs | egrep -qw "$device"; then
			#echo $device is already used
			continue
		else
			#echo $device is usable
			usable_devices="$usable_devices $device"
		fi
	done

	if [ -z "$usable_devices" ]; then
		echo "No free disk devices found."
		return 1
	fi
	echo $usable_devices
	return 0
}

function extend_volume_group() {
	vg_name="$1"

	usable_devices=`find_storage_devices`
	if [ $? -ne 0 ]; then
		return 1
	fi
	extended=false
	for device in $usable_devices; do
		if pvcreate $device 2>/dev/null && vgextend "$vg_name" $device 2>/dev/null; then
			echo "Added $device to $vg_name."
			extended=true
		fi
	done

	# If the VG was extended at all, return 0 so we can retry the LV creation
	if $extended; then
		return 0
	else
		echo "Unable to extend $vg_name"
		return 1
	fi
}

function create_new_vg() {
	vg_name="$1"
	usable_devices=`find_storage_devices`
	if [ $? -ne 0 ]; then
		return 1
	fi
	for device in $usable_devices; do
		pvcreate $device 2>/dev/null
		if [ $? -eq 0 ]; then
			vgcreate ${vg_name} $device 2>/dev/null
			echo "Created ${vg_name} with $device."
			return 0
		fi
	done
	return 1
}

function clean_ztest_volumes() {
	vg_name=$1
	origvol=$2
	snapvol=$3
	vgreduce --removemissing ${vg_name} 2>/dev/null
	lvremove -f /dev/${vg_name}/$origvol 2>/dev/null
	lvremove -f /dev/${vg_name}/$snapvol 2>/dev/null
}

function create_ztest_volumes() {
	vg_name=$1
	origvol=$2
	snapvol=$3
	origsize=$4
	snapsize=$5
	space_needed=$(( $origsize + $snapsize ))
	clean_ztest_volumes ${vg_name} ${origvol} ${snapvol}
	# Get the volume group with the most free space
	# (PE size (field 13) * Free PE (field 16) = free space in kB)
	vg_info=`vgdisplay -c 2>/dev/null| awk -F: '{print $13*$16" "$1}' | sort -n | tail -1`
	if [ "$vg_info" = "" ]; then
		if create_new_vg ${vg_name}; then
			create_ztest_volumes ${vg_name} ${origvol} ${snapvol} ${origsize} ${snapsize}
		fi
		return $?
	fi
	set $vg_info
	vg_freespace="$1"
	vgname=${vg_name}
	vg_name="$2"
	if [ "${vg_name}" != "${vgname}" ]; then
		echo "Found VG ${vg_name}, using it rather than ${vgname}."
	fi
	if [ "$vg_freespace" -lt "$space_needed" ]; then
		echo "Not enough space in $vg_name. Attempting to extend it."
		if extend_volume_group ${vg_name}; then
			# if volumes were actually added, we'll try again
			create_ztest_volumes ${vg_name} ${origvol} ${snapvol} ${origsize} ${snapsize}
			return $?
		fi
	fi

	# Create logical volumes
	lvcreate -L ${origsize}k -n ${origvol} ${vg_name} 2>/dev/null
	lvcreate -L ${snapsize}k -n ${snapvol} ${vg_name} 2>/dev/null
	return 0
}

while getopts "n:O:S:o:s:" option ; do
	case "$option" in
	n)	VGNAME="$OPTARG";;
	O)	ORIGVOLSIZE="$OPTARG";;
	S)	SNAPVOLSIZE="$OPTARG";;
	o)	ORIGVOLNAME="$OPTARG";;
	s)	SNAPVOLNAME="$OPTARG";;
	*)	usage;;
	esac
done
shift $(($OPTIND - 1))
if [ $# -ge 1 ]; then
	usage
fi
#
# Get the sizes as kilobytes.
#
ORIGVOLSIZE=$(( ${ORIGVOLSIZE} * 1024 ))
SNAPVOLSIZE=$(( ${SNAPVOLSIZE} * 1024 ))

create_ztest_volumes ${VGNAME} ${ORIGVOLNAME} ${SNAPVOLNAME} ${ORIGVOLSIZE} ${SNAPVOLSIZE}

if [ ! -e /dev/${VGNAME}/${ORIGVOLNAME} -o ! -e /dev/${VGNAME}/${SNAPVOLNAME} ]; then
	exit 5
fi

exit 0
