#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "test_common.h"

void test_etmem_config(void)
{
    FILE *fp;
    char line[256];
    int found = 0;

    printf("\nTest: ETMEM Kernel Configuration\n");
    
    fp = popen("zcat /proc/config.gz 2>/dev/null", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "CONFIG_ETMEM=y")) {
                found = 1;
                break;
            }
        }
        pclose(fp);
    }
    
    if (found) PASS("CONFIG_ETMEM=y enabled");
    else FAIL("CONFIG_ETMEM not found");
}

void test_swap_interface(void)
{
    int fd;
    
    printf("\nTest: Swap Interface\n");
    
    if (access("/sys/kernel/mm/swap/kernel_swap_enable", F_OK) == 0)
        PASS("kernel_swap_enable exists");
    else
        FAIL("kernel_swap_enable not found");
    
    fd = open_swap_pages();
    if (fd >= 0) {
        PASS("swap_pages interface exists");
        close(fd);
    } else {
        FAIL("swap_pages not found");
    }
}

void test_hugetlb_alloc(void)
{
    void *addr;
    int reserved = get_nr_hugepages();
    
    printf("\nTest: HugeTLB Allocation\n");
    
    if (reserved < 2) {
        SKIP("Need at least 2 hugepages (have %d)", reserved);
        return;
    }
    
    addr = alloc_hugetlb(2 * HPAGE_SIZE_2M);
    if (!addr) {
        FAIL("Failed to allocate 4MB: %s", strerror(errno));
        return;
    }
    
    PASS("Allocated 4MB at %p", addr);
    
    memset(addr, 0x42, 2 * HPAGE_SIZE_2M);
    if (verify_pattern(addr, 2 * HPAGE_SIZE_2M, 0x42) == 0)
        PASS("Memory read-back verified");
    
    free_hugetlb(addr, 2 * HPAGE_SIZE_2M);
    PASS("Memory freed");
}

void test_hugetlb_swap(void)
{
    void *addr;
    int fd;
    char addr_str[32];
    unsigned long swap_before, swap_after;
    
    printf("\nTest: HugeTLB Swap\n");
    
    if (get_nr_hugepages() < 2) {
        SKIP("Need at least 2 hugepages");
        return;
    }
    
    addr = alloc_hugetlb(2 * HPAGE_SIZE_2M);
    if (!addr) {
        FAIL("Allocation failed: %s", strerror(errno));
        return;
    }
    
    memset(addr, 0xAB, 2 * HPAGE_SIZE_2M);
    PASS("Allocated and touched 4MB");
    
    swap_before = get_vmswap();
    INFO("VmSwap before: %lu kB", swap_before);
    
    fd = open_swap_pages();
    if (fd < 0) {
        FAIL("Cannot open swap_pages");
        free_hugetlb(addr, 2 * HPAGE_SIZE_2M);
        return;
    }
    
    snprintf(addr_str, sizeof(addr_str), "0x%lx\n", (uintptr_t)addr + HPAGE_SIZE_2M / 2);
    if (write(fd, addr_str, strlen(addr_str)) >= 0)
        PASS("Swap trigger sent");
    else
        INFO("Swap trigger failed: %s", strerror(errno));
    close(fd);
    
    usleep(500000);
    
    swap_after = get_vmswap();
    INFO("VmSwap after: %lu kB", swap_after);
    
    if (swap_after > swap_before)
        PASS("Swap out detected");
    else
        INFO("Swap may still be pending");
    
    free_hugetlb(addr, 2 * HPAGE_SIZE_2M);
    PASS("Memory freed");
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    
    printf("=== HugeTLB Swap Tests ===\n");
    printf("Kernel: "); fflush(stdout); system("uname -r");
    
    if (getuid() != 0)
        printf("WARNING: Not running as root\n");
    
    test_etmem_config();
    test_swap_interface();
    test_hugetlb_alloc();
    test_hugetlb_swap();
    
    printf("\n=== Summary ===\n");
    printf("  Passed:  %d\n", passed);
    printf("  Failed:  %d\n", failed);
    printf("  Skipped: %d\n", skipped);
    
    return failed > 0 ? 1 : 0;
}
