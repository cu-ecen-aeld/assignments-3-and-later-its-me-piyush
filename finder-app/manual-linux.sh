#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.
# Used LLM for this file: https://chatgpt.com/share/697feafb-6660-800d-8fdb-fa6d5a315488
set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

# Helper functions

die() {
    echo "ERROR: $*" >&2
    exit 1
}

copy_lib_to_rootfs() {
    local libname="$1"
    local sysroot="$2"
    local dest_rootfs="$3"

    # Find the first match in sysroot (lib or lib64)
    local src
    src=$(find "${sysroot}" -type f -name "${libname}" 2>/dev/null | head -n 1 || true)
    [ -n "${src}" ] || die "Could not find ${libname} in sysroot ${sysroot}"

    # Preserve lib vs lib64 placement based on src path
    if echo "${src}" | grep -q "/lib64/"; then
        mkdir -p "${dest_rootfs}/lib64"
        cp -a "${src}" "${dest_rootfs}/lib64/"
    else
        mkdir -p "${dest_rootfs}/lib"
        cp -a "${src}" "${dest_rootfs}/lib/"
    fi
}

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR} || die "Failed to create outdir ${OUTDIR}"

echo "OUTDIR is ${OUTDIR}"

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
    echo "Building kernel (ARCH=${ARCH})"
    make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" mrproper
    make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" defconfig
    make -j"$(nproc)" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" Image
fi

echo "Adding the Image in outdir"
cp -a "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" "${OUTDIR}/Image"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
mkdir -p "${OUTDIR}/rootfs"
mkdir -p "${OUTDIR}/rootfs"/{bin,dev,etc,home,lib,lib64,proc,sbin,sys,tmp,usr,var,tty}
mkdir -p "${OUTDIR}/rootfs/usr"/{bin,sbin,lib}
mkdir -p "${OUTDIR}/rootfs/var/log"

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # TODO:  Configure busybox
    make distclean
    make defconfig
else
    cd busybox
fi

# TODO: Make and install busybox
make -j"$(nproc)" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}"
make CONFIG_PREFIX="${OUTDIR}/rootfs" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "Shared library"

SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)
echo "SYSROOT is ${SYSROOT}"

# Copy interpreter (dynamic linker)
INTERP=$(${CROSS_COMPILE}readelf -a "${OUTDIR}/rootfs/bin/busybox" \
  | sed -n 's/.*program interpreter: \(\/[^]]*\).*/\1/p')

if [ -z "${INTERP}" ]; then
  die "Could not determine program interpreter for busybox"
fi

# First try the direct sysroot path (common toolchain layout)
if [ -e "${SYSROOT}${INTERP}" ]; then
  mkdir -p "${OUTDIR}/rootfs/$(dirname "${INTERP}")"
  cp -a "${SYSROOT}${INTERP}" "${OUTDIR}/rootfs/$(dirname "${INTERP}")/"
else
  # Fallback: search by basename
  INTERP_BASENAME="$(basename "${INTERP}")"
  INTERP_SRC=$(find "${SYSROOT}" -type f -name "${INTERP_BASENAME}" 2>/dev/null | head -n 1 || true)
  [ -n "${INTERP_SRC}" ] || die "Could not find interpreter ${INTERP} in sysroot ${SYSROOT}"

  mkdir -p "${OUTDIR}/rootfs/$(dirname "${INTERP}")"
  cp -a "${INTERP_SRC}" "${OUTDIR}/rootfs/$(dirname "${INTERP}")/"
fi



# Find and copy interpreter into the same path inside rootfs
INTERP_SRC=$(find "${SYSROOT}" -type f -path "*${INTERP}" 2>/dev/null | head -n 1 || true)
[ -n "${INTERP_SRC}" ] || die "Could not find interpreter ${INTERP} in sysroot"

mkdir -p "${OUTDIR}/rootfs/$(dirname "${INTERP}")"
cp -a "${INTERP_SRC}" "${OUTDIR}/rootfs/$(dirname "${INTERP}")/"

# Copy all shared libs busybox depends on
mapfile -t LIBS < <(${CROSS_COMPILE}readelf -a "${OUTDIR}/rootfs/bin/busybox" \
    | awk -F'[][]' '/Shared library/ {print $2}')

for lib in "${LIBS[@]}"; do
    copy_lib_to_rootfs "${lib}" "${SYSROOT}" "${OUTDIR}/rootfs"
done

# Also copy libc loader symlinks if needed (common autograder gotcha)
# Try to copy libc.so.6 explicitly if not already present in rootfs
if [ ! -e "${OUTDIR}/rootfs/lib/libc.so.6" ] && [ ! -e "${OUTDIR}/rootfs/lib64/libc.so.6" ]; then
    copy_lib_to_rootfs "libc.so.6" "${SYSROOT}" "${OUTDIR}/rootfs" || true
fi

# TODO: Add library dependencies to rootfs

# TODO: Make device nodes
sudo mknod -m 666 "${OUTDIR}/rootfs/dev/null" c 1 3 || true
sudo mknod -m 600 "${OUTDIR}/rootfs/dev/console" c 5 1 || true

# TODO: Clean and build the writer utility
cd "${FINDER_APP_DIR}"
make clean
make CROSS_COMPILE="${CROSS_COMPILE}"


# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
cp -a "${FINDER_APP_DIR}/writer" "${OUTDIR}/rootfs/home/"
mkdir -p "${OUTDIR}/rootfs/home/conf"


cp -a "${FINDER_APP_DIR}/finder.sh" "${OUTDIR}/rootfs/home/"
cp -a "${FINDER_APP_DIR}/finder-test.sh" "${OUTDIR}/rootfs/home/"
cp -a "${FINDER_APP_DIR}/autorun-qemu.sh" "${OUTDIR}/rootfs/home/"

cp -a "${FINDER_APP_DIR}/conf/username.txt" "${OUTDIR}/rootfs/home/conf/"
cp -a "${FINDER_APP_DIR}/conf/assignment.txt" "${OUTDIR}/rootfs/home/conf/"

# Patch finder-test.sh to use conf/assignment.txt (not ../conf/assignment.txt)
sed -i 's|\.\./conf/assignment\.txt|conf/assignment.txt|g' "${OUTDIR}/rootfs/home/finder-test.sh"

# Ensure scripts are executable
chmod +x "${OUTDIR}/rootfs/home/"*.sh || true
chmod +x "${OUTDIR}/rootfs/home/writer" || true


# TODO: Chown the root directory
sudo chown -R root:root "${OUTDIR}/rootfs"


# TODO: Create initramfs.cpio.gz
cd "${OUTDIR}/rootfs"
find . -print0 \
  | cpio --null -ov --format=newc --owner=root:root > "${OUTDIR}/initramfs.cpio"
gzip -f "${OUTDIR}/initramfs.cpio"
echo "Created ${OUTDIR}/initramfs.cpio.gz"
echo "Kernel Image: ${OUTDIR}/Image"