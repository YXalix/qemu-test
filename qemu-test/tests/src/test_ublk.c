/*
 * test_ublk - Test ublk userspace block device functionality
 */

#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <linux/ublk_cmd.h>

#define UBLK_CTRL_DEV "/dev/ublk-control"
#define TEST_DEV_ID 0
#define TEST_DEV_SIZE (64 * 1024 * 1024)  /* 64MB */
#define BLOCK_SIZE 4096

static int open_ctrl_dev(void)
{
    int fd = open(UBLK_CTRL_DEV, O_RDWR);
    if (fd < 0) {
        SKIP("Cannot open %s: %s (ublk module not loaded?)", UBLK_CTRL_DEV, strerror(errno));
        return -1;
    }
    return fd;
}

static void test_ublk_module_loaded(void)
{
    printf("\nTest: ublk module loaded\n");

    if (access(UBLK_CTRL_DEV, F_OK) == 0)
        PASS("ublk-control device exists");
    else
        FAIL("ublk-control device not found");
}

static void test_ublk_add_device(void)
{
    struct ublksrv_ctrl_cmd cmd = {0};
    int ctrl_fd;

    printf("\nTest: ublk add device\n");

    ctrl_fd = open_ctrl_dev();
    if (ctrl_fd < 0) return;

    cmd.dev_id = TEST_DEV_ID;
    cmd.queue_depth = 64;
    cmd.nr_hw_queues = 1;
    cmd.max_io_buf_bytes = BLOCK_SIZE;

    if (ioctl(ctrl_fd, UBLK_CMD_ADD_DEV, &cmd) == 0)
        PASS("Device %d added successfully", TEST_DEV_ID);
    else
        FAIL("Failed to add device: %s", strerror(errno));

    close(ctrl_fd);
}

static void test_ublk_device_exists(void)
{
    char dev_path[64];

    printf("\nTest: ublk device node exists\n");

    snprintf(dev_path, sizeof(dev_path), "/dev/ublkb%d", TEST_DEV_ID);

    if (access(dev_path, F_OK) == 0)
        PASS("Device node %s exists", dev_path);
    else
        INFO("Device node %s not found (may need to start daemon)", dev_path);
}

static void test_ublk_basic_io(void)
{
    char dev_path[64];
    int fd;
    char write_buf[BLOCK_SIZE];
    char read_buf[BLOCK_SIZE];
    off_t offset = 0;

    printf("\nTest: ublk basic IO\n");

    snprintf(dev_path, sizeof(dev_path), "/dev/ublkb%d", TEST_DEV_ID);

    fd = open(dev_path, O_RDWR | O_DIRECT);
    if (fd < 0) {
        SKIP("Cannot open %s: %s", dev_path, strerror(errno));
        return;
    }

    /* Prepare write buffer with pattern */
    memset(write_buf, 0xAB, sizeof(write_buf));

    /* Write to device */
    if (pwrite(fd, write_buf, BLOCK_SIZE, offset) != BLOCK_SIZE) {
        FAIL("Write failed: %s", strerror(errno));
        close(fd);
        return;
    }
    PASS("Write %d bytes at offset %ld", BLOCK_SIZE, offset);

    /* Read back */
    if (pread(fd, read_buf, BLOCK_SIZE, offset) != BLOCK_SIZE) {
        FAIL("Read failed: %s", strerror(errno));
        close(fd);
        return;
    }
    PASS("Read %d bytes at offset %ld", BLOCK_SIZE, offset);

    /* Verify pattern */
    if (memcmp(write_buf, read_buf, BLOCK_SIZE) == 0)
        PASS("Data verification passed");
    else
        FAIL("Data mismatch");

    close(fd);
}

static void test_ublk_del_device(void)
{
    struct ublksrv_ctrl_cmd cmd = {0};
    int ctrl_fd;

    printf("\nTest: ublk delete device\n");

    ctrl_fd = open_ctrl_dev();
    if (ctrl_fd < 0) return;

    /* Stop first */
    cmd.dev_id = TEST_DEV_ID;
    ioctl(ctrl_fd, UBLK_CMD_STOP_DEV, &cmd);

    /* Then delete */
    if (ioctl(ctrl_fd, UBLK_CMD_DEL_DEV, &cmd) == 0)
        PASS("Device %d deleted successfully", TEST_DEV_ID);
    else
        INFO("Delete device failed: %s (may already be deleted)", strerror(errno));

    close(ctrl_fd);
}

void run_tests(void)
{
    test_ublk_module_loaded();
    test_ublk_add_device();
    test_ublk_device_exists();
    test_ublk_basic_io();
    test_ublk_del_device();
}
