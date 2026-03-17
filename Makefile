# Makefile for QEMU Hugetlb Swap Testing
# Quick start commands for testing environment

QEMU_TEST_DIR := qemu-test
KERNEL_PATH ?= $(abspath $(CURDIR)/..)
KERNEL_IMAGE ?= $(KERNEL_PATH)/arch/arm64/boot/Image
QEMU_TIMEOUT ?= 0
AUTO_TEST ?= 1

.PHONY: all help qemu qemu-kvm qemu-debug build-swap clean clean-swap initrd qemu-test

all: help

help:
	@echo "QEMU Hugetlb Swap Test Environment"
	@echo ""
	@echo "Available targets:"
	@echo "  qemu          - Start QEMU VM"
	@echo "  qemu-kvm      - Start QEMU VM with KVM acceleration"
	@echo "  qemu-debug    - Start QEMU with GDB stub"
	@echo "  qemu-test     - Start QEMU with timeout (for CI)"
	@echo "  build-swap    - Build swap.img (512MB swap space)"
	@echo "  initrd        - Rebuild initrd.img"
	@echo "  build-all     - Build swap.img and initrd.img"
	@echo "  clean         - Remove generated images"
	@echo ""
	@echo "Variables:"
	@echo "  KERNEL_IMAGE   - Path to kernel image (default: $(KERNEL_IMAGE))"
	@echo "  QEMU_TIMEOUT   - Timeout for qemu-test in seconds (0 = no timeout)"
	@echo "  KAE            - PCI device for hisi_zip passthrough (e.g., 04:00.0)"
	@echo ""
	@echo "Examples:"
	@echo "  make qemu                       # Run interactively"
	@echo "  make qemu-kvm                   # Run with KVM acceleration"
	@echo "  make qemu-kvm KAE=31:00.1  	 # Run with KAE ZIP passthrough"
	@echo "  make qemu-debug                 # Debug with GDB"
	@echo "  make qemu-test QEMU_TIMEOUT=15  # Auto-test with 15s timeout"
	@echo "  make build-all                  # Build all images"

qemu:
	@echo "Starting QEMU..."
	cd $(QEMU_TEST_DIR) && ./run-qemu.sh $(KERNEL_IMAGE)

qemu-kvm:
	@echo "Starting QEMU with KVM acceleration..."
ifdef KAE
	@echo "KAE ZIP passthrough: $(KAE)"
endif
	cd $(QEMU_TEST_DIR) && QEMU_KVM=1 KAE=$(KAE) ./run-qemu.sh $(KERNEL_IMAGE)

qemu-debug:
	@echo "Starting QEMU with GDB stub on port 1234..."
	cd $(QEMU_TEST_DIR) && QEMU_DEBUG=1 ./run-qemu.sh $(KERNEL_IMAGE)

qemu-test:
	@if [ "$(QEMU_TIMEOUT)" = "0" ]; then \
		echo "ERROR: Set QEMU_TIMEOUT (e.g., make qemu-test QEMU_TIMEOUT=60)"; \
		exit 1; \
	fi
	@cd $(QEMU_TEST_DIR) && AUTO_TEST=$(AUTO_TEST) timeout --foreground $(QEMU_TIMEOUT) ./run-qemu.sh $(KERNEL_IMAGE); \
	EXIT_CODE=$$?; \
	if [ $$EXIT_CODE -eq 124 ]; then \
		echo "ERROR: Test timed out after $(QEMU_TIMEOUT) seconds"; \
		exit 124; \
	elif [ $$EXIT_CODE -ne 0 ]; then \
		exit $$EXIT_CODE; \
	fi

build-swap:
	@if [ -f $(QEMU_TEST_DIR)/swap.qcow2 ]; then \
		echo "swap.qcow2 already exists."; \
	else \
		echo "Creating swap.qcow2 (512MB qcow2 format)..."; \
		qemu-img create -f qcow2 $(QEMU_TEST_DIR)/swap.qcow2 512M; \
	fi

initrd:
	@echo "Rebuilding initrd.img..."
	cd $(QEMU_TEST_DIR) && ./build-initrd.sh

build-all: build-swap initrd
	@echo "All images built."
	@ls -lh $(QEMU_TEST_DIR)/*.img 2>/dev/null || true

clean:
	rm -f $(QEMU_TEST_DIR)/swap.qcow2 $(QEMU_TEST_DIR)/initrd.img

clean-swap:
	rm -f $(QEMU_TEST_DIR)/swap.qcow2
