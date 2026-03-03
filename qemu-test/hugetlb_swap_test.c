/*
 * Hugetlb Swap Test Program
 * Tests allocation and swapping of huge pages via ETMEM interface
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/wait.h>

#define HUGEPAGE_SIZE_2M (2 * 1024 * 1024)
#define HUGEPAGE_SIZE_1G (1024 * 1024 * 1024)
#define NUM_PAGES 10

static void print_meminfo(void)
{
	FILE *fp = fopen("/proc/meminfo", "r");
	char line[256];

	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "Huge") || strstr(line, "Swap") ||
		    strstr(line, "MemFree") || strstr(line, "MemTotal"))
			printf("  %s", line);
	}
	fclose(fp);
}

static int reserve_hugepages(int num_pages)
{
	FILE *fp = fopen("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages", "w");
	if (!fp) {
		perror("Failed to open nr_hugepages");
		return -1;
	}
	fprintf(fp, "%d", num_pages);
	fclose(fp);
	return 0;
}

static int get_free_hugepages(void)
{
	int free_pages = 0;
	FILE *fp = fopen("/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages", "r");
	if (fp) {
		fscanf(fp, "%d", &free_pages);
		fclose(fp);
	}
	return free_pages;
}

static void test_basic_allocation(void)
{
	int fd;
	void *addr;
	int i;

	printf("\n--- Test 1: Basic Hugetlb Allocation ---\n");

	// Open hugetlbfs
	fd = open("/mnt/huge", O_RDWR);
	if (fd < 0) {
		perror("Failed to open /mnt/huge");
		return;
	}

	// Reserve huge pages
	printf("Reserving %d huge pages...\n", NUM_PAGES);
	if (reserve_hugepages(NUM_PAGES) < 0) {
		close(fd);
		return;
	}

	printf("Free huge pages before allocation: %d\n", get_free_hugepages());

	// Allocate huge pages via mmap
	for (i = 0; i < NUM_PAGES; i++) {
		char fname[64];
		snprintf(fname, sizeof(fname), "/mnt/huge/testfile_%d", i);

		int hfd = open(fname, O_CREAT | O_RDWR, 0644);
		if (hfd < 0) {
			perror("open hugetlb file failed");
			continue;
		}

		addr = mmap(NULL, HUGEPAGE_SIZE_2M, PROT_READ | PROT_WRITE,
			    MAP_SHARED, hfd, 0);
		close(hfd);

		if (addr == MAP_FAILED) {
			perror("mmap failed");
			continue;
		}

		// Touch the memory to fault it in
		memset(addr, (char)(i & 0xFF), HUGEPAGE_SIZE_2M);
		printf("  Allocated page %d at %p\n", i, addr);

		munmap(addr, HUGEPAGE_SIZE_2M);
		unlink(fname);
	}

	printf("Free huge pages after allocation: %d\n", get_free_hugepages());

	close(fd);
	reserve_hugepages(0);  // Release reservation
	printf("Test 1 complete\n");
}

static void test_mmap_hugetlb(void)
{
	void *addr;
	int i;

	printf("\n--- Test 2: MAP_HUGETLB mmap ---\n");

	printf("Reserving %d huge pages...\n", NUM_PAGES);
	if (reserve_hugepages(NUM_PAGES) < 0)
		return;

	printf("Free huge pages before mmap: %d\n", get_free_hugepages());

	// Use MAP_HUGETLB flag
	for (i = 0; i < NUM_PAGES; i++) {
		addr = mmap(NULL, HUGEPAGE_SIZE_2M, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (21 << MAP_HUGE_SHIFT),
			    -1, 0);

		if (addr == MAP_FAILED) {
			if (errno == ENOMEM)
				printf("  Page %d: mmap failed (ENOMEM - no huge pages available)\n", i);
			else
				perror("mmap");
			continue;
		}

		// Touch the memory
		memset(addr, (char)(i & 0xFF), 4096);  // Just touch first page
		printf("  Mapped page %d at %p\n", i, addr);

		munmap(addr, HUGEPAGE_SIZE_2M);
	}

	printf("Free huge pages after mmap: %d\n", get_free_hugepages());

	reserve_hugepages(0);  // Release reservation
	printf("Test 2 complete\n");
}

static void test_etmem_interface(void)
{
	FILE *fp;

	printf("\n--- Test 3: ETMEM Interface Check ---\n");

	// Check if etmem reclaim is available
	fp = fopen("/proc/sys/vm/etmem_reclaim", "r");
	if (fp) {
		int enabled;
		fscanf(fp, "%d", &enabled);
		fclose(fp);
		printf("  /proc/sys/vm/etmem_reclaim = %d\n", enabled);

		// Try to enable it
		fp = fopen("/proc/sys/vm/etmem_reclaim", "w");
		if (fp) {
			fprintf(fp, "1");
			fclose(fp);
			printf("  Enabled etmem_reclaim\n");
		}
	} else {
		printf("  /proc/sys/vm/etmem_reclaim not available\n");
	}

	// Check kernel swap enable
	fp = fopen("/sys/kernel/mm/etmem/kernel_swap_enable", "r");
	if (fp) {
		int enabled;
		fscanf(fp, "%d", &enabled);
		fclose(fp);
		printf("  /sys/kernel/mm/etmem/kernel_swap_enable = %d\n", enabled);
	} else {
		printf("  /sys/kernel/mm/etmem/kernel_swap_enable not available\n");
	}

	// Check /proc/etmem
	if (access("/proc/etmem", X_OK) == 0) {
		printf("  /proc/etmem directory exists\n");
		// Try to list contents
		printf("  Contents:\n");
		system("ls -la /proc/etmem/ 2>/dev/null | sed 's/^/    /'");
	} else {
		printf("  /proc/etmem not available (may need kernel modules)\n");
	}

	printf("Test 3 complete\n");
}

static void test_swap_usage(void)
{
	FILE *fp;

	printf("\n--- Test 4: Swap Status ---\n");

	printf("Swap information:\n");
	fp = fopen("/proc/swaps", "r");
	if (fp) {
		char line[256];
		while (fgets(line, sizeof(line), fp))
			printf("  %s", line);
		fclose(fp);
	}

	printf("\nSwap statistics:\n");
	fp = fopen("/proc/vmstat", "r");
	if (fp) {
		char line[256];
		while (fgets(line, sizeof(line), fp)) {
			if (strstr(line, "swap") || strstr(line, "pswp"))
				printf("  %s", line);
		}
		fclose(fp);
	}

	printf("Test 4 complete\n");
}

static void test_memory_pressure(void)
{
	int i;
	void **buffers;
	int num_buffers = 100;
	int alloc_size = 10 * 1024 * 1024;  // 10MB each

	printf("\n--- Test 5: Memory Pressure Test ---\n");

	printf("Initial memory state:\n");
	print_meminfo();

	// Reserve some huge pages
	reserve_hugepages(20);

	printf("\nAllocating %d x %dMB buffers...\n", num_buffers, alloc_size / (1024*1024));

	buffers = calloc(num_buffers, sizeof(void *));
	if (!buffers) {
		perror("calloc");
		return;
	}

	for (i = 0; i < num_buffers; i++) {
		buffers[i] = malloc(alloc_size);
		if (!buffers[i]) {
			printf("  Failed to allocate buffer %d (OOM?)\n", i);
			break;
		}
		// Touch the memory
		memset(buffers[i], i & 0xFF, alloc_size);

		if ((i + 1) % 10 == 0) {
			printf("  Allocated %dMB...\n", (i + 1) * alloc_size / (1024*1024));
		}
	}

	printf("\nMemory after allocation:\n");
	print_meminfo();

	printf("\nSwap statistics:\n");
	system("cat /proc/vmstat | grep -E '(swap|pswp)' | sed 's/^/  /'");

	// Cleanup
	for (i = 0; i < num_buffers; i++) {
		if (buffers[i])
			free(buffers[i]);
	}
	free(buffers);
	reserve_hugepages(0);

	printf("\nTest 5 complete\n");
}

int main(int argc, char *argv[])
{
	printf("=== Hugetlb Swap Test Suite ===\n");
	printf("Kernel: ");
	fflush(stdout);
	system("uname -r");

	// Check if running as root (needed for some operations)
	if (getuid() != 0) {
		printf("\nWARNING: Not running as root. Some tests may fail.\n");
	}

	// Run all tests
	test_basic_allocation();
	test_mmap_hugetlb();
	test_etmem_interface();
	test_swap_usage();
	test_memory_pressure();

	printf("\n=== All Tests Complete ===\n");
	return 0;
}
