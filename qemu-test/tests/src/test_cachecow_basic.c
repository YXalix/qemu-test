/*
 * CACHECOW Basic Functionality Tests
 * Tests device creation, sysfs interface, and basic operations
 *
 * NOTE: CACHECOW is now a character device (file-like), not a block device.
 * It only supports mmap access, not block I/O.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/sysmacros.h>
#include "test_common.h"

#define SYSFS_CREATE "/sys/class/cachecow/create"
#define TEST_ORIGIN "/dev/ublkb0"
#define TEST_UPPER "/dev/ublkb1"
#define CACHECOW_DEV "/dev/cachecow0"

void test_module_loaded(void)
{
    INFO("Checking if cachecow module is loaded...");

    struct stat st;
    if (stat("/sys/class/cachecow", &st) == 0) {
        PASS("cachecow sysfs class exists");
    } else {
        FAIL("cachecow sysfs class not found (module not loaded?)");
    }
}

void test_sysfs_create_exists(void)
{
    INFO("Checking sysfs create interface...");

    struct stat st;
    if (stat(SYSFS_CREATE, &st) == 0) {
        PASS("sysfs create attribute exists");
    } else {
        FAIL("sysfs create attribute not found");
    }
}

void test_create_device(void)
{
    INFO("Creating CACHECOW device...");

    /* Check that origin and upper devices exist */
    struct stat st;
    if (stat(TEST_ORIGIN, &st) != 0) {
        FAIL("Origin device %s not found", TEST_ORIGIN);
        return;
    }
    if (stat(TEST_UPPER, &st) != 0) {
        FAIL("Upper device %s not found", TEST_UPPER);
        return;
    }

    /* Open sysfs create file */
    int fd = open(SYSFS_CREATE, O_WRONLY);
    if (fd < 0) {
        FAIL("Failed to open %s: %s", SYSFS_CREATE, strerror(errno));
        return;
    }

    /* Write device paths */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s %s", TEST_ORIGIN, TEST_UPPER);

    ssize_t ret = write(fd, cmd, strlen(cmd));
    close(fd);

    if (ret < 0) {
        FAIL("Failed to create device: %s", strerror(errno));
        return;
    }

    PASS("Device creation command sent");

    /* Wait for device to appear */
    sleep(1);

    /* Check device exists */
    if (stat(CACHECOW_DEV, &st) == 0) {
        PASS("CACHECOW device %s created", CACHECOW_DEV);
    } else {
        FAIL("CACHECOW device %s not found after creation", CACHECOW_DEV);
    }
}

void test_device_type(void)
{
    INFO("Verifying device is a character device (not block device)...");

    struct stat st;
    if (stat(CACHECOW_DEV, &st) != 0) {
        FAIL("Failed to stat device: %s", strerror(errno));
        return;
    }

    if (S_ISCHR(st.st_mode)) {
        PASS("Device is a character device (major=%d, minor=%d)",
             major(st.st_rdev), minor(st.st_rdev));
    } else if (S_ISBLK(st.st_mode)) {
        FAIL("Device is a block device - expected character device!");
    } else {
        FAIL("Device is neither char nor block (mode=%o)", st.st_mode);
    }
}

void test_device_open(void)
{
    INFO("Testing device open...");

    int fd = open(CACHECOW_DEV, O_RDWR);
    if (fd < 0) {
        FAIL("Failed to open %s: %s", CACHECOW_DEV, strerror(errno));
        return;
    }

    PASS("Device opened successfully (fd=%d)", fd);
    close(fd);
}

void test_device_size(void)
{
    INFO("Checking device size...");

    int fd = open(CACHECOW_DEV, O_RDONLY);
    if (fd < 0) {
        FAIL("Failed to open device: %s", strerror(errno));
        return;
    }

    off_t size = lseek(fd, 0, SEEK_END);
    close(fd);

    if (size > 0) {
        PASS("Device size: %ld bytes (%ld MB)",
             (long)size, (long)(size / 1024 / 1024));
    } else {
        FAIL("Failed to get device size");
    }
}

void test_debugfs_exists(void)
{
    INFO("Checking debugfs interface...");

    struct stat st;
    if (stat("/sys/kernel/debug/cachecow", &st) == 0) {
        PASS("debugfs cachecow directory exists");
    } else {
        SKIP("debugfs cachecow directory not found (debugfs may not be mounted)");
        return;
    }

    char path[256];
    snprintf(path, sizeof(path), "/sys/kernel/debug/cachecow/cachecow0/info");
    if (stat(path, &st) == 0) {
        PASS("debugfs device info file exists");

        /* Try to read it */
        FILE *fp = fopen(path, "r");
        if (fp) {
            char buf[256];
            if (fgets(buf, sizeof(buf), fp)) {
                INFO("Device info: %s", buf);
            }
            fclose(fp);
        }
    } else {
        SKIP("debugfs device info not found");
    }
}

void run_tests(void)
{
    printf("\n=== CACHECOW Basic Tests ===\n\n");

    test_module_loaded();
    test_sysfs_create_exists();
    test_create_device();
    test_device_type();
    test_device_open();
    test_device_size();
    test_debugfs_exists();

    printf("\n=== Results: %d passed, %d failed, %d skipped ===\n",
           passed, failed, skipped);
}
