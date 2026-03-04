# Makefile for tests
CC := gcc
CFLAGS := -static -O2 -Wall

TEST_SRCS := $(wildcard *.c)
TEST_BINS := $(patsubst %.c,%,$(TEST_SRCS))

.PHONY: all clean

all: $(TEST_BINS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TEST_BINS)
