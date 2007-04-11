#!/bin/bash
# zumastor automated build script
# Shapor Naghibzadeh <shapor@google.com>

KERNEL_VERSION=2.6.19.1
BUILD_DIR=build # relative to current directory
LOG=/dev/null
TIME=`date +%s`

[[ -e $1 ]] || { echo "Usage: $0 <path_to_kernel_config>"; exit 1; }

mkdir $BUILD_DIR 2>/dev/null
cp $1 $BUILD_DIR/$KERNEL_VERSION.config
pushd $BUILD_DIR >> $LOG

echo -ne Getting zumastor sources from subversion ...
if [[ -e zumastor ]]; then
	svn update zumastor >> $LOG || exit $?
else
	svn checkout http://zumastor.googlecode.com/svn/trunk/ zumastor >> $LOG || exit $?
fi
SVNREV=`svn info zumastor | grep ^Revision:  | cut -d\  -f2`
VERSION=`cat zumastor/VERSION` || exit 1
echo -e "done.\n"

echo -n Getting kernel sources from kernel.org ...
# use -c to prevent re-downloading the same kernel tarball more than once
wget -c http://www.kernel.org/pub/linux/kernel/v2.6/linux-${KERNEL_VERSION}.tar.bz2 >> $LOG || exit $?
echo -e "done.\n"

echo -n Building zumastor Debian package...
pushd zumastor/zumastor >> $LOG || exit 1
dch -b -v $VERSION-r$SVNREV "revision $SVNREV" || exit 1
dpkg-buildpackage -rfakeroot >> $LOG || exit 1
popd >> $LOG
echo -e "done.\n"

echo -n Building ddsnap Debian package...
pushd zumastor/ddsnap >> $LOG || exit 1
dch -b -v $VERSION-r$SVNREV "revision $SVNREV" || exit 1
dpkg-buildpackage -rfakeroot >> $LOG || exit 1
popd >> $LOG
cp zumastor/*.deb .
echo -e "done.\n"

# this mv can pollute for filesystem with lots of kernel trees, but is safer than rm -rf
[[ -e linux-${KERNEL_VERSION} ]] && { \
	echo Moving old kernel tree to linux-${KERNEL_VERSION}.$TIME; \
	mv linux-${KERNEL_VERSION} linux-${KERNEL_VERSION}.$TIME; }

echo -n Unpacking kernel...
tar xjf linux-${KERNEL_VERSION}.tar.bz2 || exit 1
echo -e "done.\n"

echo -n "Setting .config ..."
mv $KERNEL_VERSION.config linux-${KERNEL_VERSION}/.config
echo -e "done.\n"

echo Applying patches...
pushd linux-${KERNEL_VERSION} >> $LOG || exit 1
for patch in ../zumastor/zumastor/patches/${KERNEL_VERSION}/* ../zumastor/ddsnap/patches/${KERNEL_VERSION}/*; do
	echo "   $patch"
	< $patch patch -p1 >> $LOG || exit 1
done
echo -e "done.\n"

echo -n Building kernel package...
fakeroot make-kpkg --append_to_version=-zumastor-r$SVNREV --revision=1.0 --initrd  --mkimage="mkinitramfs -o /boot/initrd.img-%s %s" --bzimage kernel_image kernel_headers >> $LOG || exit 1
popd >> $LOG
echo -e "done.\n"

echo zumastor packages successfully built in $BUILD_DIR/*.deb
