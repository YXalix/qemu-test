# Makefile for tests
# Build output goes to build/ directory

CC := gcc
CFLAGS := -static -O2 -Wall -pthread

BUILD_DIR := build
TEST_SRCS := $(wildcard test-*.c)
TEST_BINS := $(patsubst %.c,$(BUILD_DIR)/%,$(TEST_SRCS))

.PHONY: all clean

all: $(BUILD_DIR) $(TEST_BINS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(BUILD_DIR)
