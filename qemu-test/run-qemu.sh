#!/bin/bash
# QEMU launch script for hugetlb swap testing

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Auto-detect kernel directory: use KERNEL_PATH env var, or detect relative to script
KERNEL_PATH="${KERNEL_PATH:-${SCRIPT_DIR}/../../..}"

# Default kernel path (can be overridden by command line argument)
DEFAULT_KERNEL="${KERNEL_PATH}/arch/arm64/boot/Image"
KERNEL="${1:-$DEFAULT_KERNEL}"
INITRD="${SCRIPT_DIR}/initrd.img"
DISK="${SCRIPT_DIR}/swap.qcow2"

# Default kernel if not provided
if [ ! -f "$KERNEL" ]; then
    # Try alternative locations
    ALT_KERNEL="${KERNEL_PATH}/arch/arm64/boot/Image.gz"
    if [ -f "$ALT_KERNEL" ]; then
        echo "Note: Using compressed kernel image (Image.gz)"
        KERNEL="$ALT_KERNEL"
    else
        echo "ERROR: Kernel image not found at $KERNEL"
        echo "Usage: $0 [path-to-Image]"
        echo "Default: ${DEFAULT_KERNEL}"
        echo ""
        echo "Set KERNEL_PATH environment variable to specify kernel source location:"
        echo "  KERNEL_PATH=/path/to/kernel $0"
        exit 1
    fi
fi

if [ ! -f "$INITRD" ]; then
    echo "ERROR: Initramfs not found at $INITRD"
    echo "Run: make initrd  (to build initrd.img)"
    exit 1
fi

# Check for disk image - use NVMe SSD interface
DISK_OPT=""
if [ -f "$DISK" ]; then
    DISK_OPT="-blockdev driver=qcow2,file.driver=file,file.filename=$DISK,node-name=ssd0,discard=unmap,file.discard=unmap,file.locking=off "
    DISK_OPT+="-device nvme,drive=ssd0,serial=nvme-ssd-0"
fi

# Memory configuration - 1GB for testing
VM_MEMORY="1G"

# Huge page configuration
# Use memfd backend without hugetlb (let kernel handle hugetlb inside guest)
MEMORY_BACKEND="-object memory-backend-memfd,id=mem,size=$VM_MEMORY,hugetlb=off,share=off"
MACHINE="virt,memory-backend=mem"
HUGETLB_STATUS="standard memory (hugetlb managed by guest)"

# CPU configuration
if [ "${QEMU_KVM:-}" = "1" ]; then
    CPU="host"
    KVM_OPTS="-enable-kvm"
    ACCEL_STATUS="KVM"
else
    CPU="cortex-a72"
    KVM_OPTS=""
    ACCEL_STATUS="TCG"
fi
SMP="8"

# Graphics (serial only for testing)
CONSOLE="-nographic -serial mon:stdio"

# Additional options
OPTIONS="-no-reboot"

# Debug mode: enable GDB stub and wait for connection
if [ -n "$QEMU_DEBUG" ]; then
    DEBUG_OPTS="-s -S"
    echo "  DEBUG: GDB stub enabled on port 1234"
    echo "  DEBUG: Waiting for GDB connection before starting..."
else
    DEBUG_OPTS=""
fi

# Determine auto-test mode
AUTO_TEST_FLAG=""
if [ "${AUTO_TEST:-}" = "1" ]; then
    AUTO_TEST_FLAG=" auto_test"
fi

# KAE PCI Passthrough (hisi_zip)
KAE_OPTS=""
if [ -n "${KAE:-}" ]; then
    if [ ! -e "/sys/bus/pci/devices/0000:${KAE}" ]; then
        echo "ERROR: KAE device ${KAE} not found"
        exit 1
    fi
    KAE_OPTS="-device vfio-pci,host=${KAE}"
fi

echo "=========================================="
echo "QEMU Hugetlb Swap Test Environment"
echo "=========================================="
echo "  Kernel: $KERNEL"
echo "  Initrd: $INITRD"
if [ -n "$DISK_OPT" ]; then
    echo "  Disk:   $DISK (512MB NVMe SSD swap)"
fi
echo "  Memory: $VM_MEMORY ($HUGETLB_STATUS)"
echo "  CPUs: $SMP"
echo "  Accelerator: $ACCEL_STATUS"
if [ -n "${KAE:-}" ]; then
    echo "  KAE: PCI passthrough $KAE"
fi
echo "  Auto-test: ${AUTO_TEST:-0}"
if [ -n "$QEMU_DEBUG" ]; then
    echo "  Debug: enabled (GDB port 1234)"
fi
echo ""
echo "Starting QEMU..."
echo "=========================================="

# QEMU binary - use QEMU env var or auto-detect
if [ -n "$QEMU" ]; then
    QEMU_BIN="$QEMU"
elif command -v qemu-system-aarch64 >/dev/null 2>&1; then
    QEMU_BIN="qemu-system-aarch64"
elif command -v qemu-kvm >/dev/null 2>&1; then
    QEMU_BIN="qemu-kvm"
elif [ -x "/root/github/qemu-10.2.1/build/qemu-system-aarch64" ]; then
    QEMU_BIN="/root/github/qemu-10.2.1/build/qemu-system-aarch64"
else
    echo "ERROR: qemu-system-aarch64 not found"
    echo "Set QEMU environment variable to specify the path:"
    echo "  QEMU=/path/to/qemu-system-aarch64 $0"
    exit 1
fi

# Launch QEMU
set +e

$QEMU_BIN \
    -machine $MACHINE \
    $MEMORY_BACKEND \
    $KVM_OPTS \
    -cpu $CPU \
    -smp $SMP \
    -m $VM_MEMORY \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -append "console=ttyAMA0 root=/dev/ram0 rw=1 init=/init loglevel=8${AUTO_TEST_FLAG}" \
    $DISK_OPT \
    $KAE_OPTS \
    $CONSOLE \
    $OPTIONS \
    $DEBUG_OPTS

exit $?
