#!/bin/bash
# QEMU launch script for hugetlb swap testing

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL="${1:-/root/kernel/arch/arm64/boot/Image}"
INITRD="${SCRIPT_DIR}/initrd.img"
DISK="${SCRIPT_DIR}/swap.img"

# Default kernel if not provided
if [ ! -f "$KERNEL" ]; then
    # Try alternative locations
    if [ -f "/root/kernel/arch/arm64/boot/Image.gz" ]; then
        echo "Note: Using compressed kernel image (Image.gz)"
        KERNEL="/root/kernel/arch/arm64/boot/Image.gz"
    else
        echo "ERROR: Kernel image not found at $KERNEL"
        echo "Usage: $0 [path-to-Image]"
        echo "Default: /root/kernel/arch/arm64/boot/Image"
        exit 1
    fi
fi

if [ ! -f "$INITRD" ]; then
    echo "ERROR: Initramfs not found at $INITRD"
    echo "Run: make initrd  (to build initrd.img)"
    exit 1
fi

# Check for disk image
DISK_OPT=""
if [ -f "$DISK" ]; then
    DISK_OPT="-drive file=$DISK,format=raw,if=virtio"
fi

# Memory configuration - 1GB for testing
VM_MEMORY="2G"

# QEMU machine type
MACHINE="virt"

# CPU configuration
CPU="cortex-a72"
SMP="1"

# Graphics (serial only for testing)
CONSOLE="-nographic -serial mon:stdio"

# Additional options
OPTIONS="-no-reboot"

# Determine auto-test mode
AUTO_TEST_FLAG=""
if [ "${AUTO_TEST:-}" = "1" ]; then
    AUTO_TEST_FLAG=" auto_test"
fi

echo "=========================================="
echo "QEMU Hugetlb Swap Test Environment"
echo "=========================================="
echo "  Kernel: $KERNEL"
echo "  Initrd: $INITRD"
if [ -n "$DISK_OPT" ]; then
    echo "  Disk:   $DISK (512MB swap)"
fi
echo "  Memory: $VM_MEMORY"
echo "  CPUs: $SMP"
echo "  Auto-test: ${AUTO_TEST:-0}"
echo ""
echo "Starting QEMU..."
echo "=========================================="

# Launch QEMU
set +e
/root/github/qemu-10.2.1/build/qemu-system-aarch64 \
    -machine $MACHINE \
    -cpu $CPU \
    -smp $SMP \
    -m $VM_MEMORY \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -append "console=ttyAMA0 root=/dev/ram0 rw=1 init=/init loglevel=8${AUTO_TEST_FLAG}" \
    $DISK_OPT \
    $CONSOLE \
    $OPTIONS

EXIT_CODE=$?
set -e

# Exit message
echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo "QEMU exited successfully"
else
    echo "QEMU exited with code $EXIT_CODE"
fi

exit $EXIT_CODE
