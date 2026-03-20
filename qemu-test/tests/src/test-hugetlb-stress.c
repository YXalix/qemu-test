/*
 * HugeTLB Swap Concurrent Stress Test
 *
 * Tests:
 * 1. Anonymous huge pages: Writer Verify + Concurrent Swapin Trigger
 * 2. HugeTLBFS (file-backed) huge pages: same concurrent stress via /mnt/huge/
 *
 * Design:
 * - Writers: Each writer owns a set of pages, writes pattern and verifies read-back
 * - Swapin Triggers: Randomly access pages to trigger concurrent swapin
 * - Swappers: Swap out pages to create pressure
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>

#include "test_common.h"

/* Debug logging - disable in normal runs */
#define DEBUG 0
#define DBG(fmt, ...) do { if (DEBUG) { \
    struct timespec _ts; \
    clock_gettime(CLOCK_MONOTONIC, &_ts); \
    printf("[%ld.%06ld] DBG: " fmt "\n", _ts.tv_sec, _ts.tv_nsec/1000, ##__VA_ARGS__); \
    fflush(stdout); \
}} while(0)

/* Test configuration */
#define NR_WRITERS      2       /* Number of writer threads */
#define NR_SWAPPERS     1       /* Number of swapper threads */
#define NR_TRIGGERS     8       /* Number of swapin trigger threads */
#define NR_PAGES        16      /* Total pages */
#define TEST_DURATION   5      /* seconds */

/* Per-page metadata */
struct page_meta {
    uint64_t write_seq;         /* Sequence number for pattern generation */
    unsigned char pattern;      /* Current expected pattern */
};

static struct page_meta g_page_meta[NR_PAGES];
static void *g_pages[NR_PAGES];
static _Atomic int g_stop = 0;

/* Statistics */
static _Atomic uint64_t g_write_count = 0;
static _Atomic uint64_t g_verify_ok = 0;
static _Atomic uint64_t g_verify_fail = 0;
static _Atomic uint64_t g_swap_out_count = 0;
static _Atomic uint64_t g_swap_in_trigger = 0;

struct thread_arg {
    int id;
    int start_page;     /* First page this thread owns */
    int nr_pages;       /* Number of pages this thread owns */
};

static void reset_stats(void)
{
    atomic_store(&g_write_count, 0);
    atomic_store(&g_verify_ok, 0);
    atomic_store(&g_verify_fail, 0);
    atomic_store(&g_swap_out_count, 0);
    atomic_store(&g_swap_in_trigger, 0);
    atomic_store(&g_stop, 0);
}

static void print_stats(void)
{
    printf("\n  --- Statistics ---\n");
    INFO("Writes:         %lu", atomic_load(&g_write_count));
    INFO("Verify OK:      %lu", atomic_load(&g_verify_ok));
    INFO("Verify Fail:    %lu", atomic_load(&g_verify_fail));
    INFO("Swap out:       %lu", atomic_load(&g_swap_out_count));
    INFO("Swapin trigger: %lu", atomic_load(&g_swap_in_trigger));
}

/* Verify page content matches expected pattern */
static int verify_page_content(int page_idx, unsigned char expected)
{
    unsigned char *p = g_pages[page_idx];
    size_t i;

    for (i = 0; i < HPAGE_SIZE_2M; i++) {
        if (p[i] != expected) {
            FAIL("Page %d corruption at offset %zu: expected 0x%02X, got 0x%02X",
                 page_idx, i, expected, p[i]);
            return -1;
        }
    }
    return 0;
}

/* Writer thread: owns specific pages, writes pattern and verifies */
static void *writer_thread(void *arg)
{
    struct thread_arg *t = arg;
    int id = t->id;
    int start = t->start_page;
    int count = t->nr_pages;
    int i;
    unsigned char pattern;
    uint64_t seq;

    DBG("Writer[%d] starting, pages %d-%d", id, start, start + count - 1);

    while (!atomic_load(&g_stop)) {
        for (i = 0; i < count && !atomic_load(&g_stop); i++) {
            int page_idx = start + i;
            struct page_meta *meta = &g_page_meta[page_idx];

            /* Generate new pattern based on sequence */
            seq = ++meta->write_seq;
            pattern = (unsigned char)(seq & 0xFF);
            if (pattern == 0) pattern = 1;  /* Avoid 0 */
            meta->pattern = pattern;

            /* Write pattern to entire page */
            memset(g_pages[page_idx], pattern, HPAGE_SIZE_2M);

            /* Immediately verify read-back (may trigger swapin) */
            if (verify_page_content(page_idx, pattern) == 0) {
                atomic_fetch_add(&g_verify_ok, 1);
            } else {
                atomic_fetch_add(&g_verify_fail, 1);
                return NULL;  /* Exit on error */
            }

            atomic_fetch_add(&g_write_count, 1);
        }
    }

    DBG("Writer[%d] finished", id);
    return NULL;
}

/* Swapin trigger thread: randomly access pages to trigger concurrent swapin */
static void *swapin_trigger_thread(void *arg)
{
    struct thread_arg *t = arg;
    int id = t->id;
    unsigned int seed = id + time(NULL);

    DBG("SwapinTrigger[%d] starting", id);

    while (!atomic_load(&g_stop)) {
        /* Pick random page */
        int page_idx = rand_r(&seed) % NR_PAGES;

        /*
         * Access page to trigger swapin if it's swapped out.
         * Multiple threads may concurrently access the same page,
         * testing kernel's swapin race handling.
         */
        volatile unsigned char val = *(volatile unsigned char *)g_pages[page_idx];
        (void)val;

        atomic_fetch_add(&g_swap_in_trigger, 1);

        /* Small delay to allow swapper to work */
        if ((rand_r(&seed) % 100) < 10) {
            usleep(100);  /* 100us delay occasionally */
        }
    }

    DBG("SwapinTrigger[%d] finished", id);
    return NULL;
}

/* Swapper thread: swaps out pages */
static void *swapper_thread(void *arg)
{
    struct thread_arg *t = arg;
    int id = t->id;
    int fd;
    char addr_str[32];
    int i = 0;

    fd = open_swap_pages();
    if (fd < 0) {
        INFO("Swapper[%d]: Cannot open swap_pages", id);
        return NULL;
    }

    DBG("Swapper[%d] starting", id);

    while (!atomic_load(&g_stop)) {
        /* Cycle through pages */
        int page_idx = (i++) % NR_PAGES;

        /* Try to swap out (may fail if page is locked by writer) */
        snprintf(addr_str, sizeof(addr_str), "0x%lx\n",
                 (unsigned long)g_pages[page_idx]);

        if (write(fd, addr_str, strlen(addr_str)) > 0) {
            atomic_fetch_add(&g_swap_out_count, 1);
            DBG("Swapper[%d]: swapped out page %d", id, page_idx);
        }

        /* Small delay before next swap */
        usleep(100000);  /* 100ms */
    }

    DBG("Swapper[%d] finished", id);
    close(fd);
    return NULL;
}

static int init_page_meta(void)
{
    int i;
    for (i = 0; i < NR_PAGES; i++) {
        g_page_meta[i].write_seq = 0;
        g_page_meta[i].pattern = 0;
    }
    return 0;
}

static int alloc_test_pages(void)
{
    int i;

    for (i = 0; i < NR_PAGES; i++) {
        g_pages[i] = alloc_hugetlb(HPAGE_SIZE_2M);
        if (!g_pages[i]) {
            FAIL("Failed to allocate hugepage %d: %s", i, strerror(errno));
            return -1;
        }
        /* Initialize with zero */
        memset(g_pages[i], 0, HPAGE_SIZE_2M);
    }
    PASS("Allocated %d huge pages (2MB each)", NR_PAGES);
    return 0;
}

static void free_test_pages(void)
{
    int i;
    for (i = 0; i < NR_PAGES; i++) {
        if (g_pages[i]) {
            free_hugetlb(g_pages[i], HPAGE_SIZE_2M);
            g_pages[i] = NULL;
        }
    }
}

/* ------------------------------------------------------------------ *
 * HugeTLBFS helpers                                                   *
 * ------------------------------------------------------------------ */

#define HUGETLBFS_STRESS_BASE "/mnt/huge/stress"

static char g_hugetlbfs_paths[NR_PAGES][64];

static int alloc_hugetlbfs_pages(void)
{
    int i;

    if (access("/mnt/huge", F_OK) != 0) {
        INFO("hugetlbfs not mounted at /mnt/huge");
        return -1;
    }

    for (i = 0; i < NR_PAGES; i++) {
        int fd;
        void *addr;

        snprintf(g_hugetlbfs_paths[i], sizeof(g_hugetlbfs_paths[i]),
                 HUGETLBFS_STRESS_BASE "-%d", i);

        fd = open(g_hugetlbfs_paths[i], O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (fd < 0) {
            FAIL("Failed to create hugetlbfs file %d: %s", i, strerror(errno));
            /* Clean up already-allocated pages */
            while (--i >= 0) {
                munmap(g_pages[i], HPAGE_SIZE_2M);
                g_pages[i] = NULL;
                unlink(g_hugetlbfs_paths[i]);
            }
            return -1;
        }

        if (ftruncate(fd, HPAGE_SIZE_2M) != 0) {
            FAIL("ftruncate failed for file %d: %s", i, strerror(errno));
            close(fd);
            unlink(g_hugetlbfs_paths[i]);
            while (--i >= 0) {
                munmap(g_pages[i], HPAGE_SIZE_2M);
                g_pages[i] = NULL;
                unlink(g_hugetlbfs_paths[i]);
            }
            return -1;
        }

        addr = mmap(NULL, HPAGE_SIZE_2M, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);  /* fd no longer needed after mmap */

        if (addr == MAP_FAILED) {
            FAIL("mmap failed for hugetlbfs page %d: %s", i, strerror(errno));
            unlink(g_hugetlbfs_paths[i]);
            while (--i >= 0) {
                munmap(g_pages[i], HPAGE_SIZE_2M);
                g_pages[i] = NULL;
                unlink(g_hugetlbfs_paths[i]);
            }
            return -1;
        }

        g_pages[i] = addr;
        memset(addr, 0, HPAGE_SIZE_2M);
    }

    PASS("Allocated %d hugetlbfs-backed pages (2MB each)", NR_PAGES);
    return 0;
}

static void free_hugetlbfs_pages(void)
{
    int i;
    for (i = 0; i < NR_PAGES; i++) {
        if (g_pages[i]) {
            munmap(g_pages[i], HPAGE_SIZE_2M);
            g_pages[i] = NULL;
        }
        unlink(g_hugetlbfs_paths[i]);
    }
}

static void test_writer_swapin_concurrent(void)
{
    pthread_t writers[NR_WRITERS];
    pthread_t swappers[NR_SWAPPERS];
    pthread_t triggers[NR_TRIGGERS];
    struct thread_arg wargs[NR_WRITERS];
    struct thread_arg sargs[NR_SWAPPERS];
    struct thread_arg targs[NR_TRIGGERS];
    int pages_per_writer = NR_PAGES / NR_WRITERS;
    int i;

    printf("\nTest: Writer Verify + Concurrent Swapin\n");
    INFO("Config: %d writers, %d swappers, %d triggers, %d pages, %ds duration",
         NR_WRITERS, NR_SWAPPERS, NR_TRIGGERS, NR_PAGES, TEST_DURATION);

    if (get_nr_hugepages() < NR_PAGES) {
        SKIP("Need at least %d hugepages", NR_PAGES);
        return;
    }

    if (alloc_test_pages() < 0)
        return;

    init_page_meta();
    reset_stats();

    /* Create writer threads - each owns a range of pages */
    for (i = 0; i < NR_WRITERS; i++) {
        wargs[i].id = i;
        wargs[i].start_page = i * pages_per_writer;
        wargs[i].nr_pages = pages_per_writer;
        pthread_create(&writers[i], NULL, writer_thread, &wargs[i]);
    }

    /* Create swapper threads */
    for (i = 0; i < NR_SWAPPERS; i++) {
        sargs[i].id = i;
        pthread_create(&swappers[i], NULL, swapper_thread, &sargs[i]);
    }

    /* Create swapin trigger threads */
    for (i = 0; i < NR_TRIGGERS; i++) {
        targs[i].id = i;
        pthread_create(&triggers[i], NULL, swapin_trigger_thread, &targs[i]);
    }

    INFO("All threads started, running for %d seconds...", TEST_DURATION);

    /* Let it run */
    sleep(TEST_DURATION);
    atomic_store(&g_stop, 1);

    /* Wait for completion */
    for (i = 0; i < NR_WRITERS; i++)
        pthread_join(writers[i], NULL);
    for (i = 0; i < NR_SWAPPERS; i++)
        pthread_join(swappers[i], NULL);
    for (i = 0; i < NR_TRIGGERS; i++)
        pthread_join(triggers[i], NULL);

    print_stats();

    /* Check for verification failures */
    if (atomic_load(&g_verify_fail) > 0) {
        FAIL("Detected %lu verification failures", atomic_load(&g_verify_fail));
    } else {
        PASS("All write+verify cycles completed successfully");
    }

    free_test_pages();

    PASS("Test completed");
}

static void test_hugetlbfs_writer_swapin_concurrent(void)
{
    pthread_t writers[NR_WRITERS];
    pthread_t swappers[NR_SWAPPERS];
    pthread_t triggers[NR_TRIGGERS];
    struct thread_arg wargs[NR_WRITERS];
    struct thread_arg sargs[NR_SWAPPERS];
    struct thread_arg targs[NR_TRIGGERS];
    int pages_per_writer = NR_PAGES / NR_WRITERS;
    int i;

    printf("\nTest: HugeTLBFS Writer Verify + Concurrent Swapin\n");
    INFO("Config: %d writers, %d swappers, %d triggers, %d pages, %ds duration",
         NR_WRITERS, NR_SWAPPERS, NR_TRIGGERS, NR_PAGES, TEST_DURATION);

    if (access("/mnt/huge", F_OK) != 0) {
        SKIP("hugetlbfs not mounted at /mnt/huge");
        return;
    }

    if (get_nr_hugepages() < NR_PAGES) {
        SKIP("Need at least %d hugepages", NR_PAGES);
        return;
    }

    if (alloc_hugetlbfs_pages() < 0)
        return;

    init_page_meta();
    reset_stats();

    /* Writer threads - each owns a range of pages */
    for (i = 0; i < NR_WRITERS; i++) {
        wargs[i].id = i;
        wargs[i].start_page = i * pages_per_writer;
        wargs[i].nr_pages = pages_per_writer;
        pthread_create(&writers[i], NULL, writer_thread, &wargs[i]);
    }

    /* Swapper threads */
    for (i = 0; i < NR_SWAPPERS; i++) {
        sargs[i].id = i;
        pthread_create(&swappers[i], NULL, swapper_thread, &sargs[i]);
    }

    /* Swapin trigger threads */
    for (i = 0; i < NR_TRIGGERS; i++) {
        targs[i].id = i;
        pthread_create(&triggers[i], NULL, swapin_trigger_thread, &targs[i]);
    }

    INFO("All threads started, running for %d seconds...", TEST_DURATION);

    sleep(TEST_DURATION);
    atomic_store(&g_stop, 1);

    for (i = 0; i < NR_WRITERS; i++)
        pthread_join(writers[i], NULL);
    for (i = 0; i < NR_SWAPPERS; i++)
        pthread_join(swappers[i], NULL);
    for (i = 0; i < NR_TRIGGERS; i++)
        pthread_join(triggers[i], NULL);

    print_stats();

    if (atomic_load(&g_verify_fail) > 0) {
        FAIL("Detected %lu verification failures", atomic_load(&g_verify_fail));
    } else {
        PASS("All write+verify cycles completed successfully");
    }

    free_hugetlbfs_pages();
    PASS("Test completed");
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("=== HugeTLB Swap Concurrent Stress Tests ===\n");
    printf("Kernel: ");
    fflush(stdout);
    system("uname -r");

    if (getuid() != 0)
        printf("WARNING: Not running as root\n");

    /* Seed random for any future use */
    srand(time(NULL));

    test_writer_swapin_concurrent();
    test_hugetlbfs_writer_swapin_concurrent();

    printf("\n=== Summary ===\n");
    printf("  Passed:  %d\n", passed);
    printf("  Failed:  %d\n", failed);
    printf("  Skipped: %d\n", skipped);

    return failed > 0 ? 1 : 0;
}
