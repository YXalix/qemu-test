#include "test_common.h"

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
