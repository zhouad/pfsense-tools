#!/bin/sh

# pfSense master builder script
# (C)2005-2006 Scott Ullrich and the pfSense project
# All rights reserved.
#
# $Id$

set -e -u

# Suck in local vars
. ./pfsense_local.sh

# Suck in script helper functions
. ./builder_common.sh

# If a full build has been performed we need to nuke
# /usr/obj.pfSense/ since embedded uses a different
# make.conf
if [ -f /usr/obj.pfSense/pfSense.6.world.done ]; then
	echo -n "Removing /usr/obj* since full build performed prior..."
	rm -rf /usr/obj*
	echo "done."
fi

# Checkout a fresh copy from pfsense cvs depot
update_cvs_depot

# Calculate versions
export version_kernel=`cat $CVS_CO_DIR/etc/version_kernel`
version_base=`cat $CVS_CO_DIR/etc/version_base`
version=`cat $CVS_CO_DIR/etc/version`

cd $CVS_CO_DIR

# Nuke the boot directory
[ -d "${CVS_CO_DIR}/boot" ] && rm -rf ${CVS_CO_DIR}/boot

rm -f conf/packages

set +e # grep could fail
(cd /var/db/pkg && ls | grep bsdinstaller) > conf/packages
(cd /var/db/pkg && ls | grep cpdup) >> conf/packages
set -e

create_pfSense_Small_update_tarball

# Use pfSense_wrap.6 as kernel configuration file
export KERNELCONF=${KERNELCONF:-${PWD}/conf/pfSense_wrap.6}
export NO_COMPRESSEDFS=yes
export PRUNE_LIST="${PWD}/remove.list"

# Use embedded make.conf
export MAKE_CONF="${PWD}/conf/make.conf.embedded"

# Clean out directories
freesbie_make cleandir

# Build if needed and install world and kernel
make_world_kernel

fixup_updates

create_pfSense_BaseSystem_Small_update_tarball
