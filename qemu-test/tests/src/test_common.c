#include "test_common.h"
#include <string.h>
#include <stdint.h>
#include <errno.h>

int passed = 0;
int failed = 0;
int skipped = 0;

int get_nr_hugepages(void)
{
    FILE *fp = fopen("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages", "r");
    int nr = 0;
    if (fp) {
        fscanf(fp, "%d", &nr);
        fclose(fp);
    }
    return nr;
}

int get_free_hugepages(void)
{
    FILE *fp = fopen("/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages", "r");
    int free = -1;
    if (fp) {
        fscanf(fp, "%d", &free);
        fclose(fp);
    }
    return free;
}

int set_nr_hugepages(int count)
{
    FILE *fp = fopen("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages", "w");
    if (!fp) return -1;
    fprintf(fp, "%d\n", count);
    fclose(fp);
    return 0;
}

unsigned long get_vmswap(void)
{
    FILE *fp = fopen("/proc/self/status", "r");
    char line[256];
    unsigned long vmswap = 0;

    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmSwap:", 7) == 0) {
            sscanf(line, "VmSwap: %lu", &vmswap);
            break;
        }
    }
    fclose(fp);
    return vmswap;
}

/*
 * get_hugepages_swpd - Get number of swapped hugepages from /proc/meminfo
 *
 * The kernel now counts all hugepage swap in HugePages_Swap field.
 * This is more accurate than VmSwap for hugepage-specific swap tracking.
 */
int get_hugepages_swpd(void)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    char line[256];
    int swpd = 0;

    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "HugePages_Swap:", 15) == 0) {
            sscanf(line, "HugePages_Swap: %d", &swpd);
            break;
        }
    }
    fclose(fp);
    return swpd;
}

int open_swap_pages(void)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/swap_pages", getpid());
    return open(path, O_WRONLY | O_CLOEXEC);
}

void *alloc_hugetlb(size_t size)
{
    void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    return (addr == MAP_FAILED) ? NULL : addr;
}

void free_hugetlb(void *addr, size_t size)
{
    if (addr) munmap(addr, size);
}

int verify_pattern(void *addr, size_t len, unsigned char pattern)
{
    unsigned char *p = addr;
    for (size_t i = 0; i < len; i++) {
        if (p[i] != pattern) {
            FAIL("Data corruption at offset %zu: expected 0x%02X, got 0x%02X", i, pattern, p[i]);
            return -1;
        }
    }
    return 0;
}

int trigger_swap(void *addr, size_t offset)
{
    int fd = open_swap_pages();
    if (fd < 0) {
        INFO("Cannot open swap_pages: %s", strerror(errno));
        return -1;
    }

    char addr_str[32];
    snprintf(addr_str, sizeof(addr_str), "0x%lx\n", (uintptr_t)addr + offset);

    ssize_t ret = write(fd, addr_str, strlen(addr_str));
    close(fd);

    if (ret < 0) {
        INFO("Swap trigger write failed: %s", strerror(errno));
        return -1;
    }

    /*
     * Note: swap_pages_write returns count even if address is invalid.
     * We can only verify swap success by checking HugePages_Swpd or free
     * hugepages before/after the call. Return 0 means request was sent.
     */
    PASS("Swap trigger sent for %d pages", 1);
    return 0;
}

/*
 * trigger_swap_multi - Swap multiple pages at once (with batching)
 * @addrs: Array of virtual addresses
 * @count: Number of addresses
 *
 * Returns: 0 on success (request sent), -1 on failure
 *
 * Note: Like trigger_swap(), this only sends the request. Actual swap
 * success must be verified by checking HugePages_Swpd or free hugepages.
 *
 * Batching: Kernel has ~8MB kmalloc limit. We batch to stay well under it.
 * Each address: "0x%lx\n" ~ 20 bytes, so 1000 addresses ~ 20KB.
 */
int trigger_swap_multi(void **addrs, int count)
{
    /* Process in batches of 1000 to stay well under 8MB kernel limit */
    const int BATCH_SIZE = 1000;
    int processed = 0;
    int fd;

    fd = open_swap_pages();
    if (fd < 0) {
        INFO("Cannot open swap_pages: %s", strerror(errno));
        return -1;
    }

    while (processed < count) {
        char buf[32 * BATCH_SIZE];
        size_t len = 0;
        int batch_count = 0;

        /* Build batch */
        for (int i = processed; i < count && batch_count < BATCH_SIZE; i++, batch_count++) {
            len += snprintf(buf + len, sizeof(buf) - len, "0x%lx\n", (uintptr_t)addrs[i]);
        }

        /* Send batch */
        ssize_t ret = write(fd, buf, len);
        if (ret < 0) {
            INFO("Swap trigger write failed at batch %d: %s",
                 processed / BATCH_SIZE, strerror(errno));
            close(fd);
            return -1;
        }

        processed += batch_count;
    }

    close(fd);
    PASS("Swap trigger sent for %d pages", count);
    return 0;
}

int get_hugepages_resv(void)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    char line[256];
    int rsvd = 0;

    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "HugePages_Rsvd:", 15) == 0) {
            sscanf(line, "HugePages_Rsvd: %d", &rsvd);
            break;
        }
    }
    fclose(fp);
    return rsvd;
}
