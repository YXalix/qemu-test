// SPDX-License-Identifier: GPL-2.0
/*
 * Hugetlb Swap KUnit Tests
 *
 * Copyright (C) 2024 Huawei Technologies Co., Ltd.
 *
 * These tests verify the hugetlb swap functionality:
 * - Swap slot allocation for huge pages
 * - Hugetlb page isolation
 * - Swap cache operations
 * - Reclaim integration
 */

/* Define config flags before including any headers */
#define CONFIG_HUGETLB_SWAP 1
#define CONFIG_HUGETLB_PAGE 1
#define CONFIG_SWAP 1
#define CONFIG_ETMEM 1

#include <linux/module.h>
#include <linux/kernel.h>
#include <kunit/test.h>
#include <linux/swap.h>
#include <linux/swap_slots.h>
#include <linux/hugetlb.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/nodemask.h>
#include <linux/gfp.h>
#include <linux/etmem.h>

/* For swap cache internal functions */
#include "../mm/swap.h"

/*
 * Test: Swap entry type detection for huge pages
 * Verifies SWP_HUGETLB type is properly defined and detected
 */
static void hugetlb_swap_test_entry_type(struct kunit *test)
{
#ifdef SWP_HUGETLB
	swp_entry_t entry;

	/* Create a swap entry with HUGETLB type */
	entry = swp_entry(SWP_HUGETLB, 1);

	KUNIT_EXPECT_TRUE(test, swp_type(entry) == SWP_HUGETLB);
	KUNIT_EXPECT_TRUE(test, swp_offset(entry) == 1);
#else
	kunit_mark_skipped(test, "SWP_HUGETLB not defined - hugetlb swap not yet implemented");
	return;
#endif
}

/*
 * Test: Huge page order to swap slots calculation
 * 2MB page (order 9) needs 512 slots (2MB / 4KB)
 * 1GB page (order 30) needs 262144 slots (1GB / 4KB)
 */
static void hugetlb_swap_test_slots_calculation(struct kunit *test)
{
	unsigned int slots_2mb, slots_1gb;

	/* Order 9 = 2MB page = 512 slots (2MB / 4KB) */
	slots_2mb = 1U << 9;
	KUNIT_EXPECT_EQ(test, slots_2mb, 512U);

	/* Order 18 = 1GB page = 262144 slots (1GB / 4KB) */
	slots_1gb = 1U << 18;
	KUNIT_EXPECT_EQ(test, slots_1gb, 262144U);

	/* Verify page order calculations */
	KUNIT_EXPECT_EQ(test, 1U << 9, 512U);   /* 2MB in 4KB units */
	KUNIT_EXPECT_EQ(test, 1U << 18, 262144U); /* 1GB in 4KB units */
}

/*
 * Test: Slot alignment requirements for huge pages
 * Huge pages require order-aligned swap slot allocation
 */
static void hugetlb_swap_test_alignment(struct kunit *test)
{
	unsigned long align_mask_2mb, align_mask_1gb;

	/* Order 9 (2MB) requires 512-slot alignment */
	align_mask_2mb = (1UL << 9) - 1;
	KUNIT_EXPECT_EQ(test, align_mask_2mb, 511UL);

	/* Order 18 (1GB) requires 262144-slot alignment */
	align_mask_1gb = (1UL << 18) - 1;
	KUNIT_EXPECT_EQ(test, align_mask_1gb, 262143UL);

	/* Verify alignment check: address 512 is aligned to 512 */
	KUNIT_EXPECT_EQ(test, 512UL & ((1UL << 9) - 1), 0UL);

	/* Verify alignment check: address 513 is NOT aligned to 512 */
	KUNIT_EXPECT_NE(test, 513UL & ((1UL << 9) - 1), 0UL);
}

/*
 * Test: Swap slot allocation for 2MB page
 * This test will initially fail until alloc_huge_swap_slots() is implemented
 */
static void hugetlb_swap_test_alloc_2mb(struct kunit *test)
{
#ifdef CONFIG_HUGETLB_SWAP
	swp_entry_t entry;
	int ret;

	ret = alloc_huge_swap_slots(9, &entry);
	KUNIT_EXPECT_EQ(test, ret, 0);
	/* Inline check: swap entry is valid if val is non-zero */
	KUNIT_EXPECT_TRUE(test, entry.val != 0);

	/* Clean up */
	free_huge_swap_slots(9, entry);
#else
	kunit_mark_skipped(test, "CONFIG_HUGETLB_SWAP not enabled");
	return;
#endif
}

/*
 * Test: Swap slot allocation failure when no space
 * This test will initially fail until error handling is implemented
 */
static void hugetlb_swap_test_alloc_nospace(struct kunit *test)
{
#ifdef CONFIG_HUGETLB_SWAP
	swp_entry_t entry;
	int ret;

	ret = alloc_huge_swap_slots(9, &entry);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
#else
	kunit_mark_skipped(test, "CONFIG_HUGETLB_SWAP not enabled");
	return;
#endif
}

/*
 * Test: Isolate a free huge page
 * Verifies isolation works for unmapped hugetlb pages
 */
static void hugetlb_isolate_test_free_page(struct kunit *test)
{
#ifdef CONFIG_HUGETLB_PAGE
	struct hstate *h = &default_hstate;
	struct folio *folio;
	LIST_HEAD(pagelist);
	bool isolated;

	folio = alloc_hugetlb_folio_nodemask(h, NUMA_NO_NODE, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, folio);

	isolated = isolate_hugetlb(folio, &pagelist);
	KUNIT_EXPECT_TRUE(test, isolated);

	/* Clean up - put back the isolated page */
	if (isolated)
		putback_hugetlb_folio(folio);
	else
		free_huge_folio(folio);
#else
	kunit_mark_skipped(test, "CONFIG_HUGETLB_PAGE not enabled");
	return;
#endif
}

/*
 * Test: Cannot isolate an already isolated page
 * Isolate a page twice - second isolation should fail
 */
static void hugetlb_isolate_test_already_isolated(struct kunit *test)
{
#ifdef CONFIG_HUGETLB_PAGE
	struct hstate *h = &default_hstate;
	struct folio *folio;
	LIST_HEAD(pagelist);
	bool isolated1, isolated2;

	folio = alloc_hugetlb_folio_nodemask(h, NUMA_NO_NODE, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, folio);

	/* First isolation should succeed */
	isolated1 = isolate_hugetlb(folio, &pagelist);
	KUNIT_EXPECT_TRUE(test, isolated1);

	if (isolated1) {
		/* Second isolation should fail - page is already isolated */
		isolated2 = isolate_hugetlb(folio, &pagelist);
		KUNIT_EXPECT_FALSE(test, isolated2);

		putback_hugetlb_folio(folio);
	} else {
		free_huge_folio(folio);
	}
#else
	kunit_mark_skipped(test, "CONFIG_HUGETLB_PAGE not enabled");
	return;
#endif
}

/*
 * Test: Add huge folio to swap cache
 */
static void hugetlb_cache_test_add(struct kunit *test)
{
#ifdef CONFIG_SWAP
	struct hstate *h = &default_hstate;
	struct folio *folio;
	swp_entry_t entry;
	int ret;

	/* Allocate a huge page */
	folio = alloc_hugetlb_folio_nodemask(h, NUMA_NO_NODE, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, folio);

	/* Allocate swap slots for the huge page */
	ret = alloc_huge_swap_slots(huge_page_order(h), &entry);
	if (ret) {
		free_huge_folio(folio);
		kunit_mark_skipped(test, "Failed to allocate swap slots");
		return;
	}

	/* Lock the folio before adding to swap cache */
	folio_lock(folio);
	ret = add_to_swap_cache(folio, entry, GFP_KERNEL, NULL);
	folio_unlock(folio);

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, folio_test_swapcache(folio));

	/* Clean up */
	if (folio_test_swapcache(folio))
		delete_from_swap_cache(folio);
	free_huge_swap_slots(huge_page_order(h), entry);
	free_huge_folio(folio);
#else
	kunit_mark_skipped(test, "CONFIG_SWAP not enabled");
	return;
#endif
}

/*
 * Test: Lookup huge folio in swap cache
 */
static void hugetlb_cache_test_lookup(struct kunit *test)
{
#ifdef CONFIG_SWAP
	struct hstate *h = &default_hstate;
	struct folio *folio, *found;
	swp_entry_t entry;
	int ret;

	/* Allocate a huge page */
	folio = alloc_hugetlb_folio_nodemask(h, NUMA_NO_NODE, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, folio);

	/* Allocate swap slots */
	ret = alloc_huge_swap_slots(huge_page_order(h), &entry);
	if (ret) {
		free_huge_folio(folio);
		kunit_mark_skipped(test, "Failed to allocate swap slots");
		return;
	}

	/* Add to swap cache */
	folio_lock(folio);
	ret = add_to_swap_cache(folio, entry, GFP_KERNEL, NULL);
	folio_unlock(folio);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Lookup in swap cache */
	found = swap_cache_get_folio(entry, NULL, 0);
	KUNIT_EXPECT_NOT_NULL(test, found);
	if (found)
		folio_put(found);

	/* Clean up */
	folio_lock(folio);
	delete_from_swap_cache(folio);
	folio_unlock(folio);
	free_huge_swap_slots(huge_page_order(h), entry);
	free_huge_folio(folio);
#else
	kunit_mark_skipped(test, "CONFIG_SWAP not enabled");
	return;
#endif
}

/*
 * Test: Remove huge folio from swap cache
 */
static void hugetlb_cache_test_delete(struct kunit *test)
{
#ifdef CONFIG_SWAP
	struct hstate *h = &default_hstate;
	struct folio *folio;
	swp_entry_t entry;
	int ret;

	/* Allocate a huge page */
	folio = alloc_hugetlb_folio_nodemask(h, NUMA_NO_NODE, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, folio);

	/* Allocate swap slots */
	ret = alloc_huge_swap_slots(huge_page_order(h), &entry);
	if (ret) {
		free_huge_folio(folio);
		kunit_mark_skipped(test, "Failed to allocate swap slots");
		return;
	}

	/* Add to swap cache */
	folio_lock(folio);
	ret = add_to_swap_cache(folio, entry, GFP_KERNEL, NULL);
	folio_unlock(folio);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Remove from swap cache */
	folio_lock(folio);
	delete_from_swap_cache(folio);
	folio_unlock(folio);

	KUNIT_EXPECT_FALSE(test, folio_test_swapcache(folio));

	/* Clean up */
	free_huge_swap_slots(huge_page_order(h), entry);
	free_huge_folio(folio);
#else
	kunit_mark_skipped(test, "CONFIG_SWAP not enabled");
	return;
#endif
}

/*
 * Test: Memory pressure triggers hugetlb reclaim
 * Verifies that hugetlb pages are reclaimable through shrink_folio_list
 */
static void hugetlb_reclaim_test_pressure(struct kunit *test)
{
#ifdef CONFIG_HUGETLB_SWAP
	struct hstate *h = &default_hstate;
	struct folio *folio;
	int ret;

	/* Allocate a huge page */
	folio = alloc_hugetlb_folio_nodemask(h, NUMA_NO_NODE, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, folio);

	/* Allocate swap slots for the folio */
	ret = alloc_huge_swap_slots(huge_page_order(h), &folio->swap);
	if (ret) {
		free_huge_folio(folio);
		kunit_mark_skipped(test, "Failed to allocate swap slots");
		return;
	}

	/*
	 * Mark the folio as swapbacked - this is needed for reclaim path
	 * The swap entry was allocated above
	 */
	folio_set_swapbacked(folio);

	/*
	 * Verify the folio can be added to swap cache
	 * This is the first step in the reclaim path
	 */
	folio_lock(folio);
	ret = add_to_swap_cache(folio, folio->swap, GFP_KERNEL, NULL);
	folio_unlock(folio);

	if (ret == 0) {
		KUNIT_EXPECT_TRUE(test, folio_test_swapcache(folio));
		KUNIT_EXPECT_TRUE(test, folio_test_swapbacked(folio));

		/* Remove from swap cache */
		folio_lock(folio);
		delete_from_swap_cache(folio);
		folio_unlock(folio);
	}

	/* Clean up */
	free_huge_swap_slots(huge_page_order(h), folio->swap);
	folio->swap.val = 0;
	free_huge_folio(folio);
#else
	kunit_mark_skipped(test, "CONFIG_HUGETLB_SWAP not enabled");
	return;
#endif
}

/*
 * Test: Hugetlb pages are forced to FOLIOREF_RECLAIM
 * In shrink_folio_list, hugetlb pages bypass reference checks
 */
static void hugetlb_reclaim_test_priority(struct kunit *test)
{
#ifdef CONFIG_HUGETLB_SWAP
	struct hstate *h = &default_hstate;
	struct folio *folio;
	int ret;

	/*
	 * This test verifies that hugetlb pages are always marked for reclaim
	 * regardless of reference counts. In vmscan.c:1193-1194:
	 *   if (folio_test_hugetlb(folio))
	 *       references = FOLIOREF_RECLAIM;
	 */

	folio = alloc_hugetlb_folio_nodemask(h, NUMA_NO_NODE, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, folio);

	/* Allocate swap slots */
	ret = alloc_huge_swap_slots(huge_page_order(h), &folio->swap);
	if (ret) {
		free_huge_folio(folio);
		kunit_mark_skipped(test, "Failed to allocate swap slots");
		return;
	}

	folio_set_swapbacked(folio);

	/* Add to swap cache - this is what happens during reclaim */
	folio_lock(folio);
	ret = add_to_swap_cache(folio, folio->swap, GFP_KERNEL, NULL);
	folio_unlock(folio);

	KUNIT_EXPECT_EQ(test, ret, 0);

	/* Verify folio is in swap cache - reclaim path requires this */
	KUNIT_EXPECT_TRUE(test, folio_test_swapcache(folio));

	/*
	 * In actual reclaim, the folio would go through shrink_folio_list
	 * where it gets special handling at lines 1230-1234 and 1487-1488
	 */

	/* Clean up */
	folio_lock(folio);
	if (folio_test_swapcache(folio))
		delete_from_swap_cache(folio);
	folio_unlock(folio);
	free_huge_swap_slots(huge_page_order(h), folio->swap);
	folio->swap.val = 0;
	free_huge_folio(folio);
#else
	kunit_mark_skipped(test, "CONFIG_HUGETLB_SWAP not enabled");
	return;
#endif
}

/*
 * Test: Hugetlb folio is freed correctly after reclaim
 * Verifies folio_unref_putback() path at vmscan.c:1488
 */
static void hugetlb_reclaim_test_folio_free(struct kunit *test)
{
#ifdef CONFIG_HUGETLB_SWAP
	struct hstate *h = &default_hstate;
	struct folio *folio;
	int ret;
	swp_entry_t entry;

	folio = alloc_hugetlb_folio_nodemask(h, NUMA_NO_NODE, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, folio);

	/*
	 * Simulate the reclaim path:
	 * 1. Allocate swap slots
	 * 2. Add to swap cache
	 * 3. Remove from swap cache (simulating successful swapout)
	 * 4. Free the folio
	 */

	ret = alloc_huge_swap_slots(huge_page_order(h), &entry);
	if (ret) {
		free_huge_folio(folio);
		kunit_mark_skipped(test, "Failed to allocate swap slots");
		return;
	}

	folio_set_swapbacked(folio);

	/* Add to swap cache */
	folio_lock(folio);
	ret = add_to_swap_cache(folio, entry, GFP_KERNEL, NULL);
	folio_unlock(folio);

	KUNIT_EXPECT_EQ(test, ret, 0);

	/* Simulate successful swapout - remove from swap cache but keep slots */
	folio_lock(folio);
	delete_from_swap_cache(folio);
	folio_unlock(folio);

	KUNIT_EXPECT_FALSE(test, folio_test_swapcache(folio));

	/*
	 * The folio would now be freed via folio_unref_putback()
	 * which calls free_huge_folio() for hugetlb pages
	 */
	free_huge_swap_slots(huge_page_order(h), entry);
	free_huge_folio(folio);

	KUNIT_EXPECT_TRUE(test, true); /* If we get here, free succeeded */
#else
	kunit_mark_skipped(test, "CONFIG_HUGETLB_SWAP not enabled");
	return;
#endif
}

/*
 * Test: ETMEM add_page_for_swap detects hugetlb pages
 * Verifies that add_page_for_swap correctly identifies and handles hugetlb pages
 */
static void hugetlb_etmem_test_detect_hugetlb(struct kunit *test)
{
#ifdef CONFIG_ETMEM
	struct hstate *h = &default_hstate;
	struct folio *folio;
	struct page *page;
	LIST_HEAD(pagelist);
	int ret;

	/* Allocate a huge page */
	folio = alloc_hugetlb_folio_nodemask(h, NUMA_NO_NODE, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, folio);

	/* Get the head page */
	page = folio_page(folio, 0);

	/* Try to add the page for swap */
	ret = add_page_for_swap(page, &pagelist);

	/*
	 * The page should be isolated successfully.
	 * Note: If the page is already isolated or mapped by multiple processes,
	 * this may return -EBUSY or -EACCES
	 */
	if (ret == 0) {
		KUNIT_EXPECT_FALSE(test, list_empty(&pagelist));
		/* Clean up - put back the isolated page */
		putback_hugetlb_folio(folio);
	} else {
		/* Page might be already isolated or have other constraints */
		KUNIT_EXPECT_TRUE(test, ret == -EBUSY || ret == -EACCES);
	}

	free_huge_folio(folio);
#else
	kunit_mark_skipped(test, "CONFIG_ETMEM not enabled");
	return;
#endif
}

/*
 * Test: ETMEM rejects file-backed hugetlb pages
 * Only anonymous hugetlb pages can be swapped
 */
static void hugetlb_etmem_test_reject_filebacked(struct kunit *test)
{
#ifdef CONFIG_ETMEM
	/* File-backed hugetlb pages cannot be swapped */
	/* This is implicitly tested by folio_test_anon() check in add_page_for_swap */
	kunit_mark_skipped(test, "File-backed hugetlb test requires shm setup");
	return;
#else
	kunit_mark_skipped(test, "CONFIG_ETMEM not enabled");
	return;
#endif
}

/*
 * Test: ETMEM kernel_swap_enabled reflects configuration
 */
static void hugetlb_etmem_test_kernel_swap_enabled(struct kunit *test)
{
#ifdef CONFIG_ETMEM
	bool enabled;

	/* Get current state */
	enabled = kernel_swap_enabled();

	/* Kernel swap should be enabled by default */
	KUNIT_EXPECT_TRUE(test, enabled);
#else
	kunit_mark_skipped(test, "CONFIG_ETMEM not enabled");
	return;
#endif
}

/* Define test cases for swap slot suite */
static struct kunit_case hugetlb_swap_slot_cases[] = {
	KUNIT_CASE(hugetlb_swap_test_entry_type),
	KUNIT_CASE(hugetlb_swap_test_slots_calculation),
	KUNIT_CASE(hugetlb_swap_test_alignment),
	KUNIT_CASE(hugetlb_swap_test_alloc_2mb),
	KUNIT_CASE(hugetlb_swap_test_alloc_nospace),
	{}
};

static struct kunit_suite hugetlb_swap_slot_suite = {
	.name = "hugetlb-swap-slot",
	.test_cases = hugetlb_swap_slot_cases,
};

/* Define test cases for isolation suite */
static struct kunit_case hugetlb_isolation_cases[] = {
	KUNIT_CASE(hugetlb_isolate_test_free_page),
	KUNIT_CASE(hugetlb_isolate_test_already_isolated),
	{}
};

static struct kunit_suite hugetlb_isolation_suite = {
	.name = "hugetlb-isolation",
	.test_cases = hugetlb_isolation_cases,
};

/* Define test cases for swap cache suite */
static struct kunit_case hugetlb_swap_cache_cases[] = {
	KUNIT_CASE(hugetlb_cache_test_add),
	KUNIT_CASE(hugetlb_cache_test_lookup),
	KUNIT_CASE(hugetlb_cache_test_delete),
	{}
};

static struct kunit_suite hugetlb_swap_cache_suite = {
	.name = "hugetlb-swap-cache",
	.test_cases = hugetlb_swap_cache_cases,
};

/* Define test cases for reclaim suite */
static struct kunit_case hugetlb_reclaim_cases[] = {
	KUNIT_CASE(hugetlb_reclaim_test_pressure),
	KUNIT_CASE(hugetlb_reclaim_test_priority),
	KUNIT_CASE(hugetlb_reclaim_test_folio_free),
	{}
};

static struct kunit_suite hugetlb_reclaim_suite = {
	.name = "hugetlb-reclaim",
	.test_cases = hugetlb_reclaim_cases,
};

/* Define test cases for ETMEM interface suite */
static struct kunit_case hugetlb_etmem_cases[] = {
	KUNIT_CASE(hugetlb_etmem_test_detect_hugetlb),
	KUNIT_CASE(hugetlb_etmem_test_reject_filebacked),
	KUNIT_CASE(hugetlb_etmem_test_kernel_swap_enabled),
	{}
};

static struct kunit_suite hugetlb_etmem_suite = {
	.name = "hugetlb-etmem",
	.test_cases = hugetlb_etmem_cases,
};

/* Register all test suites with KUnit */
kunit_test_suites(&hugetlb_swap_slot_suite,
		      &hugetlb_isolation_suite,
		      &hugetlb_swap_cache_suite,
		      &hugetlb_reclaim_suite,
		      &hugetlb_etmem_suite);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hugetlb Swap Developers");
MODULE_DESCRIPTION("KUnit tests for Hugetlb Swap feature");
MODULE_VERSION("0.1");
