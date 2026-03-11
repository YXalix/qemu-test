# Makefile for tests - wrapper around CMake

BUILD_DIR := build

.PHONY: all clean

all:
	@mkdir -p $(BUILD_DIR) && cd $(BUILD_DIR) && cmake .. >/dev/null && $(MAKE)

clean:
	rm -rf $(BUILD_DIR)
