#ifndef TEST_KAE_H
#define TEST_KAE_H

/* KAE deflate setup/cleanup functions
 *
 * Usage:
 *   int kae = kae_test_setup();
 *   if (kae >= 0) {
 *       // run swap test
 *       kae_test_cleanup();
 *   }
 */

/* Check if KAE (hisi_zip) module is loaded */
int kae_available(void);

/* Check if deflate is available in zram */
int zram_has_deflate(void);

/* Setup: check prerequisites, save current algo, switch to deflate
 * Returns: 1 = KAE hw available, 0 = CPU fallback, -1 = skip/error
 */
int kae_test_setup(void);

/* Cleanup: restore original zram algorithm */
void kae_test_cleanup(void);

/* Full KAE deflate swap test (setup + swap + cleanup) */
void test_kae_deflate_swap(void);

/* Deprecated - kept for compatibility */
void test_kae_compression(void);
void test_kae_fallback(void);

#endif
