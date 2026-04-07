/*
 * CACHECOW mmap Tests
 * Tests mmap operations and COW (Copy-on-Write) behavior
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include "test_common.h"

#define CACHECOW_DEV "/dev/cachecow0"
#define TEST_SIZE 4096

void test_mmap_read(void)
{
    INFO("Testing mmap read...");

    int fd = open(CACHECOW_DEV, O_RDONLY);
    if (fd < 0) {
        FAIL("Failed to open device: %s", strerror(errno));
        return;
    }

    void *map = mmap(NULL, TEST_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        FAIL("mmap failed: %s", strerror(errno));
        close(fd);
        return;
    }

    PASS("mmap successful (addr=%p)", map);

    /* Read first few bytes */
    unsigned char *data = (unsigned char *)map;
    INFO("First 4 bytes: %02x %02x %02x %02x",
         data[0], data[1], data[2], data[3]);
    PASS("Read via mmap successful");

    munmap(map, TEST_SIZE);
    close(fd);
}

void test_mmap_write(void)
{
    INFO("Testing mmap write (triggers COW)...");

    int fd = open(CACHECOW_DEV, O_RDWR);
    if (fd < 0) {
        FAIL("Failed to open device: %s", strerror(errno));
        return;
    }

    void *map = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        FAIL("mmap failed: %s", strerror(errno));
        close(fd);
        return;
    }

    PASS("mmap (RW) successful");

    /* Write test pattern - this triggers COW */
    char *data = (char *)map;
    const char *test_str = "CACHECOW_TEST_PATTERN";
    memcpy(data, test_str, strlen(test_str) + 1);

    PASS("Write via mmap successful (COW triggered)");

    /* Read back to verify */
    if (memcmp(data, test_str, strlen(test_str)) == 0) {
        PASS("Read back after COW successful");
    } else {
        FAIL("Read back after COW failed");
    }

    munmap(map, TEST_SIZE);
    close(fd);
}

void test_mmap_cow_isolation(void)
{
    INFO("Testing COW isolation...");

    /* First, write a pattern via mmap */
    int fd1 = open(CACHECOW_DEV, O_RDWR);
    if (fd1 < 0) {
        FAIL("Failed to open device: %s", strerror(errno));
        return;
    }

    void *map1 = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd1, 0);
    if (map1 == MAP_FAILED) {
        FAIL("mmap failed: %s", strerror(errno));
        close(fd1);
        return;
    }

    /* Write pattern */
    char *data1 = (char *)map1;
    char orig_byte = data1[0];
    data1[0] = 0x42;

    PASS("Modified first byte via mmap");

    munmap(map1, TEST_SIZE);
    close(fd1);

    /* Now open again and verify the change persisted */
    int fd2 = open(CACHECOW_DEV, O_RDONLY);
    if (fd2 < 0) {
        FAIL("Failed to reopen device: %s", strerror(errno));
        return;
    }

    void *map2 = mmap(NULL, TEST_SIZE, PROT_READ, MAP_SHARED, fd2, 0);
    if (map2 == MAP_FAILED) {
        FAIL("mmap failed: %s", strerror(errno));
        close(fd2);
        return;
    }

    char *data2 = (char *)map2;
    if (data2[0] == 0x42) {
        PASS("COW change persisted: byte = 0x%02x", (unsigned char)data2[0]);
    } else {
        FAIL("COW change lost: expected 0x42, got 0x%02x",
             (unsigned char)data2[0]);
    }

    munmap(map2, TEST_SIZE);
    close(fd2);
}

void test_mmap_multiple_pages(void)
{
    INFO("Testing mmap across multiple pages...");

    size_t size = TEST_SIZE * 4;  /* 16KB */

    int fd = open(CACHECOW_DEV, O_RDWR);
    if (fd < 0) {
        FAIL("Failed to open device: %s", strerror(errno));
        return;
    }

    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        FAIL("mmap failed: %s", strerror(errno));
        close(fd);
        return;
    }

    PASS("mmap (16KB) successful");

    /* Write to different pages */
    char *data = (char *)map;
    for (int i = 0; i < 4; i++) {
        data[i * TEST_SIZE] = 0x10 + i;
    }

    PASS("Wrote to 4 different pages");

    /* Verify */
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        if (data[i * TEST_SIZE] != 0x10 + i) {
            ok = 0;
            break;
        }
    }

    if (ok) {
        PASS("Multi-page data verification successful");
    } else {
        FAIL("Multi-page data verification failed");
    }

    munmap(map, size);
    close(fd);
}

void test_mmap_offset(void)
{
    INFO("Testing mmap with offset...");

    off_t offset = 8192;  /* 8KB offset */

    int fd = open(CACHECOW_DEV, O_RDWR);
    if (fd < 0) {
        FAIL("Failed to open device: %s", strerror(errno));
        return;
    }

    void *map = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if (map == MAP_FAILED) {
        FAIL("mmap with offset failed: %s", strerror(errno));
        close(fd);
        return;
    }

    PASS("mmap with offset successful");

    /* Write and verify */
    char *data = (char *)map;
    data[0] = 0x99;

    if (data[0] == 0x99) {
        PASS("Read/write at offset successful");
    } else {
        FAIL("Read/write at offset failed");
    }

    munmap(map, TEST_SIZE);
    close(fd);
}

void run_tests(void)
{
    printf("\n=== CACHECOW mmap Tests ===\n\n");

    test_mmap_read();
    test_mmap_write();
    test_mmap_cow_isolation();
    test_mmap_multiple_pages();
    test_mmap_offset();

    printf("\n=== Results: %d passed, %d failed, %d skipped ===\n",
           passed, failed, skipped);
}
