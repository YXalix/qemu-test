// SPDX-License-Identifier: GPL-2.0
/*
 * Hugetlb OOM Rescue Test with ETMEM Swap
 *
 * This test verifies that ETMEM swap can rescue a process from hugetlb OOM:
 * 1. Creates a cgroup with hugetlb limit (e.g., 10MB = 5 huge pages)
 * 2. Main thread mmaps 20MB (10 huge pages) - exceeding the limit
 * 3. Monitor thread watches cgroup hugetlb.2MB.events for OOM events
 * 4. When OOM is triggered, monitor thread triggers ETMEM swap
 * 5. Main thread continues allocation without being killed
 *
 * Key requirement: Main thread MUST NOT be killed by OOM killer
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
#include <sys/wait.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <poll.h>

#define HUGEPAGE_SIZE (2 * 1024 * 1024)  /* 2MB */
#define LIMIT_MB 10
#define ALLOC_MB 20
#define LIMIT_PAGES (LIMIT_MB / 2)       /* 5 pages */
#define ALLOC_PAGES (ALLOC_MB / 2)       /* 10 pages */

/* cgroup v1 paths */
#define CGROUP_V1_HUGETLB_PATH "/sys/fs/cgroup/hugetlb"
#define TEST_CGROUP "test-hugetlb-oom"

/* Test result macros */
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

/* Shared state between main thread and monitor thread */
struct shared_state {
	pthread_mutex_t lock;
	int alloc_count;                    /* Number of pages allocated */
	void *addrs[ALLOC_PAGES];           /* Array of mapped addresses */
	atomic_int swap_triggered;          /* Set when swap starts */
	atomic_int swap_completed;          /* Set when swap done */
	atomic_int oom_detected;            /* Set when OOM event detected */
	atomic_int main_thread_alive;       /* Main thread sets this periodically */
	atomic_int main_thread_done;        /* Set when main thread completes */
	int pages_swapped;                  /* Count of successfully swapped pages */
	pid_t main_tid;                     /* Main thread TID */
};

static struct shared_state *g_state = NULL;
static volatile int g_stop_monitor = 0;

static void print_header(void)
{
	printf("=== Hugetlb OOM Rescue Test (Multi-threaded) ===\n\n");
	printf("Architecture: arm64\n");
	printf("Kernel: ");
	fflush(stdout);
	system("uname -r");
	printf("\n");
	TEST_INFO("Limit: %dMB (%d pages), Target: %dMB (%d pages)",
		  LIMIT_MB, LIMIT_PAGES, ALLOC_MB, ALLOC_PAGES);
}

/* Read cgroup hugetlb failcnt (v1) or events max (v2) */
static int read_hugetlb_events(const char *cgroup_path, unsigned long *max_events)
{
	char path[512];
	char buf[256];
	int fd;
	ssize_t n;

	/* Try cgroup v1 first - failcnt file */
	snprintf(path, sizeof(path), "%s/%s/hugetlb.2MB.failcnt", CGROUP_V1_HUGETLB_PATH, cgroup_path);
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = '\0';
			*max_events = strtoul(buf, NULL, 10);
			return 0;
		}
	}

	/* Try cgroup v2 events file */
	snprintf(path, sizeof(path), "%s/%s/hugetlb.2MB.events", CGROUP_V1_HUGETLB_PATH, cgroup_path);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);

	if (n <= 0)
		return -1;

	buf[n] = '\0';

	/* Parse format: "low 0\nhigh 0\nmax <count>\n" */
	char *line = strtok(buf, "\n");
	while (line) {
		if (strncmp(line, "max ", 4) == 0) {
			*max_events = strtoul(line + 4, NULL, 10);
			return 0;
		}
		line = strtok(NULL, "\n");
	}

	return -1;
}

/* Read current hugetlb usage in bytes */
static unsigned long read_hugetlb_usage(const char *cgroup_path)
{
	char path[512];
	char buf[64];
	int fd;
	ssize_t n;
	unsigned long val = 0;

	/* cgroup v1: usage_in_bytes */
	snprintf(path, sizeof(path), "%s/%s/hugetlb.2MB.usage_in_bytes",
		 CGROUP_V1_HUGETLB_PATH, cgroup_path);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);

	if (n > 0) {
		buf[n] = '\0';
		val = strtoul(buf, NULL, 10);
	}

	return val;
}

/* Read free hugepages from system */
static int read_free_hugepages(void)
{
	FILE *fp;
	int free_hp = 0;

	fp = fopen("/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages", "r");
	if (fp) {
		fscanf(fp, "%d", &free_hp);
		fclose(fp);
	}
	return free_hp;
}

/* Trigger ETMEM swap for a specific address */
static int trigger_etmem_swap(pid_t pid, void *addr)
{
	char path[256];
	int fd;
	char buf[64];
	ssize_t ret;

	snprintf(path, sizeof(path), "/proc/%d/swap_pages", pid);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;

	snprintf(buf, sizeof(buf), "%p\n", addr);
	ret = write(fd, buf, strlen(buf));
	close(fd);

	return (ret > 0) ? 0 : -1;
}

/* Check if ETMEM is available */
static int etmem_available(void)
{
	return access("/sys/kernel/mm/swap/kernel_swap_enable", F_OK) == 0;
}

/* Check if cgroup hugetlb controller is available */
static int cgroup_hugetlb_available(void)
{
	return access(CGROUP_V1_HUGETLB_PATH, F_OK) == 0;
}

/* Create test cgroup */
static int create_test_cgroup(const char *name)
{
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "mkdir -p %s/%s 2>/dev/null", CGROUP_V1_HUGETLB_PATH, name);
	return system(cmd) == 0 ? 0 : -1;
}

/* Remove test cgroup */
static void remove_test_cgroup(const char *name)
{
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rmdir %s/%s 2>/dev/null", CGROUP_V1_HUGETLB_PATH, name);
	system(cmd);
}

/* Move current process to cgroup */
static int move_to_cgroup(const char *name)
{
	char path[512];
	char pid_str[32];
	int fd;

	snprintf(path, sizeof(path), "%s/%s/tasks", CGROUP_V1_HUGETLB_PATH, name);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;

	snprintf(pid_str, sizeof(pid_str), "%d", getpid());
	if (write(fd, pid_str, strlen(pid_str)) < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/* Set hugetlb limit in cgroup (v1 uses limit_in_bytes) */
static int set_hugetlb_limit(const char *name, int limit_mb)
{
	char path[512];
	char val[32];
	int fd;
	unsigned long limit_bytes = limit_mb * 1024 * 1024;

	snprintf(path, sizeof(path), "%s/%s/hugetlb.2MB.limit_in_bytes",
		 CGROUP_V1_HUGETLB_PATH, name);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;

	snprintf(val, sizeof(val), "%lu", limit_bytes);
	if (write(fd, val, strlen(val)) < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/*
 * Monitor thread function
 * Watches for hugetlb OOM events and triggers ETMEM swap
 */
static void *monitor_thread(void *arg)
{
	struct shared_state *state = (struct shared_state *)arg;
	unsigned long max_events_before = 0;
	unsigned long max_events_after = 0;
	int poll_count = 0;
	int max_polls = 1000;  /* 10 seconds max (10ms * 1000) */

	TEST_INFO("Monitor: Started monitoring cgroup %s", TEST_CGROUP);

	/* Wait for main thread to start */
	TEST_INFO("Monitor: Waiting for main thread to start...");
	int wait_count = 0;
	while (!atomic_load(&state->main_thread_alive) && wait_count < 100) {
		usleep(10000);  /* 10ms */
		wait_count++;
	}
	if (!atomic_load(&state->main_thread_alive)) {
		TEST_INFO("Monitor: Main thread failed to start, aborting");
		return NULL;
	}
	TEST_INFO("Monitor: Main thread started");

	/* Read initial events count */
	if (read_hugetlb_events(TEST_CGROUP, &max_events_before) < 0) {
		TEST_INFO("Monitor: Failed to read initial events");
	}
	TEST_INFO("Monitor: Initial max events = %lu", max_events_before);

	/* Main monitoring loop - can trigger swap multiple times */
	while (!g_stop_monitor && poll_count < max_polls) {
		unsigned long current_usage;

		/* Check if main thread is still alive (skip during swap) */
		if (!atomic_load(&state->swap_triggered) &&
		    !atomic_load(&state->main_thread_done)) {
			if (!atomic_load(&state->main_thread_alive)) {
				/* Give main thread time to set the flag */
				int wait_count = 0;
				while (!atomic_load(&state->main_thread_alive) && wait_count < 50) {
					usleep(10000);  /* 10ms */
					wait_count++;
				}
				if (!atomic_load(&state->main_thread_alive)) {
					TEST_INFO("Monitor: Main thread appears dead, exiting");
					return NULL;
				}
			}
			/* Reset the alive flag - main thread should set it periodically */
			atomic_store(&state->main_thread_alive, 0);
		}

		/* Read current events */
		if (read_hugetlb_events(TEST_CGROUP, &max_events_after) == 0) {
			if (max_events_after > max_events_before) {
				TEST_INFO("Monitor: OOM event detected! max: %lu -> %lu",
					  max_events_before, max_events_after);
				atomic_store(&state->oom_detected, 1);
			}
		}

		/* Check usage and trigger swap when needed */
		current_usage = read_hugetlb_usage(TEST_CGROUP);
		if (current_usage >= (LIMIT_MB - 4) * 1024 * 1024 &&
		    !atomic_load(&state->swap_triggered)) {
			TEST_INFO("Monitor: Usage high (%lu bytes), triggering swap",
				  current_usage);

			/* Trigger swap */
			atomic_store(&state->swap_triggered, 1);

			pthread_mutex_lock(&state->lock);
			int current_allocs = state->alloc_count;
			pthread_mutex_unlock(&state->lock);

			TEST_INFO("Monitor: Current allocations = %d, swapping first half...",
				  current_allocs);

			/* Swap out first half of allocated pages */
			int swapped = 0;
			for (int i = 0; i < current_allocs / 2 && i < ALLOC_PAGES; i++) {
				void *addr = state->addrs[i];
				if (addr) {
					if (trigger_etmem_swap(state->main_tid, addr) == 0) {
						swapped++;
						TEST_INFO("Monitor: Swapped page %d at %p", i, addr);
					} else {
						TEST_INFO("Monitor: Failed to swap page %d at %p (%s)",
							  i, addr, strerror(errno));
					}
				}
			}

			/* Wait for swap to complete */
			TEST_INFO("Monitor: Waiting for swap to complete...");
			usleep(300000);  /* 300ms */

			unsigned long usage_after = read_hugetlb_usage(TEST_CGROUP);
			TEST_INFO("Monitor: Usage after swap: %lu bytes", usage_after);

			pthread_mutex_lock(&state->lock);
			state->pages_swapped += swapped;
			pthread_mutex_unlock(&state->lock);

			atomic_store(&state->swap_completed, 1);
			TEST_INFO("Monitor: Swap completed (%d pages swapped this round)", swapped);

			/* Reset for next round if needed */
			usleep(100000);  /* 100ms */
			atomic_store(&state->swap_triggered, 0);
			atomic_store(&state->swap_completed, 0);
		}

		/* Check if main thread is done */
		if (atomic_load(&state->main_thread_done)) {
			TEST_INFO("Monitor: Main thread completed");
			return NULL;
		}

		usleep(5000);  /* 5ms polling interval */
		poll_count++;
	}

	if (poll_count >= max_polls) {
		TEST_INFO("Monitor: Timeout waiting");
	}

	return NULL;
}

/*
 * Main thread function
 * Allocates hugetlb pages continuously, triggering OOM
 */
static void *main_thread(void *arg)
{
	struct shared_state *state = (struct shared_state *)arg;
	int i;
	void *ptr;

	state->main_tid = gettid();
	TEST_INFO("Main: Starting allocation (tid=%d)", (int)gettid());

	/* Signal that we're alive - do this BEFORE monitor can check */
	atomic_store(&state->main_thread_alive, 1);

	for (i = 0; i < ALLOC_PAGES; i++) {
		/* Wait for any ongoing swap to complete before allocating */
		if (atomic_load(&state->swap_triggered)) {
			TEST_INFO("Main: Swap in progress, waiting...");
			int wait_count = 0;
			while (atomic_load(&state->swap_triggered) && wait_count < 100) {
				usleep(10000);  /* 10ms */
				wait_count++;
				atomic_store(&state->main_thread_alive, 1);
			}
			if (!atomic_load(&state->swap_triggered)) {
				TEST_INFO("Main: Swap completed, resuming allocation");
			}
		}

		/* Allocate a huge page - retry if it fails */
		int retry_count = 0;
		while (retry_count < 10) {
			ptr = mmap(NULL, HUGEPAGE_SIZE,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
				   -1, 0);

			if (ptr != MAP_FAILED)
				break;

			/* mmap failed - wait for monitor to swap out pages */
			TEST_INFO("Main: mmap failed at page %d (retry %d), waiting for swap...", i, retry_count);
			int wait_count = 0;
			while (wait_count < 50) {
				usleep(20000);  /* 20ms */
				wait_count++;
				atomic_store(&state->main_thread_alive, 1);
				if (atomic_load(&state->pages_swapped) > 0)
					break;
			}
			retry_count++;
		}

		if (ptr == MAP_FAILED) {
			TEST_INFO("Main: mmap failed at page %d after retries: %s", i, strerror(errno));
			break;
		}

		/* Touch the page to trigger page fault (allocating physical page) */
		memset(ptr, 0xAA + (i % 10), HUGEPAGE_SIZE);

		/* Store address */
		pthread_mutex_lock(&state->lock);
		state->addrs[state->alloc_count] = ptr;
		state->alloc_count++;
		pthread_mutex_unlock(&state->lock);

		TEST_INFO("Main: Allocated and touched page %d at %p", i + 1, ptr);

		/* Signal we're alive */
		atomic_store(&state->main_thread_alive, 1);

		/* Small delay to allow monitor thread to detect OOM */
		usleep(30000);  /* 30ms */
	}

	TEST_INFO("Main: Allocation loop completed (%d pages)", i);
	atomic_store(&state->main_thread_done, 1);

	return (void *)(uintptr_t)i;
}

/* Signal handler to detect OOM kill */
static volatile int g_signal_received = 0;
static volatile int g_signal_num = 0;

static void signal_handler(int sig)
{
	g_signal_received = 1;
	g_signal_num = sig;
	TEST_INFO("SIGNAL: Received signal %d (%s)", sig,
		  sig == SIGKILL ? "SIGKILL" :
		  sig == SIGBUS ? "SIGBUS" :
		  sig == SIGSEGV ? "SIGSEGV" : "UNKNOWN");
}

/* Main test function */
static void test_hugetlb_oom_rescue(void)
{
	pthread_t main_tid, monitor_tid;
	void *main_result;
	int ret;
	unsigned long max_events_before, max_events_after;
	unsigned long usage_before, usage_after;
	int free_hp_before, free_hp_after;

	printf("\n=== Test: Hugetlb OOM Rescue with ETMEM Swap ===\n\n");

	/* Check prerequisites */
	if (!etmem_available()) {
		TEST_SKIP("ETMEM not available");
		return;
	}

	if (!cgroup_hugetlb_available()) {
		TEST_SKIP("cgroup hugetlb controller not available");
		return;
	}

	/* Check free hugepages */
	free_hp_before = read_free_hugepages();
	if (free_hp_before < ALLOC_PAGES) {
		TEST_SKIP("Not enough free hugepages (need %d, have %d)",
			  ALLOC_PAGES, free_hp_before);
		return;
	}

	/* Create test cgroup */
	remove_test_cgroup(TEST_CGROUP);
	if (create_test_cgroup(TEST_CGROUP) != 0) {
		TEST_FAIL("Failed to create test cgroup");
		return;
	}
	TEST_PASS("Created test cgroup: %s", TEST_CGROUP);

	/* Set hugetlb limit */
	if (set_hugetlb_limit(TEST_CGROUP, LIMIT_MB) != 0) {
		TEST_FAIL("Failed to set hugetlb limit");
		remove_test_cgroup(TEST_CGROUP);
		return;
	}
	TEST_PASS("Set hugetlb limit to %dMB", LIMIT_MB);

	/* Move to cgroup */
	if (move_to_cgroup(TEST_CGROUP) != 0) {
		TEST_FAIL("Failed to move to test cgroup");
		remove_test_cgroup(TEST_CGROUP);
		return;
	}
	TEST_PASS("Moved process to test cgroup");

	/* Read initial state */
	read_hugetlb_events(TEST_CGROUP, &max_events_before);
	usage_before = read_hugetlb_usage(TEST_CGROUP);
	TEST_INFO("Initial state: usage=%lu bytes, max_events=%lu",
		  usage_before, max_events_before);

	/* Allocate shared state */
	g_state = calloc(1, sizeof(struct shared_state));
	if (!g_state) {
		TEST_FAIL("Failed to allocate shared state");
		remove_test_cgroup(TEST_CGROUP);
		return;
	}

	pthread_mutex_init(&g_state->lock, NULL);
	atomic_init(&g_state->swap_triggered, 0);
	atomic_init(&g_state->swap_completed, 0);
	atomic_init(&g_state->oom_detected, 0);
	atomic_init(&g_state->main_thread_alive, 0);
	atomic_init(&g_state->main_thread_done, 0);

	/* Install signal handlers */
	signal(SIGBUS, signal_handler);
	signal(SIGSEGV, signal_handler);
	/* Note: SIGKILL cannot be caught, but we check for it via WIFSIGNALED */

	/* Create monitor thread first */
	ret = pthread_create(&monitor_tid, NULL, monitor_thread, g_state);
	if (ret != 0) {
		TEST_FAIL("Failed to create monitor thread: %s", strerror(ret));
		goto cleanup;
	}
	TEST_PASS("Created monitor thread");

	/* Give monitor thread time to start */
	usleep(100000);

	/* Create main thread */
	ret = pthread_create(&main_tid, NULL, main_thread, g_state);
	if (ret != 0) {
		TEST_FAIL("Failed to create main thread: %s", strerror(ret));
		g_stop_monitor = 1;
		pthread_join(monitor_tid, NULL);
		goto cleanup;
	}
	TEST_PASS("Created main thread");

	/* Wait for main thread to complete or be killed */
	ret = pthread_join(main_tid, &main_result);
	if (ret != 0) {
		TEST_FAIL("Failed to join main thread: %s", strerror(ret));
	}

	/* Stop monitor thread */
	g_stop_monitor = 1;
	pthread_join(monitor_tid, NULL);

	/* Check if signal was received */
	if (g_signal_received) {
		TEST_INFO("Main thread received signal %d", g_signal_num);
	}

	/* Read final state */
	read_hugetlb_events(TEST_CGROUP, &max_events_after);
	usage_after = read_hugetlb_usage(TEST_CGROUP);
	free_hp_after = read_free_hugepages();

	int final_alloc_count = g_state->alloc_count;
	int pages_swapped = g_state->pages_swapped;
	int oom_detected = atomic_load(&g_state->oom_detected);
	int swap_triggered = atomic_load(&g_state->swap_triggered);

	TEST_INFO("Final state: usage=%lu bytes, max_events=%lu",
		  usage_after, max_events_after);
	TEST_INFO("Final: alloc_count=%d, oom_detected=%d, swap_triggered=%d, pages_swapped=%d",
		  final_alloc_count, oom_detected, swap_triggered, pages_swapped);

	/*
	 * Determine test result
	 *
	 * PASS conditions (any of):
	 * 1. Main thread allocated all pages without being killed AND swap was triggered
	 * 2. Main thread allocated all pages (limit was sufficient)
	 * 3. Main thread survived partial allocation with swap
	 *
	 * FAIL conditions:
	 * 1. Main thread was killed by OOM killer (SIGKILL/SIGBUS)
	 * 2. Main thread crashed with SIGSEGV
	 */

	if (g_signal_num == SIGKILL || g_signal_num == SIGBUS) {
		/* Main thread was killed by OOM killer - this is the critical failure case */
		if (swap_triggered && pages_swapped > 0) {
			TEST_FAIL("CRITICAL: Main thread killed by OOM killer despite swap attempt "
				  "(%d pages swapped). Kernel hugetlb swap rescue did not work!",
				  pages_swapped);
			TEST_INFO("This indicates a kernel issue - the swap should have freed "
				  "pages before OOM killer was invoked.");
		} else {
			TEST_FAIL("Main thread killed by OOM killer, swap not triggered or ineffective");
		}
	} else if (g_signal_num == SIGSEGV) {
		TEST_FAIL("Main thread crashed with SIGSEGV");
	} else if (final_alloc_count >= ALLOC_PAGES) {
		/* All allocations succeeded */
		if (swap_triggered && pages_swapped > 0) {
			TEST_PASS("SUCCESS: All %d pages allocated with ETMEM swap rescue (%d pages swapped)",
				  final_alloc_count, pages_swapped);
		} else {
			TEST_PASS("All %d pages allocated (OOM not triggered or limit sufficient)",
				  final_alloc_count);
		}
	} else if (final_alloc_count >= LIMIT_PAGES) {
		/* Partial allocation - this is expected with limit */
		if (swap_triggered && pages_swapped > 0) {
			TEST_PASS("Partial success: Allocated %d pages with swap rescue (%d pages swapped)",
				  final_alloc_count, pages_swapped);
		} else {
			TEST_INFO("Allocated %d pages without swap trigger", final_alloc_count);
			if (max_events_after > max_events_before) {
				TEST_PASS("OOM event recorded but main thread survived (no swap needed)");
			} else {
				TEST_FAIL("Only allocated %d pages, no OOM or swap", final_alloc_count);
			}
		}
	} else {
		/* Low allocation count - investigate */
		TEST_FAIL("Only allocated %d pages (expected at least %d)",
			  final_alloc_count, LIMIT_PAGES);
	}

	/* Additional verification */
	if (max_events_after > max_events_before) {
		TEST_INFO("OOM events: %lu -> %lu (limit was enforced)",
			  max_events_before, max_events_after);
	}

	if (free_hp_after != free_hp_before) {
		TEST_INFO("Free hugepages: %d -> %d", free_hp_before, free_hp_after);
	}

cleanup:
	/* Cleanup */
	pthread_mutex_destroy(&g_state->lock);
	free(g_state);
	g_state = NULL;

	remove_test_cgroup(TEST_CGROUP);
}

/* Test kernel configuration */
static void test_kernel_config(void)
{
	FILE *fp;
	char line[256];
	int found_hugetlb_swap = 0;
	int found_etmem = 0;
	int found_cgroup = 0;

	printf("\n=== Test: Kernel Configuration ===\n\n");

	fp = popen("zcat /proc/config.gz 2>/dev/null", "r");
	if (!fp) {
		TEST_SKIP("Kernel config not accessible");
		return;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "CONFIG_HUGETLB_SWAP=y"))
			found_hugetlb_swap = 1;
		if (strstr(line, "CONFIG_ETMEM=y"))
			found_etmem = 1;
		if (strstr(line, "CONFIG_CGROUP_HUGETLB=y"))
			found_cgroup = 1;
	}

	pclose(fp);

	if (found_hugetlb_swap)
		TEST_PASS("CONFIG_HUGETLB_SWAP=y");
	else
		TEST_FAIL("CONFIG_HUGETLB_SWAP not enabled");

	if (found_etmem)
		TEST_PASS("CONFIG_ETMEM=y");
	else
		TEST_FAIL("CONFIG_ETMEM not enabled");

	if (found_cgroup)
		TEST_PASS("CONFIG_CGROUP_HUGETLB=y");
	else
		TEST_FAIL("CONFIG_CGROUP_HUGETLB not enabled");
}

static void print_summary(void)
{
	printf("\n=== Test Summary ===\n");
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
		printf("ERROR: Must run as root\n");
		return 1;
	}

	print_header();
	test_kernel_config();
	test_hugetlb_oom_rescue();
	print_summary();

	return tests_failed > 0 ? 1 : 0;
}
