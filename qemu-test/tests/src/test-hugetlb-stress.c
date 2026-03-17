/*
 * HugeTLB Swap Concurrent Stress Test
 *
 * Test scenarios:
 * 1. Mixed read/write + swap: Multiple reader/writer threads with one swapper
 * 2. Race swapin: Multiple threads racing to swapin the same page
 * 3. Loop stress: Continuous stress with alternating patterns
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
#include <sys/mman.h>
#include <errno.h>

#include "test_common.h"

/* Test configuration - aggressive settings to trigger races */
#define NR_READERS      4
#define NR_WRITERS      2
#define NR_RACERS       8
#define NR_PAGES        4
#define TEST_DURATION   5       /* seconds per phase */
#define LOOP_ROUNDS     10

/* Global state */
static pthread_barrier_t g_barrier;
static volatile int g_stop = 0;
static void *g_pages[NR_PAGES];
static unsigned char g_patterns[NR_PAGES];

/* Statistics */
static _Atomic uint64_t g_read_count = 0;
static _Atomic uint64_t g_write_count = 0;
static _Atomic uint64_t g_swap_out_count = 0;
static _Atomic uint64_t g_swap_in_count = 0;
static _Atomic uint64_t g_error_count = 0;
static _Atomic uint64_t g_race_wins = 0;

static void reset_stats(void)
{
    atomic_store(&g_read_count, 0);
    atomic_store(&g_write_count, 0);
    atomic_store(&g_swap_out_count, 0);
    atomic_store(&g_swap_in_count, 0);
    atomic_store(&g_error_count, 0);
    atomic_store(&g_race_wins, 0);
    g_stop = 0;
}

static void print_stats(const char *name)
{
    printf("\n  --- %s Statistics ---\n", name);
    INFO("Reads:     %lu", atomic_load(&g_read_count));
    INFO("Writes:    %lu", atomic_load(&g_write_count));
    INFO("Swap out:  %lu", atomic_load(&g_swap_out_count));
    INFO("Swap in:   %lu", atomic_load(&g_swap_in_count));
    INFO("Errors:    %lu", atomic_load(&g_error_count));
}

/* Allocate huge pages for testing */
static int alloc_test_pages(void)
{
    int i;

    for (i = 0; i < NR_PAGES; i++) {
        g_pages[i] = alloc_hugetlb(HPAGE_SIZE_2M);
        if (!g_pages[i]) {
            FAIL("Failed to allocate hugepage %d: %s", i, strerror(errno));
            return -1;
        }
        g_patterns[i] = 0xA0 + i;
        memset(g_pages[i], g_patterns[i], HPAGE_SIZE_2M);
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

/* Verify page content */
static int verify_page(int idx)
{
    unsigned char *p = g_pages[idx];
    unsigned char expected = g_patterns[idx];
    size_t i;

    for (i = 0; i < HPAGE_SIZE_2M; i++) {
        if (p[i] != expected) {
            FAIL("Page %d corruption at offset %zu: expected 0x%02X, got 0x%02X",
                 idx, i, expected, p[i]);
            return -1;
        }
    }
    return 0;
}

/*
 * Test 1: Mixed Read/Write + Swap
 *
 * Multiple reader threads continuously verify data,
 * writer threads modify data patterns,
 * one swapper thread cycles pages through swap.
 */

/* Per-page mutex to protect reads/writes */
static pthread_mutex_t g_page_mutex[NR_PAGES];

/* Debug logging - disable in normal runs */
#define DEBUG 0
#define DBG(fmt, ...) do { if (DEBUG) { \
    struct timespec _ts; \
    clock_gettime(CLOCK_MONOTONIC, &_ts); \
    printf("[%ld.%06ld] DBG: " fmt "\n", _ts.tv_sec, _ts.tv_nsec/1000, ##__VA_ARGS__); \
    fflush(stdout); \
}} while(0)

struct thread_arg {
    int id;
    int page_idx;
};

static void *reader_thread(void *arg)
{
    struct thread_arg *t = arg;
    int id = t->id;
    int page_idx = t->page_idx;
    unsigned long local_reads = 0;
    unsigned long local_errors = 0;

    DBG("Reader[%d] starting on page %d", id, page_idx);
    pthread_barrier_wait(&g_barrier);

    while (!g_stop) {
        unsigned char expected;
        int ret;

        /* NO LOCK - intentionally racy to trigger kernel bugs */
        expected = g_patterns[page_idx];
        ret = verify_page(page_idx);

        if (ret != 0) {
            DBG("Reader[%d] verify FAILED (expected=0x%02X)", id, expected);
            local_errors++;
            atomic_fetch_add(&g_error_count, 1);
        }
        local_reads++;
    }

    DBG("Reader[%d] finished: reads=%lu errors=%lu", id, local_reads, local_errors);
    atomic_fetch_add(&g_read_count, local_reads);
    return NULL;
}

static void *writer_thread(void *arg)
{
    struct thread_arg *t = arg;
    int id = t->id;
    int page_idx = t->page_idx;
    unsigned long local_writes = 0;
    unsigned char new_pattern;

    DBG("Writer[%d] starting on page %d", id, page_idx);
    pthread_barrier_wait(&g_barrier);

    while (!g_stop) {
        /* NO LOCK - intentionally racy to trigger kernel bugs */
        new_pattern = g_patterns[page_idx] + 1;
        g_patterns[page_idx] = new_pattern;
        memset(g_pages[page_idx], new_pattern, HPAGE_SIZE_2M);

        local_writes++;
        atomic_fetch_add(&g_write_count, 1);
    }

    DBG("Writer[%d] finished: writes=%lu", id, local_writes);
    return NULL;
}

static void *swapper_thread(void *arg)
{
    int i;
    int fd;
    char addr_str[32];

    fd = open_swap_pages();
    if (fd < 0) {
        INFO("Cannot open swap_pages, skipping swap");
        return NULL;
    }

    DBG("Swapper starting");
    pthread_barrier_wait(&g_barrier);

    while (!g_stop) {
        for (i = 0; i < NR_PAGES && !g_stop; i++) {
            /* Swap out - NO SLEEP, aggressive swapping */
            snprintf(addr_str, sizeof(addr_str), "0x%lx\n",
                     (unsigned long)g_pages[i]);
            if (write(fd, addr_str, strlen(addr_str)) > 0) {
                atomic_fetch_add(&g_swap_out_count, 1);
            }

            /* Swap in by accessing (triggers page fault) - IMMEDIATELY */
            *(volatile char *)g_pages[i];
            atomic_fetch_add(&g_swap_in_count, 1);
        }
    }

    DBG("Swapper finished");
    close(fd);
    return NULL;
}

static void test_mixed_rw_swap(void)
{
    pthread_t readers[NR_READERS];
    pthread_t writers[NR_WRITERS];
    pthread_t swapper;
    struct thread_arg rargs[NR_READERS];
    struct thread_arg wargs[NR_WRITERS];
    int i;

    printf("\nTest: Mixed Read/Write + Swap\n");
    INFO("Config: %d readers, %d writers, 1 swapper, %ds duration",
         NR_READERS, NR_WRITERS, TEST_DURATION);

    if (get_nr_hugepages() < NR_PAGES) {
        SKIP("Need at least %d hugepages", NR_PAGES);
        return;
    }

    if (alloc_test_pages() < 0)
        return;

    reset_stats();

    /* Initialize page mutexes */
    for (i = 0; i < NR_PAGES; i++) {
        pthread_mutex_init(&g_page_mutex[i], NULL);
    }

    pthread_barrier_init(&g_barrier, NULL,
                         NR_READERS + NR_WRITERS + 1 + 1); /* +1 for main, +1 for swapper */

    /* Create reader threads - each reads a specific page */
    for (i = 0; i < NR_READERS; i++) {
        rargs[i].id = i;
        rargs[i].page_idx = i % NR_PAGES;
        pthread_create(&readers[i], NULL, reader_thread, &rargs[i]);
    }

    /* Create writer threads */
    for (i = 0; i < NR_WRITERS; i++) {
        wargs[i].id = i;
        wargs[i].page_idx = i % NR_PAGES;
        pthread_create(&writers[i], NULL, writer_thread, &wargs[i]);
    }

    /* Create swapper thread */
    pthread_create(&swapper, NULL, swapper_thread, NULL);

    /* Wait for all threads to start */
    pthread_barrier_wait(&g_barrier);
    INFO("All threads started, running for %d seconds...", TEST_DURATION);

    /* Let it run */
    sleep(TEST_DURATION);
    g_stop = 1;

    /* Wait for completion */
    for (i = 0; i < NR_READERS; i++)
        pthread_join(readers[i], NULL);
    for (i = 0; i < NR_WRITERS; i++)
        pthread_join(writers[i], NULL);
    pthread_join(swapper, NULL);

    print_stats("Mixed R/W + Swap");

    /* Final verification */
    for (i = 0; i < NR_PAGES; i++) {
        if (verify_page(i) == 0)
            PASS("Page %d final verification OK", i);
    }

    free_test_pages();
    pthread_barrier_destroy(&g_barrier);

    /* Destroy page mutexes */
    for (i = 0; i < NR_PAGES; i++) {
        pthread_mutex_destroy(&g_page_mutex[i]);
    }

    PASS("Mixed test completed");
}

/*
 * Test 2: Race Swapin
 *
 * All pages are swapped out, then multiple threads race to access
 * the same page, triggering concurrent swapin handling.
 */

static pthread_mutex_t g_race_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_race_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_swap_out_done = 0;
static _Atomic uint32_t g_first_racer = 0;

static void *racer_thread(void *arg)
{
    struct thread_arg *t = arg;
    int id = t->id;
    int page_idx = t->page_idx;

    pthread_barrier_wait(&g_barrier);

    /* Wait for swap out to complete */
    pthread_mutex_lock(&g_race_mutex);
    while (!g_swap_out_done)
        pthread_cond_wait(&g_race_cond, &g_race_mutex);
    pthread_mutex_unlock(&g_race_mutex);

    /* Race to access the swapped-out page */
    uint32_t expected = 0;
    uint32_t my_id = id + 1;
    bool won = atomic_compare_exchange_strong(&g_first_racer, &expected, my_id);

    if (won)
        atomic_fetch_add(&g_race_wins, 1);

    /* Access triggers swapin */
    volatile char *p = g_pages[page_idx];
    char val = p[0];
    (void)val;

    /* Verify full page */
    if (verify_page(page_idx) != 0)
        atomic_fetch_add(&g_error_count, 1);

    atomic_fetch_add(&g_read_count, 1);
    return NULL;
}

static void test_race_swapin(void)
{
    pthread_t racers[NR_RACERS];
    struct thread_arg args[NR_RACERS];
    int fd;
    char addr_str[32];
    int i;

    printf("\nTest: Race Swapin\n");
    INFO("Config: %d racers competing for swapin", NR_RACERS);

    if (get_nr_hugepages() < 1) {
        SKIP("Need at least 1 hugepage");
        return;
    }

    if (alloc_test_pages() < 0)
        return;

    reset_stats();
    g_swap_out_done = 0;
    atomic_store(&g_first_racer, 0);

    pthread_barrier_init(&g_barrier, NULL, NR_RACERS + 1);

    /* Create racer threads - all racing for page 0 */
    for (i = 0; i < NR_RACERS; i++) {
        args[i].id = i;
        args[i].page_idx = 0;
        pthread_create(&racers[i], NULL, racer_thread, &args[i]);
    }

    pthread_barrier_wait(&g_barrier);
    INFO("Racers ready, swapping out page...");

    /* Swap out page 0 */
    trigger_swap(g_pages[0], 0);
    atomic_fetch_add(&g_swap_out_count, 1);
    PASS("Page swapped out");

    /* Signal racers to start */
    pthread_mutex_lock(&g_race_mutex);
    g_swap_out_done = 1;
    pthread_cond_broadcast(&g_race_cond);
    pthread_mutex_unlock(&g_race_mutex);

    /* Wait for all racers */
    for (i = 0; i < NR_RACERS; i++)
        pthread_join(racers[i], NULL);

    INFO("First racer: thread %u", atomic_load(&g_first_racer) - 1);
    INFO("Race wins: %lu", atomic_load(&g_race_wins));

    if (atomic_load(&g_error_count) == 0)
        PASS("No data corruption detected");
    else
        FAIL("Detected %lu errors", atomic_load(&g_error_count));

    free_test_pages();
    pthread_barrier_destroy(&g_barrier);
    PASS("Race test completed");
}

/*
 * Test 3: Loop Stress
 *
 * Repeatedly run mixed operations in cycles, continuously
 * swapping and verifying.
 */

static void *loop_worker_thread(void *arg)
{
    struct thread_arg *t = arg;
    int id = t->id;
    int page_idx = id % NR_PAGES;
    unsigned long local_ops = 0;
    unsigned char new_pattern;

    DBG("LoopWorker[%d] starting on page %d", id, page_idx);
    pthread_barrier_wait(&g_barrier);

    while (!g_stop) {
        /* NO LOCK - intentionally racy */
        if (local_ops & 1) {
            /* Write new pattern */
            new_pattern = g_patterns[page_idx] + 1;
            g_patterns[page_idx] = new_pattern;
            memset(g_pages[page_idx], new_pattern, HPAGE_SIZE_2M);
            atomic_fetch_add(&g_write_count, 1);
        } else {
            /* Read and verify */
            if (verify_page(page_idx) != 0)
                atomic_fetch_add(&g_error_count, 1);
            atomic_fetch_add(&g_read_count, 1);
        }
        local_ops++;
    }

    DBG("LoopWorker[%d] finished", id);
    return NULL;
}

static void *loop_swapper_thread(void *arg)
{
    int fd;
    int round = 0;
    int i;
    char addr_str[32];

    fd = open_swap_pages();
    if (fd < 0)
        return NULL;

    DBG("LoopSwapper starting");
    pthread_barrier_wait(&g_barrier);

    while (!g_stop) {
        for (i = 0; i < NR_PAGES && !g_stop; i++) {
            /* Swap out - NO SLEEP */
            snprintf(addr_str, sizeof(addr_str), "0x%lx\n",
                     (unsigned long)g_pages[i]);
            write(fd, addr_str, strlen(addr_str));
            atomic_fetch_add(&g_swap_out_count, 1);

            /* Swap in immediately */
            *(volatile char *)g_pages[i];
            atomic_fetch_add(&g_swap_in_count, 1);
        }

        round++;
        INFO("Loop round %d completed", round);
    }

    DBG("LoopSwapper finished");
    close(fd);
    return NULL;
}

static void test_loop_stress(void)
{
    pthread_t workers[NR_READERS];
    pthread_t swapper;
    struct thread_arg args[NR_READERS];
    int i;

    printf("\nTest: Loop Stress\n");
    INFO("Config: %d workers, %d rounds", NR_READERS, LOOP_ROUNDS);

    if (get_nr_hugepages() < NR_PAGES) {
        SKIP("Need at least %d hugepages", NR_PAGES);
        return;
    }

    if (alloc_test_pages() < 0)
        return;

    reset_stats();

    /* Initialize page mutexes */
    for (i = 0; i < NR_PAGES; i++) {
        pthread_mutex_init(&g_page_mutex[i], NULL);
    }

    pthread_barrier_init(&g_barrier, NULL, NR_READERS + 1 + 1);

    /* Create worker threads */
    for (i = 0; i < NR_READERS; i++) {
        args[i].id = i;
        pthread_create(&workers[i], NULL, loop_worker_thread, &args[i]);
    }

    /* Create swapper */
    pthread_create(&swapper, NULL, loop_swapper_thread, NULL);

    pthread_barrier_wait(&g_barrier);
    INFO("Starting stress loop...");

    /* Run for fixed duration */
    sleep(TEST_DURATION);
    g_stop = 1;

    for (i = 0; i < NR_READERS; i++)
        pthread_join(workers[i], NULL);
    pthread_join(swapper, NULL);

    print_stats("Loop Stress");

    /* Final verification of all pages */
    int final_errors = 0;
    for (i = 0; i < NR_PAGES; i++) {
        if (verify_page(i) != 0)
            final_errors++;
    }

    if (final_errors == 0)
        PASS("All pages verified OK after stress");
    else
        FAIL("%d pages failed final verification", final_errors);

    free_test_pages();
    pthread_barrier_destroy(&g_barrier);

    /* Destroy page mutexes */
    for (i = 0; i < NR_PAGES; i++) {
        pthread_mutex_destroy(&g_page_mutex[i]);
    }

    PASS("Loop stress completed");
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

    test_mixed_rw_swap();
    test_race_swapin();
    test_loop_stress();

    printf("\n=== Summary ===\n");
    printf("  Passed:  %d\n", passed);
    printf("  Failed:  %d\n", failed);
    printf("  Skipped: %d\n", skipped);

    return failed > 0 ? 1 : 0;
}
