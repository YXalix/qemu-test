/*
 * Hugetlbfs Swap Race Test - Simplified
 *
 * Thread A: Continuously swap out the same page
 * Thread B: Continuously read/write and verify data
 *
 * This creates maximum probability of hitting the race window where:
 * 1. swap out updates XArray with swap entry but data not yet written
 * 2. swap in reads stale data from swap slot
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>

#include "test_common.h"

#define HPAGE_SIZE (2 * 1024 * 1024)

/* Shared state */
static void *g_addr = NULL;
static atomic_int g_stop = 0;
static atomic_int g_swap_count = 0;
static atomic_int g_verify_count = 0;
static atomic_int g_error_count = 0;

/*
 * Thread A: Swap out thread
 * Continuously trigger swap out on the same page
 */
static void *swapout_thread(void *arg)
{
    (void)arg;
    int fd;
    char addr_str[32];

    fd = open("/proc/self/swap_pages", O_WRONLY);
    if (fd < 0) {
        INFO("Swap thread: cannot open swap_pages: %s", strerror(errno));
        return NULL;
    }

    snprintf(addr_str, sizeof(addr_str), "0x%lx\n", (uintptr_t)g_addr);

    while (!atomic_load(&g_stop)) {
        /* Trigger swap out */
        lseek(fd, 0, SEEK_SET);
        write(fd, addr_str, strlen(addr_str));

        atomic_fetch_add(&g_swap_count, 1);

        /* Small delay to let other thread run */
        usleep(1000);  /* 1ms */
    }

    close(fd);
    return NULL;
}

/*
 * Thread B: Verify thread
 * Continuously write pattern, read back and verify
 */
static void *verify_thread(void *arg)
{
    (void)arg;
    uint8_t *p = g_addr;
    uint32_t seed = 0x12345678;
    int errors = 0;

    while (!atomic_load(&g_stop) && errors < 100) {
        size_t i;
        uint32_t pattern;

        /* Generate new pattern */
        seed = seed * 1103515245 + 12345;
        pattern = seed;

        /* Fill entire page with pattern */
        for (i = 0; i < HPAGE_SIZE / sizeof(uint32_t); i++) {
            ((uint32_t*)p)[i] = pattern + i;
        }

        /* Memory barrier to ensure writes complete */
        __sync_synchronize();

        /* Read back and verify */
        for (i = 0; i < HPAGE_SIZE / sizeof(uint32_t); i++) {
            uint32_t expected = pattern + i;
            uint32_t actual = ((uint32_t*)p)[i];
            if (actual != expected) {
                errors++;
                atomic_fetch_add(&g_error_count, 1);
                if (errors <= 5) {
                    FAIL("Data corruption at offset %zu: expected 0x%08X, got 0x%08X",
                         i * sizeof(uint32_t), expected, actual);
                }
                break;  /* Stop verifying this round, go to next */
            }
        }

        atomic_fetch_add(&g_verify_count, 1);
    }

    return NULL;
}

/*
 * Create hugetlbfs file and mmap it
 */
static void *create_hugetlbfs_mapping(void)
{
    char path[256];
    int fd;
    void *addr;

    /* Mount hugetlbfs */
    system("mkdir -p /mnt/hugetlb 2>/dev/null");
    system("mount -t hugetlbfs none /mnt/hugetlb -o pagesize=2MB 2>/dev/null");

    snprintf(path, sizeof(path), "/mnt/hugetlb/race_test_%d", getpid());

    fd = open(path, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        INFO("Cannot create hugetlbfs file: %s", strerror(errno));
        return NULL;
    }

    if (ftruncate(fd, HPAGE_SIZE) < 0) {
        close(fd);
        unlink(path);
        return NULL;
    }

    addr = mmap(NULL, HPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) {
        unlink(path);
        return NULL;
    }

    return addr;
}

static void cleanup_mapping(void)
{
    char path[256];
    snprintf(path, sizeof(path), "/mnt/hugetlb/race_test_%d", getpid());
    if (g_addr) {
        munmap(g_addr, HPAGE_SIZE);
        unlink(path);
    }
}

int main(int argc, char *argv[])
{
    pthread_t swap_tid, verify_tid;
    int duration_sec = 10;
    int swap_count, verify_count, error_count;

    (void)argc;
    (void)argv;

    printf("=== Hugetlbfs Swap Race Test (Simplified) ===\n");
    printf("Thread A: Continuous swap out\n");
    printf("Thread B: Continuous read/write verify\n");
    printf("Duration: %d seconds\n\n", duration_sec);

    /* Create mapping */
    g_addr = create_hugetlbfs_mapping();
    if (!g_addr) {
        FAIL("Cannot create hugetlbfs mapping");
        return 1;
    }

    INFO("Huge page mapped at %p", g_addr);

    /* Create threads */
    if (pthread_create(&swap_tid, NULL, swapout_thread, NULL) != 0) {
        FAIL("Cannot create swap thread");
        cleanup_mapping();
        return 1;
    }

    if (pthread_create(&verify_tid, NULL, verify_thread, NULL) != 0) {
        atomic_store(&g_stop, 1);
        pthread_join(swap_tid, NULL);
        FAIL("Cannot create verify thread");
        cleanup_mapping();
        return 1;
    }

    /* Run for specified duration */
    sleep(duration_sec);

    /* Stop threads */
    atomic_store(&g_stop, 1);
    pthread_join(swap_tid, NULL);
    pthread_join(verify_tid, NULL);

    /* Get final counts */
    swap_count = atomic_load(&g_swap_count);
    verify_count = atomic_load(&g_verify_count);
    error_count = atomic_load(&g_error_count);

    printf("\n=== Results ===\n");
    printf("  Swap out operations: %d\n", swap_count);
    printf("  Verify operations:   %d\n", verify_count);
    printf("  Errors detected:     %d\n", error_count);

    if (error_count == 0) {
        PASS("No data corruption detected");
    } else {
        FAIL("RACE CONDITION DETECTED: %d data corruption errors", error_count);
        printf("\nThis confirms the swap in/out race condition bug.\n");
    }

    cleanup_mapping();
    return error_count > 0 ? 1 : 0;
}
