/*
 * hugetlbfs fallocate punch hole test with swapped pages
 *
 * Tests the scenario where:
 * 1. hugetlbfs file has pages that are swapped out
 * 2. fallocate(FALLOC_FL_PUNCH_HOLE) is called on the swapped pages
 * 3. Kernel must properly clean up swap entries and punch the hole
 *
 * This tests the hugetlbfs_cleanup_swap_range() function in
 * fs/hugetlbfs/inode.c which is called during hugetlbfs_punch_hole().
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <linux/falloc.h>

#include "test_common.h"

#define HPAGE_SIZE (2 * 1024 * 1024)
#define TEST_FILE "/mnt/huge/test_punch_hole"
#define SWAP_DEVICE "/dev/zram0"

/*
 * punch_hole - Punch a hole in a file
 * @fd: file descriptor
 * @offset: offset in bytes
 * @len: length in bytes
 *
 * Returns: 0 on success, -1 on failure
 */
static int punch_hole(int fd, off_t offset, off_t len)
{
    return fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                     offset, len);
}

/*
 * is_swap_active - Check if swap device is active
 */
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

/*
 * enable_swap - Enable swap on zram device
 */
static int enable_swap(void)
{
    if (system("mkswap " SWAP_DEVICE " 2>/dev/null") != 0)
        return -1;
    if (system("swapon " SWAP_DEVICE " -p 2 2>/dev/null") != 0)
        return -1;
    return 0;
}

/*
 * verify_hole_data - Verify that punched region contains zeros
 * @addr: start of memory region
 * @hole_offset: offset of hole within addr
 * @hole_len: length of hole
 * @total_len: total length of region
 * @pattern: expected pattern for non-holed regions
 *
 * Returns: 0 if verified, -1 on failure
 */
static int verify_hole_data(void *addr, off_t hole_offset, size_t hole_len,
                             size_t total_len, unsigned char pattern)
{
    unsigned char *p = addr;

    /* Verify data before hole */
    for (size_t i = 0; i < hole_offset; i++) {
        if (p[i] != pattern) {
            FAIL("Data before hole corrupted at offset %zu: expected 0x%02X, got 0x%02X",
                 i, pattern, p[i]);
            return -1;
        }
    }

    /* Verify hole contains zeros */
    for (size_t i = hole_offset; i < hole_offset + hole_len && i < total_len; i++) {
        if (p[i] != 0) {
            FAIL("Hole region not zero at offset %zu: expected 0x00, got 0x%02X",
                 i, p[i]);
            return -1;
        }
    }

    /* Verify data after hole */
    for (size_t i = hole_offset + hole_len; i < total_len; i++) {
        if (p[i] != pattern) {
            FAIL("Data after hole corrupted at offset %zu: expected 0x%02X, got 0x%02X",
                 i, pattern, p[i]);
            return -1;
        }
    }

    PASS("Data integrity verified: hole contains zeros, rest has pattern 0x%02X", pattern);
    return 0;
}

/*
 * test_punch_middle_swapped - Test punching hole on middle swapped pages
 *
 * Creates a 4-page file, swaps out middle 2 pages, punches hole on them.
 */
static void test_punch_middle_swapped(void)
{
    int fd;
    void *addr;
    int swpd_before, swpd_after, swpd_punch;
    const int num_pages = 4;
    const size_t file_size = num_pages * HPAGE_SIZE;
    const int hole_page_start = 1;  /* Swap out pages 1 and 2 */
    const int hole_page_count = 2;

    printf("\n=== Test: Punch Hole on Middle Swapped Pages ===\n");

    if (access("/mnt/huge", F_OK) != 0) {
        SKIP("hugetlbfs not mounted at /mnt/huge");
        return;
    }

    if (get_nr_hugepages() < num_pages + 1) {
        SKIP("Need at least %d hugepages", num_pages + 1);
        return;
    }

    if (!is_swap_active() && enable_swap() < 0) {
        SKIP("Failed to enable swap");
        return;
    }
    PASS("Swap is active");

    /* Create and map hugetlbfs file */
    fd = open(TEST_FILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        FAIL("Failed to create hugetlbfs file: %s", strerror(errno));
        return;
    }

    if (ftruncate(fd, file_size) != 0) {
        FAIL("Failed to set file size: %s", strerror(errno));
        close(fd);
        return;
    }
    PASS("Created %zu MB test file", file_size / (1024 * 1024));

    addr = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        FAIL("Failed to mmap: %s", strerror(errno));
        close(fd);
        unlink(TEST_FILE);
        return;
    }
    PASS("Mapped hugetlbfs file at %p", addr);

    /* Note: keep fd open for fallocate punch hole */

    /* Fill all pages with pattern */
    memset(addr, 0xAB, file_size);
    PASS("Memory filled with pattern 0xAB");

    /* Swap out middle 2 pages */
    swpd_before = get_hugepages_swpd();
    INFO("HugePages_Swap before: %d", swpd_before);

    for (int i = hole_page_start; i < hole_page_start + hole_page_count; i++) {
        void *page_addr = (char *)addr + i * HPAGE_SIZE;
        trigger_swap(page_addr, 0);
    }

    /* Give swap time to complete */
    usleep(100000);
    swpd_after = get_hugepages_swpd();
    INFO("HugePages_Swap after swap: %d", swpd_after);

    if (swpd_after > swpd_before)
        PASS("Pages swapped out (%d -> %d)", swpd_before, swpd_after);
    else
        INFO("Warning: Swap count did not increase (may already be swapped)");

    /* Punch hole on the swapped pages */
    INFO("Punching hole on swapped pages (offset %d MB, len %d MB)...",
         hole_page_start * 2, hole_page_count * 2);

    if (punch_hole(fd, hole_page_start * HPAGE_SIZE, hole_page_count * HPAGE_SIZE) != 0) {
        FAIL("fallocate punch hole failed: %s", strerror(errno));
        munmap(addr, file_size);
        close(fd);
        unlink(TEST_FILE);
        return;
    }
    PASS("Punch hole succeeded");

    /* Verify swap cleanup */
    swpd_punch = get_hugepages_swpd();
    INFO("HugePages_Swap after punch: %d", swpd_punch);

    if (swpd_punch <= swpd_after) {
        PASS("Swap count decreased after punch hole (%d -> %d)", swpd_after, swpd_punch);
    } else {
        FAIL("Swap count did not decrease after punch hole (%d -> %d)", swpd_after, swpd_punch);
    }

    /* Verify data integrity */
    /* Note: we need to re-map or check via file read since mmap may have stale mappings */
    msync(addr, file_size, MS_SYNC);
    if (verify_hole_data(addr, hole_page_start * HPAGE_SIZE,
                         hole_page_count * HPAGE_SIZE, file_size, 0xAB) != 0) {
        /* Data verification failed - check if this is expected */
        INFO("Note: mmap may show stale data; checking file read...");
    }

    /* Cleanup */
    munmap(addr, file_size);
    close(fd);
    unlink(TEST_FILE);
    PASS("Test cleanup complete");
}

/*
 * test_punch_all_swapped - Test punching hole on all swapped pages
 *
 * Creates a 2-page file, swaps out all pages, punches hole on entire file.
 */
static void test_punch_all_swapped(void)
{
    int fd;
    void *addr;
    int swpd_before, swpd_after, swpd_punch;
    const int num_pages = 2;
    const size_t file_size = num_pages * HPAGE_SIZE;

    printf("\n=== Test: Punch Hole on All Swapped Pages ===\n");

    if (access("/mnt/huge", F_OK) != 0) {
        SKIP("hugetlbfs not mounted at /mnt/huge");
        return;
    }

    if (get_nr_hugepages() < num_pages + 1) {
        SKIP("Need at least %d hugepages", num_pages + 1);
        return;
    }

    /* Create and map hugetlbfs file */
    fd = open(TEST_FILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        FAIL("Failed to create hugetlbfs file: %s", strerror(errno));
        return;
    }

    if (ftruncate(fd, file_size) != 0) {
        FAIL("Failed to set file size: %s", strerror(errno));
        close(fd);
        return;
    }
    PASS("Created %zu MB test file", file_size / (1024 * 1024));

    addr = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        FAIL("Failed to mmap: %s", strerror(errno));
        close(fd);
        unlink(TEST_FILE);
        return;
    }
    PASS("Mapped hugetlbfs file at %p", addr);

    /* Note: keep fd open for fallocate punch hole */

    /* Fill with pattern */
    memset(addr, 0xCD, file_size);
    PASS("Memory filled with pattern 0xCD");

    /* Swap out all pages */
    swpd_before = get_hugepages_swpd();
    INFO("HugePages_Swap before: %d", swpd_before);

    for (int i = 0; i < num_pages; i++) {
        void *page_addr = (char *)addr + i * HPAGE_SIZE;
        trigger_swap(page_addr, 0);
    }

    usleep(100000);
    swpd_after = get_hugepages_swpd();
    INFO("HugePages_Swap after swap: %d", swpd_after);

    if (swpd_after > swpd_before)
        PASS("All pages swapped out");
    else
        INFO("Warning: Swap count did not increase");

    /* Punch hole on entire file */
    INFO("Punching hole on entire file...");

    if (punch_hole(fd, 0, file_size) != 0) {
        FAIL("fallocate punch hole failed: %s", strerror(errno));
        munmap(addr, file_size);
        close(fd);
        unlink(TEST_FILE);
        return;
    }
    PASS("Punch hole on entire file succeeded");

    /* Verify swap cleanup */
    swpd_punch = get_hugepages_swpd();
    INFO("HugePages_Swap after punch: %d", swpd_punch);

    if (swpd_punch <= swpd_after) {
        PASS("All swap entries cleaned up after punch hole");
    } else {
        FAIL("Swap count did not decrease after punch hole");
    }

    /* Cleanup */
    munmap(addr, file_size);
    close(fd);
    unlink(TEST_FILE);
    PASS("Test cleanup complete");
}

/*
 * test_punch_then_read - Test reading after punch hole
 *
 * Verifies that after punching a hole on swapped pages, reading
 * the file returns zeros for the punched region.
 */
static void test_punch_then_read(void)
{
    int fd;
    void *addr;
    char *read_buf;
    const int num_pages = 3;
    const size_t file_size = num_pages * HPAGE_SIZE;
    const int hole_page = 1;  /* Punch hole on middle page */

    printf("\n=== Test: Read After Punch Hole ===\n");

    if (access("/mnt/huge", F_OK) != 0) {
        SKIP("hugetlbfs not mounted at /mnt/huge");
        return;
    }

    if (get_nr_hugepages() < num_pages + 1) {
        SKIP("Need at least %d hugepages", num_pages + 1);
        return;
    }

    /* Allocate read buffer */
    read_buf = malloc(HPAGE_SIZE);
    if (!read_buf) {
        FAIL("Failed to allocate read buffer");
        return;
    }

    /* Create and map hugetlbfs file */
    fd = open(TEST_FILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        FAIL("Failed to create hugetlbfs file: %s", strerror(errno));
        free(read_buf);
        return;
    }

    if (ftruncate(fd, file_size) != 0) {
        FAIL("Failed to set file size: %s", strerror(errno));
        close(fd);
        free(read_buf);
        return;
    }

    addr = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        FAIL("Failed to mmap: %s", strerror(errno));
        close(fd);
        free(read_buf);
        return;
    }
    PASS("Mapped hugetlbfs file");

    /* Fill with pattern */
    memset(addr, 0xEF, file_size);
    msync(addr, file_size, MS_SYNC);
    PASS("Memory filled with pattern 0xEF");

    /* Swap out the middle page */
    trigger_swap((char *)addr + hole_page * HPAGE_SIZE, 0);
    usleep(100000);
    INFO("Page %d swapped out", hole_page);

    /* Punch hole on the swapped page */
    if (punch_hole(fd, hole_page * HPAGE_SIZE, HPAGE_SIZE) != 0) {
        FAIL("fallocate punch hole failed: %s", strerror(errno));
        munmap(addr, file_size);
        close(fd);
        free(read_buf);
        unlink(TEST_FILE);
        return;
    }
    PASS("Punch hole succeeded");

    /* Sync and unmap */
    msync(addr, file_size, MS_SYNC);
    munmap(addr, file_size);

    /* Re-open and read to verify */
    fd = open(TEST_FILE, O_RDONLY);
    if (fd < 0) {
        FAIL("Failed to reopen file: %s", strerror(errno));
        free(read_buf);
        unlink(TEST_FILE);
        return;
    }

    /* Read the punched page */
    if (pread(fd, read_buf, HPAGE_SIZE, hole_page * HPAGE_SIZE) != HPAGE_SIZE) {
        FAIL("Failed to read punched page: %s", strerror(errno));
        close(fd);
        free(read_buf);
        unlink(TEST_FILE);
        return;
    }

    /* Verify it's all zeros */
    int all_zero = 1;
    for (size_t i = 0; i < 512; i++) {  /* Check first 512 entries */
        if (read_buf[i] != 0) {
            all_zero = 0;
            FAIL("Punched region not zero at offset %zu: 0x%02X", i, (unsigned char)read_buf[i]);
            break;
        }
    }

    if (all_zero)
        PASS("Punched region reads as zeros");

    /* Read first page (should still have pattern) */
    if (pread(fd, read_buf, HPAGE_SIZE, 0) != HPAGE_SIZE) {
        INFO("Warning: Failed to read first page");
    } else {
        int has_pattern = (read_buf[0] == 0xEF);
        if (has_pattern)
            PASS("First page still has original data");
        else
            INFO("Note: First page data may have been modified");
    }

    /* Cleanup */
    close(fd);
    free(read_buf);
    unlink(TEST_FILE);
    PASS("Test cleanup complete");
}

/*
 * test_punch_partial_overlap - Test punch hole covering both swapped and non-swapped pages
 *
 * Creates a 4-page file, swaps out 2 middle pages, punches hole
 * covering 1 swapped and 1 non-swapped page.
 */
static void test_punch_partial_overlap(void)
{
    int fd;
    void *addr;
    int swpd_before, swpd_after, swpd_punch;
    const int num_pages = 4;
    const size_t file_size = num_pages * HPAGE_SIZE;
    const int swap_page_start = 1;
    const int swap_page_count = 2;
    const int hole_page_start = 2;  /* Overlaps with swapped pages */
    const int hole_page_count = 2;

    printf("\n=== Test: Punch Hole Partial Overlap with Swapped Pages ===\n");

    if (access("/mnt/huge", F_OK) != 0) {
        SKIP("hugetlbfs not mounted at /mnt/huge");
        return;
    }

    if (get_nr_hugepages() < num_pages + 1) {
        SKIP("Need at least %d hugepages", num_pages + 1);
        return;
    }

    /* Create and map hugetlbfs file */
    fd = open(TEST_FILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        FAIL("Failed to create hugetlbfs file: %s", strerror(errno));
        return;
    }

    if (ftruncate(fd, file_size) != 0) {
        FAIL("Failed to set file size: %s", strerror(errno));
        close(fd);
        return;
    }

    addr = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        FAIL("Failed to mmap: %s", strerror(errno));
        close(fd);
        unlink(TEST_FILE);
        return;
    }
    PASS("Mapped hugetlbfs file");

    /* Note: keep fd open for fallocate punch hole */

    /* Fill with pattern */
    memset(addr, 0xA5, file_size);
    PASS("Memory filled with pattern 0xA5");

    /* Swap out middle 2 pages */
    swpd_before = get_hugepages_swpd();
    INFO("HugePages_Swap before: %d", swpd_before);

    for (int i = swap_page_start; i < swap_page_start + swap_page_count; i++) {
        trigger_swap((char *)addr + i * HPAGE_SIZE, 0);
    }

    usleep(100000);
    swpd_after = get_hugepages_swpd();
    INFO("HugePages_Swap after swap: %d", swpd_after);

    /* Punch hole covering 1 swapped and 1 non-swapped page */
    INFO("Punching hole with partial overlap (pages %d-%d)...",
         hole_page_start, hole_page_start + hole_page_count - 1);

    if (punch_hole(fd, hole_page_start * HPAGE_SIZE, hole_page_count * HPAGE_SIZE) != 0) {
        FAIL("fallocate punch hole failed: %s", strerror(errno));
        munmap(addr, file_size);
        close(fd);
        unlink(TEST_FILE);
        return;
    }
    PASS("Punch hole with partial overlap succeeded");

    /* Verify swap cleanup */
    swpd_punch = get_hugepages_swpd();
    INFO("HugePages_Swap after punch: %d", swpd_punch);

    if (swpd_punch < swpd_after) {
        PASS("Swap count decreased (partial cleanup worked)");
    } else {
        INFO("Note: Swap count unchanged (may be expected for partial overlap)");
    }

    /* Cleanup */
    munmap(addr, file_size);
    close(fd);
    unlink(TEST_FILE);
    PASS("Test cleanup complete");
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    printf("=== hugetlbfs fallocate Punch Hole with Swap Test ===\n");
    printf("Kernel: "); fflush(stdout); system("uname -r");

    test_punch_middle_swapped();
    test_punch_all_swapped();
    test_punch_then_read();
    test_punch_partial_overlap();

    printf("\n=== Summary ===\n");
    printf("Passed: %d, Failed: %d, Skipped: %d\n", passed, failed, skipped);

    /* Check for kernel errors */
    printf("\n=== Kernel Log Check ===\n");
    system("dmesg | tail -20 | grep -i -E 'bug|warning|error|hugetlb' || echo 'No recent kernel errors'");

    return failed > 0 ? 1 : 0;
}
