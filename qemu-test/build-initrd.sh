#!/bin/bash
# Build initrd.img from scratch

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOTFS_DIR="${SCRIPT_DIR}/rootfs"
INITRD_FILE="${SCRIPT_DIR}/initrd.img"
BUSYBOX_VERSION="1.36.1"
BUSYBOX_DIR="${SCRIPT_DIR}/busybox/busybox-${BUSYBOX_VERSION}"
KERNEL_DIR="${KERNEL_DIR:-/root/kernel}"

echo "Building initrd.img..."

# Create rootfs directory
rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}"
cd "${ROOTFS_DIR}"

# Create directories
mkdir -p bin sbin lib lib64 usr/bin usr/sbin proc sys dev tmp mnt/huge \
         etc/init.d var/run root

# Build BusyBox if needed
if [ ! -f "${BUSYBOX_DIR}/busybox" ]; then
    echo "Building BusyBox..."
    mkdir -p "${SCRIPT_DIR}/busybox"
    cd "${SCRIPT_DIR}/busybox"

    if [ ! -f "busybox-${BUSYBOX_VERSION}.tar.bz2" ]; then
        wget -q "https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2"
    fi

    if [ ! -d "busybox-${BUSYBOX_VERSION}" ]; then
        tar -xjf "busybox-${BUSYBOX_VERSION}.tar.bz2"
    fi

    cd "busybox-${BUSYBOX_VERSION}"
    make defconfig
    sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    make -j$(nproc)
fi

# Copy BusyBox
cp "${BUSYBOX_DIR}/busybox" "${ROOTFS_DIR}/bin/"
cd "${ROOTFS_DIR}/bin"
./busybox --install -s .

# Fix symlinks
for link in $(find . -type l); do
    target=$(readlink "$link")
    if [[ "$target" == /* ]]; then
        ln -sf "$(basename "$target")" "$link"
    fi
done

cd "${ROOTFS_DIR}"

# Copy init script
cp "${SCRIPT_DIR}/init" .
chmod +x init

# Copy kernel modules
mkdir -p lib/modules
for mod in zram etmem_scan etmem_swap; do
    ko_file="${KERNEL_DIR}/$(find ${KERNEL_DIR} -name "${mod}.ko" -print -quit 2>/dev/null)"
    if [ -f "$ko_file" ]; then
        cp "$ko_file" lib/modules/
        echo "  Module: ${mod}.ko"
    fi
done

# Build and copy tests
echo "Building tests..."
mkdir -p tests
if [ -d "${SCRIPT_DIR}/tests" ]; then
    cd "${SCRIPT_DIR}/tests"
    make -s clean 2>/dev/null || true
    make -s 2>/dev/null || true
    cd "${ROOTFS_DIR}"

    for test_src in "${SCRIPT_DIR}/tests"/*.c; do
        if [ -f "$test_src" ]; then
            test_bin="$(basename "$test_src" .c)"
            if [ -f "${SCRIPT_DIR}/tests/${test_bin}" ]; then
                cp "${SCRIPT_DIR}/tests/${test_bin}" tests/
                chmod +x "tests/${test_bin}"
                echo "  Test: ${test_bin}"
            fi
        fi
    done
fi

# Build initrd
echo "Creating initrd.img..."
cd "${ROOTFS_DIR}"
find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 > "${INITRD_FILE}"

SIZE=$(ls -lh "${INITRD_FILE}" | awk '{print $5}')
echo "Done: ${INITRD_FILE} (${SIZE})"
