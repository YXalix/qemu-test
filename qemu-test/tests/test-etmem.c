// SPDX-License-Identifier: GPL-2.0
/*
 * ETMEM (Enhanced Tiered Memory) Swap Test Program
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

static int tests_passed = 0;
static int tests_failed = 0;
static int tests_skipped = 0;

#define TEST_PASS(fmt, ...) do { \
	tests_passed++; \
	printf("  [PASS] " fmt "\n", ##__VA_ARGS__); \
} while(0)

#define TEST_FAIL(fmt, ...) do { \
	tests_failed++; \
	printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); \
} while(0)

#define TEST_SKIP(fmt, ...) do { \
	tests_skipped++; \
	printf("  [SKIP] " fmt "\n", ##__VA_ARGS__); \
} while(0)

#define TEST_INFO(fmt, ...) do { \
	printf("  [INFO] " fmt "\n", ##__VA_ARGS__); \
} while(0)

static void print_header(void)
{
	printf("=== ETMEM Swap E2E Tests ===\n\n");
	printf("Architecture: arm64\n");
	printf("Kernel: ");
	fflush(stdout);
	system("uname -r");
	printf("\n");
}

static void test_etmem_config(void)
{
	FILE *fp;
	char line[256];
	int found = 0;

	printf("Test 1: ETMEM Kernel Configuration\n");

	fp = popen("zcat /proc/config.gz 2>/dev/null", "r");
	if (!fp) {
		TEST_SKIP("Kernel config not accessible");
		printf("\n");
		return;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "CONFIG_ETMEM=y")) {
			TEST_PASS("CONFIG_ETMEM=y enabled");
			found = 1;
			break;
		}
	}

	if (!found)
		TEST_FAIL("CONFIG_ETMEM not found");

	pclose(fp);
	printf("\n");
}

static void test_kernel_swap_enable(void)
{
	const char *sysfs_path = "/sys/kernel/mm/swap/kernel_swap_enable";
	FILE *fp;
	char val[16];

	printf("Test 2: Kernel Swap Enable Interface\n");

	fp = fopen(sysfs_path, "r");
	if (!fp) {
		TEST_FAIL("kernel_swap_enable not found");
		printf("\n");
		return;
	}

	if (fgets(val, sizeof(val), fp)) {
		val[strcspn(val, "\n")] = 0;
		TEST_PASS("sysfs interface exists");
	}
	fclose(fp);
	printf("\n");
}

static void test_swap_pages_interface(void)
{
	const char *swap_pages = "/proc/self/swap_pages";
	int fd;

	printf("Test 3: /proc/<pid>/swap_pages Interface\n");

	fd = open(swap_pages, O_WRONLY);
	if (fd < 0) {
		TEST_FAIL("swap_pages not found: %s", strerror(errno));
		printf("\n");
		return;
	}

	TEST_PASS("swap_pages interface exists");
	close(fd);
	printf("\n");
}

/* Get current VmSwap value from /proc/self/status (in kB) */
static unsigned long get_vmswap(void)
{
	FILE *fp;
	char line[256];
	unsigned long vmswap = 0;

	fp = fopen("/proc/self/status", "r");
	if (!fp)
		return 0;

	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "VmSwap:", 7) == 0) {
			sscanf(line, "VmSwap: %lu", &vmswap);
			break;
		}
	}
	fclose(fp);
	return vmswap;
}

#define NUM_HUGEPAGES 3
#define HPAGE_SIZE (2 * 1024 * 1024)  /* 2MB hugepage on arm64 */

/* Get current free hugepage count for 2MB hugepages */
static int get_free_hugepages(void)
{
	FILE *fp;
	int free_hp = -1;

	fp = fopen("/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages", "r");
	if (fp) {
		char line[256];
		if (fgets(line, sizeof(line), fp))
			free_hp = atoi(line);
		fclose(fp);
	}
	return free_hp;
}

/* Test 4: Anonymous HugeTLB allocation via mmap with MAP_HUGETLB */
static void test_anonymous_hugetlb(void)
{
	void *addr;
	size_t len = NUM_HUGEPAGES * HPAGE_SIZE;  /* 6MB total (3 x 2MB) */
	int fd;
	char addr_str[32];
	unsigned long swap_before, swap_after;
	int swap_triggered_count = 0;
	int reserved = 0;
	int free_hp_before, free_hp_after;

	printf("Test 4: Anonymous HugeTLB Allocation with Swap (6MB)\n");

	/* Check if we have enough hugepages reserved */
	fd = open("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages", O_RDONLY);
	if (fd >= 0) {
		char line[256];
		if (read(fd, line, sizeof(line)) > 0)
			reserved = atoi(line);
		close(fd);
	}

	if (reserved < NUM_HUGEPAGES) {
		TEST_SKIP("Not enough hugepages reserved (need %d, have %d)",
			  NUM_HUGEPAGES, reserved);
		printf("\n");
		return;
	}

	/* Allocate anonymous hugepages using MAP_HUGETLB */
	addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

	if (addr == MAP_FAILED) {
		if (errno == ENOMEM) {
			TEST_SKIP("No hugepages available (need %d x 2MB)", NUM_HUGEPAGES);
		} else {
			TEST_FAIL("mmap MAP_HUGETLB failed: %s", strerror(errno));
		}
		printf("\n");
		return;
	}

	TEST_PASS("Allocated %dMB (%d x 2MB) anonymous hugepages at %p",
		  NUM_HUGEPAGES * 2, NUM_HUGEPAGES, addr);

	/* Touch all pages to ensure they're backed */
	memset(addr, 0xAA, len);
	TEST_PASS("Successfully touched all hugepage memory");

	/* Get free hugepages before swap - pages should be allocated from pool */
	free_hp_before = get_free_hugepages();
	if (free_hp_before < 0) {
		TEST_SKIP("Cannot read free_hugepages");
		munmap(addr, len);
		printf("\n");
		return;
	}
	TEST_INFO("Free hugepages before swap: %d", free_hp_before);

	/* Get swap usage before triggering swap */
	swap_before = get_vmswap();
	TEST_INFO("VmSwap before: %lu kB", swap_before);

	/* Trigger swap via ETMEM for all hugepages */
	fd = open("/proc/self/swap_pages", O_WRONLY);
	if (fd >= 0) {
		int i;

		for (i = 0; i < NUM_HUGEPAGES; i++) {
			/* Write middle of each hugepage address */
			uintptr_t swap_addr = (uintptr_t)addr + (i * HPAGE_SIZE) + HPAGE_SIZE / 2;
			snprintf(addr_str, sizeof(addr_str), "0x%lx\n", swap_addr);

			if (write(fd, addr_str, strlen(addr_str)) >= 0) {
				swap_triggered_count++;
			}
		}

		if (swap_triggered_count > 0) {
			TEST_PASS("Triggered swap for %d/%d hugepage addresses",
				  swap_triggered_count, NUM_HUGEPAGES);
		} else {
			TEST_INFO("Swap trigger failed for all addresses: %s", strerror(errno));
		}
		close(fd);
	} else {
		TEST_INFO("Cannot open swap_pages: %s", strerror(errno));
	}

	/* Verify swap actually happened */
	if (swap_triggered_count > 0) {
		TEST_INFO("Waiting for swap to complete...");
		usleep(1000000);  /* Wait 1 second for swap to complete */

		/* Check free hugepages after swap - should INCREASE */
		free_hp_after = get_free_hugepages();
		TEST_INFO("Free hugepages after swap: %d", free_hp_after);

		swap_after = get_vmswap();
		TEST_INFO("VmSwap after: %lu kB", swap_after);

		/* Verify both conditions for complete swap verification */
		int free_hp_increased = (free_hp_after > free_hp_before);
		int vmswap_increased = (swap_after > swap_before);

		if (free_hp_increased && vmswap_increased) {
			TEST_PASS("Swap verified: Free hugepages increased from %d to %d "
				  "(pages returned to pool)", free_hp_before, free_hp_after);
			TEST_PASS("Swap verified: VmSwap increased from %lu kB to %lu kB",
				  swap_before, swap_after);
		} else {
			/* Both must pass - fail if either didn't */
			if (!free_hp_increased) {
				TEST_FAIL("Free hugepages did not increase (%d before, %d after)",
					  free_hp_before, free_hp_after);
			}
			if (!vmswap_increased) {
				TEST_FAIL("VmSwap did not increase (%lu kB before, %lu kB after)",
					  swap_before, swap_after);
			}
		}
	}

	munmap(addr, len);
	printf("\n");
}

#define HPAGE_SYSFS_PATH "/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"

/* Set nr_hugepages to specified count */
static int set_nr_hugepages(int count)
{
	FILE *fp = fopen(HPAGE_SYSFS_PATH, "w");
	if (!fp) return -1;
	fprintf(fp, "%d\n", count);
	fclose(fp);
	return 0;
}

/* Get current nr_hugepages */
static int get_nr_hugepages(void)
{
	FILE *fp = fopen(HPAGE_SYSFS_PATH, "r");
	int nr = 0;
	if (fp) {
		fscanf(fp, "%d", &nr);
		fclose(fp);
	}
	return nr;
}

/* Verify memory contains expected pattern */
static int verify_pattern(void *addr, size_t len, unsigned char pattern)
{
	unsigned char *p = addr;
	size_t i;
	for (i = 0; i < len; i++) {
		if (p[i] != pattern) {
			TEST_FAIL("Data corruption at offset %zu: expected 0x%02X, got 0x%02X",
				  i, pattern, p[i]);
			return -1;
		}
	}
	return 0;
}

/* Trigger swapin by reading from swapped pages */
static void trigger_swapin(void *addr, size_t len)
{
	volatile unsigned char *p = addr;
	size_t i;
	unsigned char val;

	/* Read from each page to trigger swapin */
	for (i = 0; i < len; i += HPAGE_SIZE) {
		val = p[i];  /* Read first byte of each hugepage */
		(void)val;   /* Suppress unused warning */
	}
}

/*
 * Test 5: Hugetlb Swap Data Integrity Test
 *
 * This test verifies data integrity across swap operations:
 * 1. Reserve only 6MB (3 hugepages)
 * 2. Allocate 6MB and write pattern 0xAA
 * 3. Swap out first 6MB (pages move to swap, physical memory freed)
 * 4. Allocate another 6MB (reuses freed physical pages) and write pattern 0xBB
 * 5. Unmap second 6MB (free physical pages)
 * 6. Trigger swapin for first 6MB
 * 7. Verify first 6MB data (0xAA) after swapin
 *
 * Note: Only 6MB resident in physical memory at any time to avoid OOM.
 */
static void test_hugetlb_swap_data_integrity(void)
{
	void *addr1 = NULL, *addr2 = NULL;
	size_t len = NUM_HUGEPAGES * HPAGE_SIZE;  /* 6MB = 3 x 2MB */
	int orig_nr_hugepages;
	int fd, i;
	char addr_str[32];
	unsigned long swap_before, swap_after;
	int free_hp_before, free_hp_after;
	int swap_triggered;

#define PATTERN_FIRST  0xAA
#define PATTERN_SECOND 0xBB

	printf("Test 5: Hugetlb Swap Data Integrity (12MB with 6MB limit)\n");

	/* Save original nr_hugepages */
	orig_nr_hugepages = get_nr_hugepages();
	if (orig_nr_hugepages < NUM_HUGEPAGES) {
		TEST_SKIP("Need at least %d hugepages reserved (have %d)", NUM_HUGEPAGES, orig_nr_hugepages);
		printf("\n");
		return;
	}
	TEST_INFO("Original nr_hugepages: %d", orig_nr_hugepages);

	/* Set nr_hugepages to NUM_HUGEPAGES (only 6MB available) */
	if (set_nr_hugepages(NUM_HUGEPAGES) != 0) {
		TEST_FAIL("Failed to set nr_hugepages to %d", NUM_HUGEPAGES);
		printf("\n");
		return;
	}
	TEST_INFO("Set nr_hugepages to %d (6MB available)", NUM_HUGEPAGES);

	/* Wait a bit for pages to settle */
	usleep(100000);

	/* Step 1: Allocate first 6MB */
	addr1 = mmap(NULL, len, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

	if (addr1 == MAP_FAILED) {
		TEST_FAIL("Failed to mmap first 6MB: %s", strerror(errno));
		goto cleanup;
	}
	TEST_PASS("Allocated first 6MB at %p", addr1);

	/* Step 2: Write pattern 0xAA to first 6MB */
	memset(addr1, PATTERN_FIRST, len);
	if (verify_pattern(addr1, len, PATTERN_FIRST) != 0) {
		goto cleanup;
	}
	TEST_PASS("Written pattern 0x%02X to first 6MB", PATTERN_FIRST);

	/* Step 3: Swap out first 6MB */
	free_hp_before = get_free_hugepages();
	swap_before = get_vmswap();

	fd = open("/proc/self/swap_pages", O_WRONLY);
	if (fd < 0) {
		TEST_FAIL("Cannot open swap_pages: %s", strerror(errno));
		goto cleanup;
	}

	swap_triggered = 0;
	for (i = 0; i < NUM_HUGEPAGES; i++) {
		uintptr_t swap_addr = (uintptr_t)addr1 + (i * HPAGE_SIZE) + HPAGE_SIZE / 2;
		snprintf(addr_str, sizeof(addr_str), "0x%lx\n", swap_addr);
		if (write(fd, addr_str, strlen(addr_str)) >= 0)
			swap_triggered++;
	}
	close(fd);

	if (swap_triggered == 0) {
		TEST_FAIL("Failed to trigger swap for first 6MB");
		goto cleanup;
	}
	TEST_PASS("Triggered swapout for first 6MB (%d/%d pages)", swap_triggered, NUM_HUGEPAGES);

	/* Wait for swap to complete */
	usleep(1000000);

	/* Verify swap completed */
	free_hp_after = get_free_hugepages();
	swap_after = get_vmswap();
	TEST_INFO("Free hugepages: %d -> %d", free_hp_before, free_hp_after);
	TEST_INFO("VmSwap: %lu -> %lu kB", swap_before, swap_after);

	if (!(swap_after > swap_before && free_hp_after > free_hp_before)) {
		TEST_FAIL("Swapout verification failed for first 6MB");
		goto cleanup;
	}
	TEST_PASS("First 6MB swapped out successfully (pages now in swap, not in memory)");

	/*
	 * Step 4: Allocate second 6MB
	 *
	 * At this point:
	 * - addr1 is still mmap'd (6MB virtual) but swapped out (0MB physical)
	 * - We allocate addr2 (another 6MB virtual)
	 * - Total: 12MB mmap'd, but only 6MB in physical memory
	 *
	 * The hugetlb pool has 6MB free (from swapped out pages), so addr2
	 * should allocate successfully using those freed physical pages.
	 */
	addr2 = mmap(NULL, len, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

	if (addr2 == MAP_FAILED) {
		TEST_FAIL("Failed to mmap second 6MB: %s", strerror(errno));
		goto cleanup;
	}
	TEST_PASS("Allocated second 6MB at %p (reusing freed physical pages)", addr2);

	/* Step 5: Write pattern 0xBB to second 6MB */
	memset(addr2, PATTERN_SECOND, len);
	if (verify_pattern(addr2, len, PATTERN_SECOND) != 0) {
		goto cleanup;
	}
	TEST_PASS("Written pattern 0x%02X to second 6MB", PATTERN_SECOND);

	/*
	 * Step 6: Unmap second 6MB to free physical pages before swapin
	 * This ensures we don't exceed 6MB physical when swapping in addr1
	 */
	munmap(addr2, len);
	addr2 = NULL;
	TEST_PASS("Unmapped second 6MB to free physical pages");

	/* Step 7: Trigger swapin for first 6MB by accessing it */
	TEST_INFO("Triggering swapin for first 6MB...");
	trigger_swapin(addr1, len);

	/* Step 8: Verify first 6MB still contains 0xAA after swapin */
	if (verify_pattern(addr1, len, PATTERN_FIRST) != 0) {
		goto cleanup;
	}
	TEST_PASS("First 6MB data verified after swapin (pattern 0x%02X)", PATTERN_FIRST);

	TEST_PASS("Data integrity verified across swap operations!");

cleanup:
	if (addr1) munmap(addr1, len);
	if (addr2) munmap(addr2, len);
	/* Restore original nr_hugepages */
	if (orig_nr_hugepages > 0) {
		set_nr_hugepages(orig_nr_hugepages);
		TEST_INFO("Restored nr_hugepages to %d", orig_nr_hugepages);
	}
	printf("\n");
}

/* Test 6: Check reserved hugepages are available */
static void test_hugepage_reservation(void)
{
	FILE *fp;
	char line[256];
	int reserved = 0;

	printf("Test 5: HugePage Reservation Status\n");

	fp = fopen("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages", "r");
	if (fp) {
		if (fgets(line, sizeof(line), fp)) {
			reserved = atoi(line);
			if (reserved > 0) {
				TEST_PASS("Reserved %d x 2MB hugepages", reserved);
			} else {
				TEST_SKIP("No 2MB hugepages reserved");
			}
		}
		fclose(fp);
	}

	fp = fopen("/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages", "r");
	if (fp) {
		if (fgets(line, sizeof(line), fp)) {
			int free = atoi(line);
			TEST_INFO("Free hugepages: %d", free);
		}
		fclose(fp);
	}

	printf("\n");
}

static void print_summary(void)
{
	printf("=== Test Summary ===\n");
	printf("  Passed:  %d\n", tests_passed);
	printf("  Failed:  %d\n", tests_failed);
	printf("  Skipped: %d\n", tests_skipped);
	printf("\n");

	if (tests_failed == 0)
		printf("Overall: SUCCESS\n");
	else
		printf("Overall: FAILURE\n");
}

int main(int argc, char *argv[])
{
	if (getuid() != 0) {
		printf("WARNING: Not running as root.\n\n");
	}

	print_header();
	test_etmem_config();
	test_kernel_swap_enable();
	test_swap_pages_interface();
	test_hugepage_reservation();
	test_anonymous_hugetlb();
	test_hugetlb_swap_data_integrity();
	print_summary();

	return tests_failed > 0 ? 1 : 0;
}
