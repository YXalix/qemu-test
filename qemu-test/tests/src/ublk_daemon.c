/*
 * ublk_daemon - Userspace block device daemon for testing
 *
 * This implements a simple RAM-based block device using ublk.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/ublk_cmd.h>

#define UBLK_CTRL_DEV "/dev/ublk-control"
#define UBLK_DEV_PREFIX "/dev/ublkb"
#define DEFAULT_DEV_ID 0
#define DEFAULT_QUEUE_DEPTH 64
#define DEFAULT_NR_HW_QUEUES 1
#define BLOCK_SIZE 4096
#define DEV_SIZE (64 * 1024 * 1024)  /* 64MB */

struct ublk_daemon {
    int ctrl_fd;
    int dev_id;
    void *ramdisk;
    size_t dev_size;
    struct ublksrv_dev *dev;
};

static int ublk_open_control(void)
{
    int fd = open(UBLK_CTRL_DEV, O_RDWR);
    if (fd < 0) {
        perror("Failed to open ublk-control");
        return -1;
    }
    return fd;
}

static int ublk_add_device(int ctrl_fd, int dev_id, int queue_depth, int nr_queues)
{
    struct ublksrv_ctrl_cmd cmd = {
        .dev_id = dev_id,
        .queue_depth = queue_depth,
        .nr_hw_queues = nr_queues,
        .max_io_buf_bytes = BLOCK_SIZE,
    };

    if (ioctl(ctrl_fd, UBLK_CMD_ADD_DEV, &cmd) < 0) {
        perror("UBLK_CMD_ADD_DEV failed");
        return -1;
    }

    printf("ublk device %d added (qd=%d, queues=%d)\n", dev_id, queue_depth, nr_queues);
    return 0;
}

static int ublk_start_device(int ctrl_fd, int dev_id)
{
    struct ublksrv_ctrl_cmd cmd = {
        .dev_id = dev_id,
    };

    if (ioctl(ctrl_fd, UBLK_CMD_START_DEV, &cmd) < 0) {
        perror("UBLK_CMD_START_DEV failed");
        return -1;
    }

    printf("ublk device %d started\n", dev_id);
    return 0;
}

static int ublk_stop_device(int ctrl_fd, int dev_id)
{
    struct ublksrv_ctrl_cmd cmd = {
        .dev_id = dev_id,
    };

    if (ioctl(ctrl_fd, UBLK_CMD_STOP_DEV, &cmd) < 0) {
        perror("UBLK_CMD_STOP_DEV failed");
        return -1;
    }

    printf("ublk device %d stopped\n", dev_id);
    return 0;
}

static int ublk_del_device(int ctrl_fd, int dev_id)
{
    struct ublksrv_ctrl_cmd cmd = {
        .dev_id = dev_id,
    };

    if (ioctl(ctrl_fd, UBLK_CMD_DEL_DEV, &cmd) < 0) {
        perror("UBLK_CMD_DEL_DEV failed");
        return -1;
    }

    printf("ublk device %d deleted\n", dev_id);
    return 0;
}

static int handle_io_request(struct ublk_daemon *daemon, struct ublksrv_io_desc *iod)
{
    uint64_t offset = iod->offset;
    uint32_t len = iod->nr_sectors * 512;
    uint8_t op = iod->op;
    void *buf = (void *)(uintptr_t)iod->addr;

    if (offset + len > daemon->dev_size) {
        fprintf(stderr, "IO out of bounds: offset=%lu, len=%u\n", offset, len);
        return -1;
    }

    switch (op) {
    case UBLK_IO_OP_READ:
        memcpy(buf, daemon->ramdisk + offset, len);
        return 0;

    case UBLK_IO_OP_WRITE:
        memcpy(daemon->ramdisk + offset, buf, len);
        return 0;

    case UBLK_IO_OP_FLUSH:
        /* RAM disk, nothing to flush */
        return 0;

    default:
        fprintf(stderr, "Unknown IO op: %u\n", op);
        return -1;
    }
}

int main(int argc, char *argv[])
{
    struct ublk_daemon daemon = {0};
    int dev_id = DEFAULT_DEV_ID;
    int opt;

    while ((opt = getopt(argc, argv, "d:s:")) != -1) {
        switch (opt) {
        case 'd':
            dev_id = atoi(optarg);
            break;
        case 's':
            daemon.dev_size = atol(optarg);
            break;
        default:
            fprintf(stderr, "Usage: %s [-d dev_id] [-s size_bytes]\n", argv[0]);
            return 1;
        }
    }

    if (daemon.dev_size == 0)
        daemon.dev_size = DEV_SIZE;

    daemon.dev_id = dev_id;

    /* Allocate RAM disk */
    daemon.ramdisk = mmap(NULL, daemon.dev_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (daemon.ramdisk == MAP_FAILED) {
        perror("Failed to allocate RAM disk");
        return 1;
    }

    /* Open control device */
    daemon.ctrl_fd = ublk_open_control();
    if (daemon.ctrl_fd < 0) {
        munmap(daemon.ramdisk, daemon.dev_size);
        return 1;
    }

    /* Add device */
    if (ublk_add_device(daemon.ctrl_fd, dev_id, DEFAULT_QUEUE_DEPTH, DEFAULT_NR_HW_QUEUES) < 0) {
        close(daemon.ctrl_fd);
        munmap(daemon.ramdisk, daemon.dev_size);
        return 1;
    }

    /* Start device */
    if (ublk_start_device(daemon.ctrl_fd, dev_id) < 0) {
        ublk_del_device(daemon.ctrl_fd, dev_id);
        close(daemon.ctrl_fd);
        munmap(daemon.ramdisk, daemon.dev_size);
        return 1;
    }

    printf("ublk daemon running on %s%d (size=%zu MB)\n",
           UBLK_DEV_PREFIX, dev_id, daemon.dev_size / (1024 * 1024));
    printf("Press Ctrl+C to stop\n");

    /* Main loop - simplified, real implementation would use io_uring */
    while (1) {
        sleep(1);
    }

    /* Cleanup */
    ublk_stop_device(daemon.ctrl_fd, dev_id);
    ublk_del_device(daemon.ctrl_fd, dev_id);
    close(daemon.ctrl_fd);
    munmap(daemon.ramdisk, daemon.dev_size);

    return 0;
}
