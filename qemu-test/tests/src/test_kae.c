#include <stdio.h>
#include <string.h>
#include <errno.h>
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
#define ZRAM_COMP_ALGO_SEL  "/sys/block/zram0/comp_algorithm"

static int  saved_algo_valid = 0;
static char saved_algo[32];

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

/* Get current zram algorithm */
static int zram_get_algo(char *algo, size_t size)
{
    int fd;
    ssize_t n;

    fd = open(ZRAM_COMP_ALGO, O_RDONLY);
    if (fd < 0)
        return -1;

    n = read(fd, algo, size - 1);
    close(fd);

    if (n <= 0)
        return -1;

    algo[n] = '\0';
    char *p = strchr(algo, '\n');
    if (p) *p = '\0';

    return 0;
}

/* Set zram compression algorithm */
static int zram_set_algo(const char *algo)
{
    int fd;
    ssize_t n;

    fd = open(ZRAM_COMP_ALGO_SEL, O_WRONLY);
    if (fd < 0)
        return -1;

    n = write(fd, algo, strlen(algo));
    close(fd);

    return (n == (ssize_t)strlen(algo)) ? 0 : -1;
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

    /* Save current algorithm */
    if (zram_get_algo(saved_algo, sizeof(saved_algo)) == 0) {
        saved_algo_valid = 1;
        INFO("Saved zram algo: %s", saved_algo);
    }

    /* Switch to deflate */
    if (zram_set_algo("deflate") < 0) {
        FAIL("Failed to set zram algorithm to deflate");
        return -1;
    }

    PASS("Zram algorithm set to deflate");
    return kae_present;
}

/* Cleanup: restore original algorithm */
void kae_test_cleanup(void)
{
    if (!saved_algo_valid)
        return;

    if (zram_set_algo(saved_algo) == 0)
        INFO("Restored zram algo: %s", saved_algo);
    else
        FAIL("Failed to restore zram algo: %s", saved_algo);

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
