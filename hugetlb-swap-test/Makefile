# SPDX-License-Identifier: GPL-2.0
# Makefile for Hugetlb Swap KUnit tests

# Kernel build directory - defaults to running kernel's build dir
# Override with: make KDIR=/path/to/kernel/build
KDIR ?= /lib/modules/$(shell uname -r)/build

# Module name
obj-m := hugetlb_swap_test.o

# Kernel configuration flags needed for the test module
# These must match the kernel's configuration
KBUILD_CFLAGS += -DCONFIG_HUGETLB_SWAP=1 -DCONFIG_HUGETLB_PAGE=1 -DCONFIG_SWAP=1 -DCONFIG_ETMEM=1

# Default target: build the module
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) KBUILD_CFLAGS="$(KBUILD_CFLAGS)" modules

# Clean target: remove build artifacts
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

# Install target: install module to kernel modules directory
install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install

# Help target
help:
	@echo "Hugetlb Swap KUnit Test Module Build"
	@echo ""
	@echo "Usage:"
	@echo "  make              - Build the test module"
	@echo "  make KDIR=...     - Build with custom kernel directory"
	@echo "  make clean        - Remove build artifacts"
	@echo "  make install      - Install module to system"
	@echo ""
	@echo "Current KDIR: $(KDIR)"

.PHONY: all clean install help
