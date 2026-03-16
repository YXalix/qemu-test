#!/bin/bash
# Build initrd.img from scratch

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOTFS_DIR="${SCRIPT_DIR}/rootfs"
INITRD_FILE="${SCRIPT_DIR}/initrd.img"
BUSYBOX_VERSION="1.36.1"
BUSYBOX_DIR="${SCRIPT_DIR}/busybox/busybox-${BUSYBOX_VERSION}"

# Auto-detect kernel directory: use KERNEL_PATH env var, or detect relative to script
KERNEL_PATH="${KERNEL_PATH:-${SCRIPT_DIR}/../..}"

# Verify kernel directory exists
if [ ! -d "${KERNEL_PATH}/arch/arm64" ]; then
    echo "ERROR: Cannot find kernel source directory at ${KERNEL_PATH}"
    echo "Set KERNEL_PATH environment variable to specify the location:"
    echo "  KERNEL_PATH=/path/to/kernel $0"
    exit 1
fi

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

# Copy kernel module to lib/modules if found
# Usage: copy_module <module_name>
copy_module() {
    local mod="$1"
    local ko_file

    ko_file="$(find ${KERNEL_PATH} -name "${mod}.ko" -print -quit 2>/dev/null)"

    # Fallback to tests directory
    if [ ! -f "$ko_file" ]; then
        ko_file="${SCRIPT_DIR}/tests/${mod}.ko"
    fi

    if [ -f "$ko_file" ]; then
        cp "$ko_file" lib/modules/
        echo "  Module: ${mod}.ko"
    fi
}

# Copy kernel modules
echo "Copying kernel modules..."
mkdir -p lib/modules

# Copy modules by category
copy_modules() {
    for mod in "$@"; do
        copy_module "$mod"
    done
}

# Core modules
copy_modules zram etmem_scan etmem_swap

# Virtio drivers
copy_modules virtio virtio_ring virtio_blk virtio_pci_modern_dev virtio_pci_legacy_dev virtio_pci virtio_mmio

# KAE (Kunpeng Acceleration Engine) modules - hisi_zip only
copy_modules uacce hisi_qm hisi_zip

# Build and copy tests
echo "Building tests..."
mkdir -p tests
if [ -d "${SCRIPT_DIR}/tests" ]; then
    cd "${SCRIPT_DIR}/tests"
    # Build using Makefile (which wraps CMake)
    make -s 2>/dev/null || true
    cd "${ROOTFS_DIR}"

    # Copy from build/bin directory
    if [ -d "${SCRIPT_DIR}/tests/build/bin" ]; then
        for test_bin in "${SCRIPT_DIR}/tests/build/bin"/*; do
            if [ -f "$test_bin" ] && [ -x "$test_bin" ]; then
                cp "$test_bin" tests/
                chmod +x "tests/$(basename "$test_bin")"
                echo "  Test: $(basename "$test_bin")"
            fi
        done
    fi
fi

# Build initrd
echo "Creating initrd.img..."
cd "${ROOTFS_DIR}"
find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 > "${INITRD_FILE}"

SIZE=$(ls -lh "${INITRD_FILE}" | awk '{print $5}')
echo "Done: ${INITRD_FILE} (${SIZE})"
