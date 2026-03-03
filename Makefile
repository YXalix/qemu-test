# Makefile for QEMU Hugetlb Swap Testing
# Quick start commands for testing environment

QEMU_TEST_DIR := qemu-test
KERNEL_IMAGE ?= /root/kernel/arch/arm64/boot/Image
QEMU_TIMEOUT ?= 0  # 0 = no timeout, or set seconds (e.g., 15 for 15 seconds)
AUTO_TEST ?= 1     # 1 = auto-run tests and exit, 0 = interactive shell with timeout

.PHONY: all help qemu build-swap clean clean-swap initrd qemu-test

all: help

help:
	@echo "QEMU Hugetlb Swap Test Environment"
	@echo ""
	@echo "Available targets:"
	@echo "  qemu          - Start QEMU VM (runs until Ctrl+C)"
	@echo "  qemu-debug    - Start QEMU with GDB stub for debugging"
	@echo "  qemu-test     - Start QEMU with timeout (for CI/testing)"
	@echo "  build-swap    - Build swap.img (one-time setup, 512MB swap space)"
	@echo "  initrd        - Rebuild initrd.img from rootfs/"
	@echo "  build-all     - Build swap.img and initrd.img"
	@echo "  clean         - Remove all generated images"
	@echo "  clean-swap    - Remove swap.img only"
	@echo ""
	@echo "Variables:"
	@echo "  KERNEL_IMAGE   - Path to kernel image (default: $(KERNEL_IMAGE))"
	@echo "  QEMU_TIMEOUT   - Timeout for qemu-test in seconds (default: $(QEMU_TIMEOUT)=no timeout)"
	@echo "  AUTO_TEST      - Auto-run tests (1=enabled, 0=interactive, default: $(AUTO_TEST))"
	@echo ""
	@echo "Examples:"
	@echo "  make qemu                                      # Run interactively until Ctrl+C"
	@echo "  make qemu-debug                                # Debug with GDB (port 1234)"
	@echo "  make qemu-test QEMU_TIMEOUT=15                 # Auto-test with 15s timeout"
	@echo "  make qemu-test QEMU_TIMEOUT=15 AUTO_TEST=0     # Interactive with 15s timeout"
	@echo "  make qemu KERNEL_IMAGE=...                     # Run with custom kernel"
	@echo "  make build-all                                 # Build all images from scratch"

# Start QEMU VM (runs indefinitely until Ctrl+C)
qemu:
	@echo "Starting QEMU..."
	cd $(QEMU_TEST_DIR) && ./run-qemu.sh $(KERNEL_IMAGE)

# Start QEMU VM with GDB stub for debugging (waits for GDB connection on port 1234)
qemu-debug:
	@echo "Starting QEMU with GDB stub on port 1234..."
	@echo "In another terminal, run: gdb-multiarch vmlinux -ex 'target remote :1234'"
	@cd $(QEMU_TEST_DIR) && QEMU_DEBUG=1 ./run-qemu.sh $(KERNEL_IMAGE)

# Start QEMU VM with timeout (auto-test mode by default, set AUTO_TEST=0 for interactive)
qemu-test:
	@echo "Starting QEMU with timeout $(QEMU_TIMEOUT)s..."
	@if [ "$(QEMU_TIMEOUT)" = "0" ]; then \
		echo "ERROR: Set QEMU_TIMEOUT to a positive value (e.g., make qemu-test QEMU_TIMEOUT=15)"; \
		exit 1; \
	fi
	@cd $(QEMU_TEST_DIR) && AUTO_TEST=$(AUTO_TEST) timeout --foreground --kill-after=5 $(QEMU_TIMEOUT) ./run-qemu.sh $(KERNEL_IMAGE); \
	EXIT_CODE=$$?; \
	if [ $$EXIT_CODE -eq 137 ] || [ $$EXIT_CODE -eq 9 ]; then \
		echo "ERROR: QEMU killed (timeout exceeded $(QEMU_TIMEOUT)s)"; \
		exit 1; \
	elif [ $$EXIT_CODE -eq 124 ]; then \
		echo "ERROR: QEMU timed out after $(QEMU_TIMEOUT)s"; \
		exit 1; \
	elif [ $$EXIT_CODE -ne 0 ]; then \
		echo "ERROR: QEMU exited with code $$EXIT_CODE"; \
		exit $$EXIT_CODE; \
	else \
		echo "SUCCESS: QEMU test completed"; \
	fi

# Build swap.img with swap space (one-time setup)
build-swap:
	@if [ -f $(QEMU_TEST_DIR)/swap.img ]; then \
		echo "swap.img already exists ($(shell ls -lh $(QEMU_TEST_DIR)/swap.img | awk '{print $$5}'))."; \
		echo "Run 'make clean-swap' first if you want to recreate it."; \
	else \
		echo "Building swap.img (512MB swap space)..."; \
		cd $(QEMU_TEST_DIR) && \
			dd if=/dev/zero of=swap.img bs=1M count=512 && \
			mkswap swap.img && \
			echo "swap.img created successfully"; \
	fi

# Rebuild initrd.img from rootfs/ (syncs source files first)
initrd:
	@echo "Syncing source files to rootfs/..."
	@# Copy init script from source
	@cp $(QEMU_TEST_DIR)/init $(QEMU_TEST_DIR)/rootfs/init
	@chmod +x $(QEMU_TEST_DIR)/rootfs/init
	@# Copy test scripts from source
	@mkdir -p $(QEMU_TEST_DIR)/rootfs/test-scripts
	@cp $(QEMU_TEST_DIR)/test-scripts/* $(QEMU_TEST_DIR)/rootfs/test-scripts/ 2>/dev/null || true
	@chmod +x $(QEMU_TEST_DIR)/rootfs/test-scripts/*
	@# Create symlinks in bin/ for test scripts
	@for script in $(QEMU_TEST_DIR)/rootfs/test-scripts/*; do \
		if [ -f "$$script" ]; then \
			name=$$(basename "$$script"); \
			ln -sf ../test-scripts/$$name $(QEMU_TEST_DIR)/rootfs/bin/$$name 2>/dev/null || true; \
		fi; \
	done
	@echo "Rebuilding initrd.img from rootfs/..."
	cd $(QEMU_TEST_DIR)/rootfs && \
		find . -print0 | cpio --null -o -H newc | gzip -9 > ../initrd.img && \
		echo "initrd.img rebuilt successfully"

# Build all images
build-all: build-swap initrd
	@echo "All images built successfully"
	@ls -lh $(QEMU_TEST_DIR)/*.img

# Clean generated images
clean:
	@echo "Cleaning generated images..."
	cd $(QEMU_TEST_DIR) && rm -f swap.img initrd.img
	@echo "Clean complete"

# Clean swap image only
clean-swap:
	@echo "Cleaning swap.img..."
	cd $(QEMU_TEST_DIR) && rm -f swap.img
	@echo "Swap image removed"

# Check environment
check:
	@echo "Checking environment..."
	@test -f $(KERNEL_IMAGE) || (echo "ERROR: Kernel not found at $(KERNEL_IMAGE)" && exit 1)
	@test -d $(QEMU_TEST_DIR)/rootfs || (echo "ERROR: rootfs/ directory not found" && exit 1)
	@test -f $(QEMU_TEST_DIR)/run-qemu.sh || (echo "ERROR: run-qemu.sh not found" && exit 1)
	@echo "Environment OK"
	@echo "Kernel: $(KERNEL_IMAGE)"
	@echo "QEMU: $$(which qemu-system-aarch64 2>/dev/null || echo 'NOT FOUND')"
