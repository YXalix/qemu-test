# QEMU Hugetlb Swap Test Environment

This directory contains a complete QEMU-based test environment with **both KUnit (in-kernel) and E2E tests** for the Hugetlb Swap feature on Linux 6.6 ARM64.

## Quick Start

### Using Makefile (Recommended)

```bash
cd /root/kernel/testing

# Build all images from scratch
make build-all

# Start QEMU VM (interactive mode, runs until Ctrl+C)
make qemu

# Debug kernel with GDB (QEMU waits for GDB connection on port 1234)
make qemu-debug

# Run automated tests with timeout (for CI/testing)
make qemu-test QEMU_TIMEOUT=15

# Or combine them
make build-all qemu
```

### Makefile Targets

| Target | Description |
|--------|-------------|
| `make qemu` | Start QEMU VM interactively (runs until Ctrl+C) |
| `make qemu-debug` | Start QEMU with GDB stub for kernel debugging |
| `make qemu-test QEMU_TIMEOUT=15` | Run tests with 15 second timeout |
| `make build-all` | Build swap.img and initrd.img |
| `make build-swap` | Create swap.img (512MB, one-time) |
| `make initrd` | Rebuild initrd.img from rootfs/ |
| `make clean` | Remove all generated images |

### Variables

- `KERNEL_IMAGE` - Path to kernel image (default: `/root/kernel/arch/arm64/boot/Image`)
- `QEMU_TIMEOUT` - Timeout in seconds for `qemu-test` (default: 0 = no timeout)
- `AUTO_TEST` - Auto-run tests (1=enabled, 0=interactive, default: 1)

### Manual Commands

```bash
cd /root/kernel/testing/qemu-test
./run-qemu.sh
```

This will boot a minimal BusyBox-based VM with the compiled kernel.

### Build initrd from Scratch

**Prerequisite**: Build kernel modules first, only if the kernel was built without modules:

```bash
cd /root/kernel
make modules -j$(nproc)
```

Then build the initrd:

```bash
cd /root/kernel/testing/
make initrd
```

This script:
1. Downloads and builds BusyBox (statically linked)
2. Creates the rootfs directory structure
3. Copies kernel modules (zram.ko, etmem_scan.ko, etmem_swap.ko)
4. Copies test programs (test-hugetlb, test-etmem, test-stress, test-hugetlb-swap) from tests/
5. Copies init script
6. Compiles the C test program (if source available)
7. Builds the compressed initrd.img

## Directory Structure

```
testing/
├── Makefile                 # Quick start commands
├── Kconfig                  # Kernel config for tests
└── qemu-test/
    ├── run-qemu.sh          # QEMU launch script
    ├── build-initrd.sh      # Build initrd from scratch
    ├── init                   # VM init script (source)
    ├── tests/                 # Test programs (source)
    │   ├── test-hugetlb       # Basic hugetlb tests
    │   ├── test-etmem         # ETMEM interface tests
    │   ├── test-stress        # Memory pressure tests
    │   └── test-hugetlb-swap  # Comprehensive E2E tests
    ├── initrd.img             # Compressed initramfs (output)
    ├── swap.img               # Swap disk image (512MB, created once)
    ├── hugetlb_swap_test.c    # (legacy - KUnit tests now in mm/)
    ├── rootfs/                # Root filesystem (build artifact)
    │   ├── init               # Copied from ../init
    │   ├── bin/
    │   │   ├── busybox        # BusyBox binary
    │   │   ├── test-hugetlb   # Symlink to ../tests/test-hugetlb
    │   │   ├── test-etmem     # Symlink to ../tests/test-etmem
    │   │   ├── test-stress    # Symlink to ../tests/test-stress
    │   │   └── hugetlb_alloc  # Compiled C test program
    │   └── tests/             # Copied from ../tests/
    └── busybox/               # BusyBox build directory
        └── busybox-1.36.1/
```

## Creating the Disk Images

### Creating initrd.img (Initial RAM Filesystem)

The `initrd.img` is a compressed cpio archive containing the BusyBox-based root filesystem. Here's how it was created:

#### 1. Build BusyBox (Static)

```bash
# Download and extract BusyBox
cd /root/kernel/qemu-test/busybox
wget https://busybox.net/downloads/busybox-1.36.1.tar.bz2
tar -xjf busybox-1.36.1.tar.bz2
cd busybox-1.36.1

# Configure for static build
make defconfig
sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config

# Build
make -j$(nproc)
```

#### 2. Create Root Filesystem Structure

```bash
cd /root/kernel/qemu-test/rootfs

# Create directory structure
mkdir -p bin sbin lib lib64 usr/bin usr/sbin proc sys dev tmp mnt/huge \
         etc/init.d var/run root

# Copy BusyBox and create symlinks
cp /root/kernel/qemu-test/busybox/busybox-1.36.1/busybox bin/
bin/busybox --install -s bin/
```

#### 3. Add Custom Files

- **`init`** - The init script (copied from `../init`) that mounts filesystems, sets up swap, and starts the shell
- **`tests/test-hugetlb`** - Shell script for basic hugepage testing
- **`tests/test-etmem`** - Shell script for ETMEM interface testing
- **`tests/test-stress`** - Shell script for memory pressure testing
- **`bin/hugetlb_alloc`** - Compiled C test program (statically linked)

#### 4. Create the init Script

The `init` script performs these steps:
1. Mounts essential filesystems (proc, sysfs, devtmpfs, tmpfs)
2. Creates device directories and mounts devpts
3. Mounts hugetlbfs at `/mnt/huge`
4. Creates and enables a swap file (`/tmp/swapfile`)
5. Enables ETMEM kernel swap if available
6. Prints system information and starts a shell

#### 5. Build the Initramfs Archive

```bash
cd /root/kernel/qemu-test/rootfs

# Create cpio archive and compress with gzip
# Use -print0 and --null for proper handling of filenames with spaces
find . -print0 | cpio --null -o -H newc | gzip -9 > ../initrd.img
```

This creates `initrd.img` (~1.4MB), a self-contained root filesystem that boots into memory.

**Important**: Ensure all symlinks use relative paths (e.g., `bin/sh -> busybox` not absolute paths like `/root/kernel/.../busybox`).

---

### Creating swap.img (Swap Disk)

The `swap.img` provides additional swap space via a virtual disk image (one-time setup):

```bash
cd /root/kernel/testing/qemu-test

# Create a 512MB empty file (one-time setup)
dd if=/dev/zero of=swap.img bs=1M count=512

# Format as swap space
mkswap swap.img
```

Or use the Makefile (skips if already exists):
```bash
make build-swap
```

**Using the Swap Image in QEMU**

The `run-qemu.sh` script automatically attaches `swap.img` if it exists. Inside the VM, you can enable it as additional swap:

```bash
# Check if disk is detected
ls -la /dev/vda*

# Enable the swap disk
swapon /dev/vda

# Verify swap is active
cat /proc/swaps
```

The disk appears as `/dev/vda` (virtio block device). To make it persistent across boots, add to the init script:

```bash
# In /root/kernel/qemu-test/rootfs/init
if [ -e /dev/vda ]; then
    swapon /dev/vda 2>/dev/null || true
fi
```

**Note**: The init script primarily uses zram swap for simplicity. The `swap.img` provides persistent swap space if needed.

---


## Manual Testing

Inside the VM, you can also manually test:

```bash
# Check hugepage support
cat /proc/sys/vm/nr_hugepages
ls /sys/kernel/mm/hugepages/

# Reserve huge pages
echo 20 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Check ETMEM status
cat /sys/kernel/mm/etmem/kernel_swap_enable
cat /proc/sys/vm/etmem_reclaim

# Enable etmem reclaim
echo 1 > /proc/sys/vm/etmem_reclaim

# Check swap status
cat /proc/swaps
cat /proc/vmstat | grep swap

# Allocate huge pages via hugetlbfs
dd if=/dev/zero of=/mnt/huge/testfile bs=2M count=1

# Monitor memory
cat /proc/meminfo | grep -E "(Huge|Swap)"
```

## Rebuilding

### Edit and Rebuild
The `init` script and `tests/` are source files outside `rootfs/`. To modify and test:

```bash
cd /root/kernel/testing/qemu-test

# 1. Edit init or test scripts
vim init                    # Edit VM init script
vim tests/test-hugetlb  # Edit test script

# 2. Rebuild initrd
./build-initrd.sh

# 3. Run QEMU
./run-qemu.sh
```

The `rootfs/` directory is a build artifact that gets regenerated each time you run `build-initrd.sh`.

### Rebuild C Test Program
If you modify `hugetlb_swap_test.c`:

```bash
gcc -static -O2 -o /root/kernel/qemu-test/rootfs/bin/hugetlb_alloc \
    /root/kernel/qemu-test/hugetlb_swap_test.c
```

### Rebuild BusyBox
If needed:

```bash
cd /root/kernel/testing/qemu-test/busybox/busybox-1.36.1
make -j$(nproc)
# Then rebuild initrd to include the new binary
./build-initrd.sh
```

## Kernel Requirements

The kernel must be built with:
- `CONFIG_HUGETLBFS=y`
- `CONFIG_HUGETLB_PAGE=y`
- `CONFIG_HUGETLB_SWAP=y` (the feature being tested)
- `CONFIG_ETMEM=y` (for user-triggered swap)
- `CONFIG_SWAP=y`

## Troubleshooting

### Kernel panic - not syncing: No working init
The init script may have issues. Check:
- `/root/kernel/testing/qemu-test/init` exists and is executable
- Rebuild initrd: `./build-initrd.sh`
- The init script has proper shebang (`#!/bin/sh`)

### Huge pages not available
Check kernel config:
```bash
grep CONFIG_HUGETLB /root/kernel/.config
```

### ETMEM not available
ETMEM may be built as a module. Check:
```bash
ls /root/kernel/mm/etmem.c  # Should exist
```

### QEMU won't start
Check that the kernel image exists:
```bash
ls -la /root/kernel/arch/arm64/boot/Image*
```

## Auto-Test Mode

When running `make qemu-test QEMU_TIMEOUT=15`, the VM will:
1. Boot with `auto_test` flag in kernel command line
2. Automatically run all test programs in `/tests/`
3. Display results (PASSED/FAILED for each test)
4. Power off the VM

Tests run automatically:
- `test-hugetlb` - Basic hugepage allocation test
- `test-etmem` - ETMEM interface test
- `test-stress` - Memory pressure test

For interactive mode with a shell:
```bash
make qemu
```

Or with timeout but interactive:
```bash
make qemu-test QEMU_TIMEOUT=60 AUTO_TEST=0
```

## Kernel Debugging with GDB

The test environment supports kernel debugging via QEMU's GDB stub.

### Quick Start

Terminal 1 - Start QEMU with debug mode:
```bash
make qemu-debug
```

Terminal 2 - Connect with GDB:
```bash
cd /root/kernel
gdb-multiarch vmlinux -ex 'target remote :1234'
```

## Exiting QEMU

### Interactive Mode (`make qemu`)

In the QEMU monitor (Ctrl+A then C), type:
- `quit` to exit
- `reboot` to restart

Or in the VM console:
- Press Ctrl+A then X to exit

### Auto-Test Mode (`make qemu-test`)

The VM powers off automatically after tests complete. The Makefile will:
- Print "SUCCESS: QEMU test completed" if all tests pass
- Print "ERROR: QEMU timed out" if timeout is exceeded
- Exit with non-zero code on failure
