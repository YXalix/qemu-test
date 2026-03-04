// SPDX-License-Identifier: GPL-2.0
/*
 * ETMEM (Enhanced Tiered Memory) Swap Test Program
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>

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
	print_summary();

	return tests_failed > 0 ? 1 : 0;
}
