/*
 * HugeTLB OOM Direct Reclaim Test
 *
 * Tests that the kernel can direct-reclaim hugetlb pages when the free
 * pool is exhausted. With MAP_NORESERVE on hugetlbfs, accessing pages
 * beyond the reserved pool triggers direct reclaim which swaps out
 * existing hugetlb pages to make room for new allocations.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <setjmp.h>
#include <fcntl.h>
#include <errno.h>

#include "test_common.h"

#define HPAGE_SIZE (2 * 1024 * 1024)
#define NR_RESERVED 32
#define NR_MMAP 34

static sigjmp_buf env;
static volatile sig_atomic_t g_sigbus = 0;
static volatile sig_atomic_t g_sigbus_page = -1;

static void handler(int sig)
{
	g_sigbus = 1;
	siglongjmp(env, 1);
}

/* Test: direct reclaim allows allocation beyond reserved pool */
static void test(void)
{
	const char *path = "/mnt/huge/t";
	int fd;
	void *ptr;
	int i, touched = 0;

	fd = open(path, O_CREAT | O_RDWR | O_TRUNC | O_LARGEFILE, 0755);
	if (fd < 0) { SKIP("open failed"); return; }

	ftruncate(fd, NR_MMAP * HPAGE_SIZE);

	signal(SIGBUS, handler);
	ptr = mmap(NULL, NR_MMAP * HPAGE_SIZE, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_NORESERVE, fd, 0);
	if (ptr == MAP_FAILED) {
		close(fd);
		SKIP("mmap failed: %s", strerror(errno));
		return;
	}

	/*
	 * Touch all NR_MMAP pages. The first NR_RESERVED use the free pool.
	 * Pages beyond NR_RESERVED require direct reclaim to swap out
	 * earlier pages and free up hugepages for new allocations.
	 */
	for (i = 0; i < NR_MMAP; i++) {
		if (sigsetjmp(env, 1) != 0) {
			/* Returned from SIGBUS handler */
			g_sigbus_page = i;
			FAIL("SIGBUS at page %d", i);
			break;
		}

		char *p = (char *)ptr + i * HPAGE_SIZE;
		p[0] = 'A' + (i % 26);
		touched++;
	}

	int swpd = get_hugepages_swpd();
	if (touched < NR_MMAP) {
		FAIL("SIGBUS at page %d, direct reclaim did not work", g_sigbus_page);
	} else {
		PASS("Direct reclaim succeeded, all %d pages touched", touched);
	}
	if (swpd > 0) {
		PASS("All %d pages touched, direct reclaim swapped %d pages",
				touched, swpd);
	} else {
		FAIL("All %d pages touched, but no pages swapped (swap count: %d)", touched, swpd);
	}

	munmap(ptr, NR_MMAP * HPAGE_SIZE);
	close(fd);
	unlink(path);
	signal(SIGBUS, SIG_DFL);
}

int main(void)
{
	printf("=== HugeTLB OOM Direct Reclaim ===\n");
	printf("Kernel: "); fflush(stdout); system("uname -r");

	test();

	printf("\n=== Summary ===\n");
	printf("  Passed:  %d\n", passed);
	printf("  Failed:  %d\n", failed);
	printf("  Skipped: %d\n", skipped);

	fflush(stdout);
	fflush(stderr);

	return failed > 0 ? 1 : 0;
}
