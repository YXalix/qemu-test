/*
 * Simple HugeTLB Swap Test - Deterministic, not racy
 *
 * This test verifies basic hugetlb swap functionality without
 * intentional races between readers and writers.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "test_common.h"

#define HPAGE_SIZE (2 * 1024 * 1024)

/* Test 1: Basic swap out/in for anonymous huge pages */
void test_anon_swap_simple(void)
{
    void *addr;
    int swpd_before, swpd_after;
    int free_before, free_after;
    unsigned char *p;
    size_t i;

    printf("\nTest: Anonymous Hugepage Simple Swap\n");

    if (get_nr_hugepages() < 2) {
        SKIP("Need at least 2 hugepages");
        return;
    }

    /* Allocate and fill with pattern */
    addr = alloc_hugetlb(HPAGE_SIZE);
    if (!addr) {
        FAIL("Failed to allocate hugepage");
        return;
    }

    p = addr;
    for (i = 0; i < HPAGE_SIZE; i++)
        p[i] = (unsigned char)(i & 0xFF);

    PASS("Allocated and filled 2MB hugepage");

    /* Record state before swap */
    swpd_before = get_hugepages_swpd();
    free_before = get_free_hugepages();
    INFO("Before swap: HugePages_Swap=%d, free=%d", swpd_before, free_before);

    /* Swap out */
    if (trigger_swap(addr, 0) < 0) {
        free_hugetlb(addr, HPAGE_SIZE);
        return;
    }

    swpd_after = get_hugepages_swpd();
    free_after = get_free_hugepages();
    INFO("After swap: HugePages_Swap=%d, free=%d", swpd_after, free_after);

    if (swpd_after > swpd_before)
        PASS("Page swapped out");
    else
        INFO("Swap may still be pending");

    /* Verify data is still correct (triggers swapin) */
    int errors = 0;
    for (i = 0; i < HPAGE_SIZE && errors < 5; i++) {
        if (p[i] != (unsigned char)(i & 0xFF)) {
            FAIL("Data corruption at offset %zu: expected 0x%02X, got 0x%02X",
                 i, (unsigned char)(i & 0xFF), p[i]);
            errors++;
        }
    }

    if (errors == 0)
        PASS("Data verified after swapin");

    free_hugetlb(addr, HPAGE_SIZE);
    PASS("Test completed");
}

/* Test 2: Sequential swap out/in, no concurrent access */
void test_sequential_swap(void)
{
    void *addr;
    int round;
    unsigned char pattern;
    unsigned char *p;
    size_t i;

    printf("\nTest: Sequential Swap\n");

    if (get_nr_hugepages() < 2) {
        SKIP("Need at least 2 hugepages");
        return;
    }

    addr = alloc_hugetlb(HPAGE_SIZE);
    if (!addr) {
        FAIL("Failed to allocate hugepage");
        return;
    }

    p = addr;

    for (round = 0; round < 5; round++) {
        pattern = 0xA0 + round;
        memset(addr, pattern, HPAGE_SIZE);

        if (trigger_swap(addr, 0) < 0)
            break;

        /* Access to trigger swapin */
        volatile char x = p[0];
        (void)x;

        /* Verify */
        int errors = 0;
        for (i = 0; i < HPAGE_SIZE && errors < 3; i++) {
            if (p[i] != pattern) {
                FAIL("Round %d: corruption at %zu: expected 0x%02X, got 0x%02X",
                     round, i, pattern, p[i]);
                errors++;
            }
        }

        if (errors == 0)
            PASS("Round %d: OK", round);
    }

    free_hugetlb(addr, HPAGE_SIZE);
    PASS("Sequential test completed");
}

/* Test 3: Fork and swap - child accesses swapped page */
void test_fork_swap(void)
{
    void *addr;
    pid_t pid;
    int status;
    unsigned char *p;

    printf("\nTest: Fork with Swap\n");

    if (get_nr_hugepages() < 2) {
        SKIP("Need at least 2 hugepages");
        return;
    }

    addr = alloc_hugetlb(HPAGE_SIZE);
    if (!addr) {
        FAIL("Failed to allocate hugepage");
        return;
    }

    p = addr;
    memset(addr, 0x42, HPAGE_SIZE);

    /* Swap out parent page */
    trigger_swap(addr, 0);

    /* Fork - child will inherit swapped entry */
    pid = fork();
    if (pid < 0) {
        FAIL("Fork failed");
        free_hugetlb(addr, HPAGE_SIZE);
        return;
    }

    if (pid == 0) {
        /* Child - access swapped page */
        int errors = 0;
        size_t i;
        for (i = 0; i < HPAGE_SIZE && errors < 3; i++) {
            if (p[i] != 0x42) {
                printf("[FAIL] Child: corruption at %zu: expected 0x42, got 0x%02X\n",
                       i, p[i]);
                errors++;
            }
        }
        if (errors == 0)
            printf("[PASS] Child: Data verified\n");
        exit(errors > 0 ? 1 : 0);
    }

    /* Parent - wait for child */
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        PASS("Child verified data correctly");
    else
        FAIL("Child detected errors");

    /* Parent also verify */
    int errors = 0;
    size_t i;
    for (i = 0; i < HPAGE_SIZE && errors < 3; i++) {
        if (p[i] != 0x42) {
            FAIL("Parent: corruption at %zu", i);
            errors++;
        }
    }
    if (errors == 0)
        PASS("Parent verified data correctly");

    free_hugetlb(addr, HPAGE_SIZE);
    PASS("Fork test completed");
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    printf("=== HugeTLB Swap Simple Tests ===\n");
    printf("Kernel: "); fflush(stdout); system("uname -r");

    test_anon_swap_simple();
    test_sequential_swap();
    test_fork_swap();

    printf("\n=== Summary ===\n");
    printf("  Passed:  %d\n", passed);
    printf("  Failed:  %d\n", failed);
    printf("  Skipped: %d\n", skipped);

    return failed > 0 ? 1 : 0;
}
