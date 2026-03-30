/*
 * Simple HugeTLBFS Swapoff Test
 *
 * Tests: swap out hugetlbfs pages, then run swapoff
 * Without hugetlb swapoff support, this may fail.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>

#include "test_common.h"

#define HPAGE_SIZE (2 * 1024 * 1024)
#define SWAP_DEVICE "/dev/zram0"

static int is_swap_active(void)
{
    FILE *fp = fopen("/proc/swaps", "r");
    char line[256];
    int active = 0;

    if (!fp) return 0;
    fgets(line, sizeof(line), fp); /* skip header */
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, SWAP_DEVICE))
            active = 1;
    }
    fclose(fp);
    return active;
}

static int enable_swap(void)
{
    if (system("mkswap " SWAP_DEVICE " 2>/dev/null") != 0)
        return -1;
    if (system("swapon " SWAP_DEVICE " 2>/dev/null") != 0)
        return -1;
    return 0;
}

static int disable_swap(void)
{
    return system("swapoff " SWAP_DEVICE " 2>/dev/null");
}

/* Test: hugetlbfs swap out then swapoff */
void test_hugetlbfs_swapoff(void)
{
    int fd;
    void *addr;
    int swpd_before, swpd_after;
    const char *test_file = "/mnt/huge/test_swapoff";

    printf("\nTest: hugetlbfs Swap + Swapoff\n");

    if (access("/mnt/huge", F_OK) != 0) {
        SKIP("hugetlbfs not mounted at /mnt/huge");
        return;
    }

    if (get_nr_hugepages() < 2) {
        SKIP("Need at least 2 hugepages");
        return;
    }

    if (!is_swap_active() && enable_swap() < 0) {
        SKIP("Failed to enable swap");
        return;
    }
    PASS("Swap is active");

    /* Create and map hugetlbfs file */
    fd = open(test_file, O_CREAT | O_RDWR, 0644);
    if (fd < 0 || ftruncate(fd, HPAGE_SIZE) != 0) {
        FAIL("Failed to create hugetlbfs file");
        if (fd >= 0) close(fd);
        return;
    }

    addr = mmap(NULL, HPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (addr == MAP_FAILED) {
        FAIL("Failed to mmap");
        unlink(test_file);
        return;
    }
    PASS("Mapped hugetlbfs file at %p", addr);

    /* Fill with pattern */
    memset(addr, 0xAB, HPAGE_SIZE);
    PASS("Memory filled with pattern");

    /* Swap out */
    swpd_before = get_hugepages_swpd();
    trigger_swap(addr, 0);
    usleep(100000);
    swpd_after = get_hugepages_swpd();
    INFO("HugePages_Swap: %d -> %d", swpd_before, swpd_after);

    if (swpd_after > swpd_before)
        PASS("Page swapped out");

    /* Try swapoff - this is the key test */
    INFO("Attempting swapoff...");
    if (disable_swap() != 0) {
        FAIL("swapoff failed (expected without hugetlb swapoff support)");
    } else {
        PASS("swapoff succeeded");
    }

    /* Cleanup: re-enable swap, unmap, cleanup */
    enable_swap();
    munmap(addr, HPAGE_SIZE);
    unlink(test_file);
    PASS("Cleanup complete");
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    printf("=== Simple HugeTLBFS Swapoff Test ===\n");
    printf("Kernel: "); fflush(stdout); system("uname -r");

    test_hugetlbfs_swapoff();

    printf("\n=== Summary ===\n");
    printf("Passed: %d, Failed: %d, Skipped: %d\n", passed, failed, skipped);

    return failed > 0 ? 1 : 0;
}
