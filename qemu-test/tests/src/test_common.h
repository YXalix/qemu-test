#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define HPAGE_SIZE_2M (2 * 1024 * 1024)

/* Simple counters */
extern int passed;
extern int failed;
extern int skipped;

#define PASS(fmt, ...) do { passed++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } while(0)
#define FAIL(fmt, ...) do { failed++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } while(0)
#define SKIP(fmt, ...) do { skipped++; printf("  [SKIP] " fmt "\n", ##__VA_ARGS__); } while(0)
#define INFO(fmt, ...) do { printf("  [INFO] " fmt "\n", ##__VA_ARGS__); } while(0)

/* Utility functions */
int get_nr_hugepages(void);
int get_free_hugepages(void);
int set_nr_hugepages(int count);
unsigned long get_vmswap(void);
int get_hugepages_swpd(void);
int open_swap_pages(void);
void *alloc_hugetlb(size_t size);
void free_hugetlb(void *addr, size_t size);
int verify_pattern(void *addr, size_t len, unsigned char pattern);
int trigger_swap(void *addr, size_t offset);
int trigger_swap_multi(void **addrs, int count);

/* Test functions */
void test_etmem_config(void);
void test_swap_interface(void);
void test_hugetlb_alloc(void);
void test_hugetlb_swap(void);
void test_hugetlbfs_swap(void);
void test_hugetlbfs_double_mmap(void);

#endif
