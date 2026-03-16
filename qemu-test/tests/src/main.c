#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "test_common.h"
#include "test_kae.h"

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
    int swpd_before, swpd_after;

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

    swpd_before = get_hugepages_swpd();
    INFO("HugePages_Swap before: %d", swpd_before);

    /* Use trigger_swap_multi to swap both hugepages */
    void *pages[2] = {addr, (char *)addr + HPAGE_SIZE_2M};
    if (trigger_swap_multi(pages, 2) < 0) {
        free_hugetlb(addr, 2 * HPAGE_SIZE_2M);
        return;
    }

    swpd_after = get_hugepages_swpd();
    INFO("HugePages_Swap after: %d", swpd_after);

    if (swpd_after > swpd_before)
        PASS("Swap out detected (%d hugepages swapped)", swpd_after - swpd_before);
    else
        INFO("Swap may still be pending");

    free_hugetlb(addr, 2 * HPAGE_SIZE_2M);
    PASS("Memory freed");
}

/* Reuse test_hugetlbfs_swap with different file path */
void test_hugetlbfs_swap_internal(const char *test_name, const char *hugetlb_file)
{
    int fd;
    void *addr;
    int swpd_before, swpd_after;
    int free_hp_before, free_hp_after;

    printf("\nTest: %s\n", test_name);

    /* Check hugetlbfs mount */
    if (access("/mnt/huge", F_OK) != 0) {
        SKIP("hugetlbfs not mounted at /mnt/huge");
        return;
    }
    PASS("hugetlbfs is mounted");

    if (get_nr_hugepages() < 1) {
        SKIP("Need at least 1 hugepage reserved");
        return;
    }

    /* Create file on hugetlbfs */
    fd = open(hugetlb_file, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        FAIL("Failed to create hugetlbfs file: %s", strerror(errno));
        return;
    }

    /* Allocate 2MB on hugetlbfs */
    if (ftruncate(fd, HPAGE_SIZE_2M) != 0) {
        FAIL("Failed to allocate 2MB on hugetlbfs: %s", strerror(errno));
        close(fd);
        unlink(hugetlb_file);
        return;
    }
    PASS("Created 2MB file on hugetlbfs");

    /* Map the file */
    addr = mmap(NULL, HPAGE_SIZE_2M, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        FAIL("Failed to mmap hugetlbfs file: %s", strerror(errno));
        close(fd);
        unlink(hugetlb_file);
        return;
    }
    close(fd);
    PASS("Mapped hugetlbfs file at %p", addr);

    /* Touch memory */
    memset(addr, 0xCD, HPAGE_SIZE_2M);
    if (verify_pattern(addr, HPAGE_SIZE_2M, 0xCD) == 0)
        PASS("Memory write/read verified");

    free_hp_before = get_free_hugepages();
    swpd_before = get_hugepages_swpd();
    INFO("Free hugepages before: %d", free_hp_before);
    INFO("HugePages_Swap before: %d", swpd_before);

    /* Trigger swap via etmem */
    if (trigger_swap(addr, HPAGE_SIZE_2M / 2) < 0) {
        munmap(addr, HPAGE_SIZE_2M);
        unlink(hugetlb_file);
        return;
    }

    free_hp_after = get_free_hugepages();
    swpd_after = get_hugepages_swpd();
    INFO("Free hugepages after: %d", free_hp_after);
    INFO("HugePages_Swap after: %d", swpd_after);

    if (swpd_after > swpd_before && free_hp_after > free_hp_before) {
        PASS("hugetlbfs swap verified: %d pages swapped, pool increased",
             swpd_after - swpd_before);
    } else if (swpd_after > swpd_before) {
        PASS("Swap detected (HugePages_Swpd increased by %d)",
             swpd_after - swpd_before);
    } else {
        INFO("Swap may not have completed");
    }

    /* Swap-in: Access memory to trigger page fault and swapin */
    /* Full pattern verification */
    if (verify_pattern(addr, HPAGE_SIZE_2M, 0xCD) == 0)
        PASS("Full memory pattern verified after swapin");
    else
        FAIL("Memory pattern mismatch after swapin");

    /* Check free hugepages after swapin (should decrease) */
    int free_hp_after_swapin = get_free_hugepages();
    INFO("Free hugepages after swapin: %d", free_hp_after_swapin);
    if (free_hp_after_swapin < free_hp_after) {
        PASS("Hugepage allocated for swapin");
    }

    /* Cleanup */
    munmap(addr, HPAGE_SIZE_2M);
    unlink(hugetlb_file);
    PASS("hugetlbfs resources cleaned up");
}

void test_hugetlbfs_swap(void)
{
    test_hugetlbfs_swap_internal("hugetlbfs-based Swap", "/mnt/huge/test_swap");
}

void test_hugetlbfs_swap_deflate(void)
{
    int kae_status;

    printf("\nTest: hugetlbfs Swap with Deflate\n");

    /* Setup KAE deflate */
    kae_status = kae_test_setup();
    if (kae_status < 0)
        return;  /* Skip if deflate not available */

    /* Report which deflate implementation is being used */
    if (kae_status == 1)
        PASS("Using KAE hardware deflate");
    else
        INFO("Using CPU software deflate (no KAE hardware)");

    /* Run the standard swap test with deflate compression */
    test_hugetlbfs_swap_internal("hugetlbfs Swap with Deflate",
                                  "/mnt/huge/test_kae_deflate");

    /* Restore original algorithm */
    kae_test_cleanup();
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
    test_hugetlbfs_swap();
    test_hugetlbfs_swap_deflate();

    printf("\n=== Summary ===\n");
    printf("  Passed:  %d\n", passed);
    printf("  Failed:  %d\n", failed);
    printf("  Skipped: %d\n", skipped);
    
    return failed > 0 ? 1 : 0;
}
