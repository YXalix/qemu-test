# QEMU E2E Testing Framework

A minimal QEMU-based testing framework for kernel features on ARM64.

## Quick Start

```bash
cd /root/kernel/testing

# Build initrd
make initrd

# Run interactively
make qemu

# Run with timeout (for CI)
make qemu-test QEMU_TIMEOUT=60

# Debug with GDB
make qemu-debug
```

## Makefile Targets

| Target | Description |
|--------|-------------|
| `make qemu` | Start QEMU VM interactively |
| `make qemu-kvm` | Start with KVM acceleration |
| `make qemu-debug` | Start with GDB stub (port 1234) |
| `make qemu-test` | Run with timeout for CI |
| `make disk` | Create disk.qcow2 block device |
| `make initrd` | Rebuild initrd.img |
| `make clean` | Remove generated images |

## Directory Structure

```
testing/
├── Makefile              # Quick start commands
├── README.md             # This file
└── qemu-test/
    ├── run-qemu.sh       # QEMU launch script
    ├── build-initrd.sh   # Build initrd from scratch
    ├── init              # VM init script
    ├── tests/            # Test programs
    │   ├── src/          # Test source files
    │   ├── Makefile      # Build wrapper
    │   └── CMakeLists.txt
    ├── initrd.img        # Generated initramfs
    ├── disk.qcow2        # Block device (optional)
    └── rootfs/           # Build artifact
```

## Writing Tests

Add test files to `qemu-test/tests/src/`:

```c
#include "test_common.h"

void run_tests(void)
{
    PASS("Example test");
}
```

Update `CMakeLists.txt` to build your test.

## Kernel Debugging

Terminal 1:
```bash
make qemu-debug
```

Terminal 2:
```bash
cd /root/kernel
gdb-multiarch vmlinux -ex 'target remote :1234'
```
