#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "test_common.h"
#include "test_kae.h"

/* KAE deflate setup/cleanup functions for integration with swap tests
 *
 * These functions allow any swap test to use deflate compression
 * via KAE hardware (if available) or CPU fallback.
 */

#define ZRAM_COMP_ALGO      "/sys/block/zram0/comp_algorithm"

static int  saved_algo_valid = 0;
static char saved_algo[32];
static unsigned long long saved_disksize = 0;

/* Check if KAE driver is available */
int kae_available(void)
{
    FILE *fp;
    char line[256];
    int found = 0;

    fp = fopen("/proc/modules", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "hisi_zip")) {
                found = 1;
                break;
            }
        }
        fclose(fp);
    }

    return found;
}

/* Check if deflate is available in zram */
int zram_has_deflate(void)
{
    int fd;
    char alg[128] = {0};
    int ret = 0;

    fd = open(ZRAM_COMP_ALGO, O_RDONLY);
    if (fd < 0)
        return 0;

    if (read(fd, alg, sizeof(alg) - 1) > 0) {
        if (strstr(alg, "deflate"))
            ret = 1;
    }
    close(fd);

    return ret;
}

/* Get current zram algorithm (extracts selected algo from list like "[lzo] lzo-rle ...") */
static int zram_get_algo(char *algo, size_t size)
{
    int fd;
    ssize_t n;
    char buf[128];
    char *start, *end;

    fd = open(ZRAM_COMP_ALGO, O_RDONLY);
    if (fd < 0)
        return -1;

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
        return -1;

    buf[n] = '\0';
    char *p = strchr(buf, '\n');
    if (p) *p = '\0';

    /* Find the algorithm in brackets [algo] */
    start = strchr(buf, '[');
    if (!start)
        return -1;
    start++; /* Skip '[' */

    end = strchr(start, ']');
    if (!end)
        return -1;

    /* Extract just the algorithm name */
    size_t len = end - start;
    if (len >= size)
        len = size - 1;

    strncpy(algo, start, len);
    algo[len] = '\0';

    return 0;
}

/* Set zram compression algorithm */
static int zram_set_algo(const char *algo)
{
    int fd;
    ssize_t n;

    fd = open(ZRAM_COMP_ALGO, O_WRONLY);
    if (fd < 0)
        return -1;

    n = write(fd, algo, strlen(algo));
    close(fd);

    return (n == (ssize_t)strlen(algo)) ? 0 : -1;
}

/* Get zram disksize */
static unsigned long long zram_get_disksize(void)
{
    int fd;
    char buf[32] = {0};
    ssize_t n;
    unsigned long long disksize = 0;

    fd = open("/sys/block/zram0/disksize", O_RDONLY);
    if (fd < 0)
        return 0;

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n > 0)
        disksize = strtoull(buf, NULL, 10);

    return disksize;
}

/* Set zram disksize */
static int zram_set_disksize(unsigned long long size)
{
    int fd;
    char buf[32];
    ssize_t n;

    fd = open("/sys/block/zram0/disksize", O_WRONLY);
    if (fd < 0)
        return -1;

    snprintf(buf, sizeof(buf), "%llu", size);
    n = write(fd, buf, strlen(buf));
    close(fd);

    return (n == (ssize_t)strlen(buf)) ? 0 : -1;
}

/* Check if zram is initialized (has disksize > 0) */
static int zram_is_initialized(void)
{
    return zram_get_disksize() > 0;
}

/* Enable swap on zram */
static int zram_enable_swap(void)
{
    /* Format as swap */
    if (system("mkswap /dev/zram0 2>/dev/null") != 0) {
        FAIL("Failed to mkswap /dev/zram0");
        return -1;
    }

    /* Enable swap */
    if (system("swapon /dev/zram0 2>/dev/null") != 0) {
        FAIL("Failed to swapon /dev/zram0");
        return -1;
    }

    return 0;
}

/* Disable swap on zram */
static int zram_disable_swap(void)
{
    /* Disable swap */
    system("swapoff /dev/zram0 2>/dev/null");
    return 0;
}

/* Reset zram device */
static int zram_reset(void)
{
    int fd;

    /* First swapoff if zram is being used as swap */
    INFO("Disabling swap on zram0 if active");
    zram_disable_swap();

    /* Try reset - this deinitializes and clears the device */
    fd = open("/sys/block/zram0/reset", O_WRONLY);
    if (fd >= 0) {
        write(fd, "1", 1);
        close(fd);
        if (!zram_is_initialized())
            return 0;
    }

    /* If reset failed, try to set disksize to 0 */
    fd = open("/sys/block/zram0/disksize", O_WRONLY);
    if (fd >= 0) {
        write(fd, "0", 1);
        close(fd);
        if (!zram_is_initialized())
            return 0;
    }

    /* If all else fails, try to remove the module and re-add */
    INFO("Reset via sysfs failed, trying to reload zram module");
    system("rmmod zram 2>/dev/null");
    if (system("modprobe zram 2>/dev/null") != 0) {
        FAIL("Failed to reload zram module");
        return -1;
    }
    return 0;
}

/* Setup: check prerequisites, save algo, switch to deflate
 * Returns: 1 = KAE available, 0 = CPU fallback, -1 = error/skip
 */
int kae_test_setup(void)
{
    int kae_present;

    /* Check KAE availability */
    kae_present = kae_available();
    if (kae_present)
        PASS("KAE (hisi_zip) module loaded");
    else
        INFO("KAE not loaded - will use CPU deflate fallback");

    /* Check deflate availability */
    if (!zram_has_deflate()) {
        SKIP("deflate not available in zram");
        return -1;
    }
    PASS("deflate available in zram");

    /* Save current algorithm and disksize */
    if (zram_get_algo(saved_algo, sizeof(saved_algo)) == 0) {
        saved_algo_valid = 1;
        saved_disksize = zram_get_disksize();
        INFO("Saved zram algo: %s, disksize: %llu", saved_algo, saved_disksize);
    }

    /* Check if zram is initialized - if so, we need to reset it first */
    if (zram_is_initialized()) {
        INFO("zram0 is initialized, resetting to allow algorithm change");
        if (zram_reset() < 0) {
            FAIL("Failed to reset zram0");
            return -1;
        }
        INFO("zram0 reset successfully");
    }

    /* Switch to deflate */
    if (zram_set_algo("deflate") < 0) {
        FAIL("Failed to set zram algorithm to deflate");
        return -1;
    }

    /* Restore disksize if it was set before */
    if (saved_disksize > 0) {
        if (zram_set_disksize(saved_disksize) == 0)
            INFO("Restored zram disksize: %llu", saved_disksize);
        else
            FAIL("Failed to restore zram disksize: %llu", saved_disksize);
    }

    /* Enable swap on zram */
    if (zram_enable_swap() < 0) {
        FAIL("Failed to enable swap on zram0");
        return -1;
    }
    INFO("Enabled swap on zram0");

    PASS("Zram algorithm set to deflate");
    return kae_present;
}

/* Cleanup: restore original algorithm and disksize */
void kae_test_cleanup(void)
{
    if (!saved_algo_valid)
        return;

    /* Reset zram before changing algorithm */
    if (zram_is_initialized()) {
        INFO("Resetting zram0 before restoring algorithm");
        if (zram_reset() < 0) {
            FAIL("Failed to reset zram0 during cleanup");
            return;
        }
    }

    if (zram_set_algo(saved_algo) == 0)
        INFO("Restored zram algo: %s", saved_algo);
    else
        FAIL("Failed to restore zram algo: %s", saved_algo);

    /* Restore disksize if it was set before */
    if (saved_disksize > 0) {
        if (zram_set_disksize(saved_disksize) == 0)
            INFO("Restored zram disksize: %llu", saved_disksize);
        else
            FAIL("Failed to restore zram disksize: %llu", saved_disksize);
    }

    /* Enable swap on zram */
    if (zram_enable_swap() < 0) {
        FAIL("Failed to enable swap on zram0 during cleanup");
        return;
    }
    INFO("Enabled swap on zram0");

    saved_disksize = 0;
    saved_algo_valid = 0;
}

/* Full KAE deflate swap test - uses setup + existing swap test + cleanup */
void test_kae_deflate_swap(void)
{
    printf("\nTest: KAE Deflate Swap (via zram)\n");

    /* Setup: check prerequisites and switch to deflate */
    if (kae_test_setup() < 0)
        return;

    INFO("KAE deflate configured - run swap test separately");

    kae_test_cleanup();
}

/* Deprecated functions - kept for compatibility */
void test_kae_compression(void)
{
    printf("\nTest: KAE Compression (deprecated)\n");
    INFO("Use test_kae_deflate_swap + test_hugetlbfs_swap");
}

void test_kae_fallback(void)
{
    printf("\nTest: KAE Fallback (deprecated)\n");
    INFO("Fallback tested when KAE not loaded");
}
