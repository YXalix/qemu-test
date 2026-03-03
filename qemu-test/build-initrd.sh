#!/bin/bash
# Build initrd.img from scratch
# This script creates a complete initramfs with BusyBox, kernel modules, and test tools

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOTFS_DIR="${SCRIPT_DIR}/rootfs"
INITRD_FILE="${SCRIPT_DIR}/initrd.img"
BUSYBOX_VERSION="1.36.1"
BUSYBOX_DIR="${SCRIPT_DIR}/busybox/busybox-${BUSYBOX_VERSION}"
KERNEL_DIR="${KERNEL_DIR:-/root/kernel}"

echo "=========================================="
echo "Building initrd.img from scratch"
echo "=========================================="
echo ""

# Create rootfs directory structure
echo "Creating rootfs directory structure..."
rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}"
cd "${ROOTFS_DIR}"

# Create standard directories
mkdir -p bin sbin lib lib64 usr/bin usr/sbin proc sys dev tmp mnt/huge \
         etc/init.d var/run root

# Build or copy BusyBox
echo ""
echo "Setting up BusyBox..."
if [ -f "${BUSYBOX_DIR}/busybox" ]; then
    echo "  Using existing BusyBox binary"
    cp "${BUSYBOX_DIR}/busybox" bin/
else
    echo "  Building BusyBox from source..."
    mkdir -p "${SCRIPT_DIR}/busybox"
    cd "${SCRIPT_DIR}/busybox"

    # Download BusyBox if not present
    if [ ! -f "busybox-${BUSYBOX_VERSION}.tar.bz2" ]; then
        echo "  Downloading BusyBox ${BUSYBOX_VERSION}..."
        wget -q "https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2"
    fi

    # Extract
    if [ ! -d "busybox-${BUSYBOX_VERSION}" ]; then
        echo "  Extracting..."
        tar -xjf "busybox-${BUSYBOX_VERSION}.tar.bz2"
    fi

    # Build
    cd "busybox-${BUSYBOX_VERSION}"
    echo "  Configuring for static build..."
    make defconfig
    sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    echo "  Building..."
    make -j$(nproc)

    # Copy to rootfs
    cp busybox "${ROOTFS_DIR}/bin/"
    cd "${ROOTFS_DIR}"
fi

# Create BusyBox symlinks
echo "  Creating symlinks..."
cd bin
./busybox --install -s .
# Convert absolute symlinks to relative (needed for initramfs)
for link in $(find . -type l); do
    target=$(readlink "$link")
    if [[ "$target" == /* ]]; then
        # Convert absolute path to relative
        rel_target=$(basename "$target")
        ln -sf "$rel_target" "$link"
    fi
done
cd ..

# Copy init script
echo ""
echo "Copying init script..."
cp "${SCRIPT_DIR}/init" .
chmod +x init

# Copy kernel modules
echo ""
echo "Copying kernel modules..."
mkdir -p lib/modules

# ZRAM module
if [ -f "${KERNEL_DIR}/drivers/block/zram/zram.ko" ]; then
    cp "${KERNEL_DIR}/drivers/block/zram/zram.ko" lib/modules/
    echo "  zram.ko"
fi

# ETMEM modules
if [ -f "${KERNEL_DIR}/fs/proc/etmem_scan.ko" ]; then
    cp "${KERNEL_DIR}/fs/proc/etmem_scan.ko" lib/modules/
    echo "  etmem_scan.ko"
fi
if [ -f "${KERNEL_DIR}/fs/proc/etmem_swap.ko" ]; then
    cp "${KERNEL_DIR}/fs/proc/etmem_swap.ko" lib/modules/
    echo "  etmem_swap.ko"
fi

# Copy test scripts
echo ""
echo "Copying test scripts..."
mkdir -p test-scripts
for script in "${SCRIPT_DIR}/test-scripts"/test-*; do
    if [ -f "$script" ]; then
        cp "$script" test-scripts/
        chmod +x "test-scripts/$(basename $script)"
        echo "  $(basename $script)"
    fi
done

# Create symlinks in bin for easy access
cd bin
for script in ../test-scripts/test-*; do
    if [ -f "$script" ]; then
        ln -sf "../test-scripts/$(basename $script)" "$(basename $script)"
    fi
done
cd ..

# Create C test program if source exists
if [ -f "${SCRIPT_DIR}/hugetlb_swap_test.c" ]; then
    echo ""
    echo "Compiling C test program..."
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        aarch64-linux-gnu-gcc -static -O2 -o bin/hugetlb_alloc "${SCRIPT_DIR}/hugetlb_swap_test.c"
        echo "  Compiled with aarch64-linux-gnu-gcc"
    elif command -v gcc >/dev/null 2>&1; then
        gcc -static -O2 -o bin/hugetlb_alloc "${SCRIPT_DIR}/hugetlb_swap_test.c"
        echo "  Compiled with gcc"
    else
        echo "  WARNING: No compiler found, skipping C test program"
    fi
else
    echo "  WARNING: hugetlb_swap_test.c not found"
fi

# Build initrd.img
echo ""
echo "Building initrd.img..."
cd "${ROOTFS_DIR}"

# Remove any stale initrd.img in rootfs
rm -f initrd.img

# Create cpio archive
find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 > "${INITRD_FILE}"

# Report
SIZE=$(ls -lh "${INITRD_FILE}" | awk '{print $5}')
echo ""
echo "=========================================="
echo "initrd.img built successfully!"
echo "Size: ${SIZE}"
echo "Location: ${INITRD_FILE}"
echo "=========================================="
