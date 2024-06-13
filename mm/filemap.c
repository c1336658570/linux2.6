/*
 *	linux/mm/filemap.c
 *
 * Copyright (C) 1994-1999  Linus Torvalds
 */

/*
 * This file handles the generic file mmap semantics used by
 * most "normal" filesystems (but you don't /have/ to use this:
 * the NFS filesystem used to do this differently, for example)
 */
#include <linux/module.h>
#include <linux/compiler.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/aio.h>
#include <linux/capability.h>
#include <linux/kernel_stat.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/hash.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/pagevec.h>
#include <linux/blkdev.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/cpuset.h>
#include <linux/hardirq.h> /* for BUG_ON(!in_atomic()) only */
#include <linux/memcontrol.h>
#include <linux/mm_inline.h> /* for page_is_file_cache() */
#include "internal.h"

/*
 * FIXME: remove all knowledge of the buffer layer from the core VM
 */
#include <linux/buffer_head.h> /* for try_to_free_buffers */

#include <asm/mman.h>

/*
 * Shared mappings implemented 30.11.1994. It's not fully working yet,
 * though.
 *
 * Shared mappings now work. 15.8.1995  Bruno.
 *
 * finished 'unifying' the page and buffer cache and SMP-threaded the
 * page-cache, 21.05.1999, Ingo Molnar <mingo@redhat.com>
 *
 * SMP-threaded pagemap-LRU 1999, Andrea Arcangeli <andrea@suse.de>
 */

/*
 * Lock ordering:
 *
 *  ->i_mmap_lock		(truncate_pagecache)
 *    ->private_lock		(__free_pte->__set_page_dirty_buffers)
 *      ->swap_lock		(exclusive_swap_page, others)
 *        ->mapping->tree_lock
 *
 *  ->i_mutex
 *    ->i_mmap_lock		(truncate->unmap_mapping_range)
 *
 *  ->mmap_sem
 *    ->i_mmap_lock
 *      ->page_table_lock or pte_lock	(various, mainly in memory.c)
 *        ->mapping->tree_lock	(arch-dependent flush_dcache_mmap_lock)
 *
 *  ->mmap_sem
 *    ->lock_page		(access_process_vm)
 *
 *  ->i_mutex			(generic_file_buffered_write)
 *    ->mmap_sem		(fault_in_pages_readable->do_page_fault)
 *
 *  ->i_mutex
 *    ->i_alloc_sem             (various)
 *
 *  ->inode_lock
 *    ->sb_lock			(fs/fs-writeback.c)
 *    ->mapping->tree_lock	(__sync_single_inode)
 *
 *  ->i_mmap_lock
 *    ->anon_vma.lock		(vma_adjust)
 *
 *  ->anon_vma.lock
 *    ->page_table_lock or pte_lock	(anon_vma_prepare and various)
 *
 *  ->page_table_lock or pte_lock
 *    ->swap_lock		(try_to_unmap_one)
 *    ->private_lock		(try_to_unmap_one)
 *    ->tree_lock		(try_to_unmap_one)
 *    ->zone.lru_lock		(follow_page->mark_page_accessed)
 *    ->zone.lru_lock		(check_pte_range->isolate_lru_page)
 *    ->private_lock		(page_remove_rmap->set_page_dirty)
 *    ->tree_lock		(page_remove_rmap->set_page_dirty)
 *    ->inode_lock		(page_remove_rmap->set_page_dirty)
 *    ->inode_lock		(zap_pte_range->set_page_dirty)
 *    ->private_lock		(zap_pte_range->__set_page_dirty_buffers)
 *
 *  ->task->proc_lock
 *    ->dcache_lock		(proc_pid_lookup)
 *
 *  (code doesn't rely on that order, so you could switch it around)
 *  ->tasklist_lock             (memory_failure, collect_procs_ao)
 *    ->i_mmap_lock
 */

/*
 * Remove a page from the page cache and free it. Caller has to make
 * sure the page is locked and that nobody else uses it - or that usage
 * is safe.  The caller must hold the mapping's tree_lock.
 */
void __remove_from_page_cache(struct page *page)
{
	struct address_space *mapping = page->mapping;

	radix_tree_delete(&mapping->page_tree, page->index);
	page->mapping = NULL;
	mapping->nrpages--;
	__dec_zone_page_state(page, NR_FILE_PAGES);
	if (PageSwapBacked(page))
		__dec_zone_page_state(page, NR_SHMEM);
	BUG_ON(page_mapped(page));

	/*
	 * Some filesystems seem to re-dirty the page even after
	 * the VM has canceled the dirty bit (eg ext3 journaling).
	 *
	 * Fix it up by doing a final dirty accounting check after
	 * having removed the page entirely.
	 */
	if (PageDirty(page) && mapping_cap_account_dirty(mapping)) {
		dec_zone_page_state(page, NR_FILE_DIRTY);
		dec_bdi_stat(mapping->backing_dev_info, BDI_RECLAIMABLE);
	}
}

void remove_from_page_cache(struct page *page)
{
	struct address_space *mapping = page->mapping;

	BUG_ON(!PageLocked(page));

	spin_lock_irq(&mapping->tree_lock);
	__remove_from_page_cache(page);
	spin_unlock_irq(&mapping->tree_lock);
	mem_cgroup_uncharge_cache_page(page);
}

static int sync_page(void *word)
{
	struct address_space *mapping;
	struct page *page;

	page = container_of((unsigned long *)word, struct page, flags);

	/*
	 * page_mapping() is being called without PG_locked held.
	 * Some knowledge of the state and use of the page is used to
	 * reduce the requirements down to a memory barrier.
	 * The danger here is of a stale page_mapping() return value
	 * indicating a struct address_space different from the one it's
	 * associated with when it is associated with one.
	 * After smp_mb(), it's either the correct page_mapping() for
	 * the page, or an old page_mapping() and the page's own
	 * page_mapping() has gone NULL.
	 * The ->sync_page() address_space operation must tolerate
	 * page_mapping() going NULL. By an amazing coincidence,
	 * this comes about because none of the users of the page
	 * in the ->sync_page() methods make essential use of the
	 * page_mapping(), merely passing the page down to the backing
	 * device's unplug functions when it's non-NULL, which in turn
	 * ignore it for all cases but swap, where only page_private(page) is
	 * of interest. When page_mapping() does go NULL, the entire
	 * call stack gracefully ignores the page and returns.
	 * -- wli
	 */
	smp_mb();
	mapping = page_mapping(page);
	if (mapping && mapping->a_ops && mapping->a_ops->sync_page)
		mapping->a_ops->sync_page(page);
	io_schedule();
	return 0;
}

static int sync_page_killable(void *word)
{
	sync_page(word);
	return fatal_signal_pending(current) ? -EINTR : 0;
}

/**
 * __filemap_fdatawrite_range - start writeback on mapping dirty pages in range
 * @mapping:	address space structure to write
 * @start:	offset in bytes where the range starts
 * @end:	offset in bytes where the range ends (inclusive)
 * @sync_mode:	enable synchronous operation
 *
 * Start writeback against all of a mapping's dirty pages that lie
 * within the byte offsets <start, end> inclusive.
 *
 * If sync_mode is WB_SYNC_ALL then this is a "data integrity" operation, as
 * opposed to a regular memory cleansing writeback.  The difference between
 * these two operations is that if a dirty page/buffer is encountered, it must
 * be waited upon, and not just skipped over.
 */
/**
 * __filemark_fdatawrite_range - 开始对映射中范围内的脏页面进行写回
 * @mapping:	需要写回的地址空间结构
 * @start:	范围开始的字节偏移
 * @end:	范围结束的字节偏移（包含在内）
 * @sync_mode:	启用同步操作
 *
 * 对地址空间中位于字节偏移 <start, end>（包括边界）内的所有脏页面开始写回操作。
 *
 * 如果 sync_mode 是 WB_SYNC_ALL，则这是一个“数据完整性”操作，与普通的内存清理写回不同。
 * 这两种操作的区别在于，如果遇到脏页/缓冲区，必须等待它，而不是简单地跳过。
 */
int __filemap_fdatawrite_range(struct address_space *mapping, loff_t start,
				loff_t end, int sync_mode)
{
	int ret;
	// 初始化写回控制结构
	struct writeback_control wbc = {
		.sync_mode = sync_mode,		// 设置同步模式
		.nr_to_write = LONG_MAX,	// 设置要写回的页数为最大，即尽可能多地写回
		.range_start = start,			// 设置写回范围的起始位置
		.range_end = end,					// 设置写回范围的结束位置
	};

	// 如果映射的地址空间不支持写回脏页，则直接返回
	if (!mapping_cap_writeback_dirty(mapping))
		return 0;

	// 执行写回操作
	ret = do_writepages(mapping, &wbc);
	return ret;
}

/**
 * __filemap_fdatawrite - 执行映射写回的辅助函数
 * @mapping: 目标地址空间
 * @sync_mode: 同步模式
 * 
 * 此内联函数调用 __filemap_fdatawrite_range 函数来写回从起始位置到最大长整数范围内的所有脏页面。
 */
static inline int __filemap_fdatawrite(struct address_space *mapping,
	int sync_mode)
{
	return __filemap_fdatawrite_range(mapping, 0, LLONG_MAX, sync_mode);
}

/**
 * filemap_fdatawrite - 启动对所有脏页面的写回
 * @mapping: 目标地址空间
 * 
 * 此函数用于启动对地址空间中所有脏页面的数据完整性写回。
 */
int filemap_fdatawrite(struct address_space *mapping)
{
	return __filemap_fdatawrite(mapping, WB_SYNC_ALL);
}
EXPORT_SYMBOL(filemap_fdatawrite);

/**
 * filemap_fdatawrite_range - 启动对特定范围内所有脏页面的写回
 * @mapping: 目标地址空间
 * @start: 起始字节偏移
 * @end: 结束字节偏移
 * 
 * 此函数用于启动对地址空间中特定范围内所有脏页面的数据完整性写回。
 */
int filemap_fdatawrite_range(struct address_space *mapping, loff_t start,
				loff_t end)
{
	return __filemap_fdatawrite_range(mapping, start, end, WB_SYNC_ALL);
}
EXPORT_SYMBOL(filemap_fdatawrite_range);

/**
 * filemap_flush - mostly a non-blocking flush
 * @mapping:	target address_space
 *
 * This is a mostly non-blocking flush.  Not suitable for data-integrity
 * purposes - I/O may not be started against all dirty pages.
 */

/**
 * filemap_flush - 主要是一个非阻塞刷新操作
 * @mapping: 目标地址空间
 * 
 * 这是一个主要的非阻塞刷新操作。不适用于数据完整性目的——可能不会对所有脏页面启动 I/O。
 */
int filemap_flush(struct address_space *mapping)
{
	return __filemap_fdatawrite(mapping, WB_SYNC_NONE);
}
EXPORT_SYMBOL(filemap_flush);

/**
 * filemap_fdatawait_range - wait for writeback to complete
 * @mapping:		address space structure to wait for
 * @start_byte:		offset in bytes where the range starts
 * @end_byte:		offset in bytes where the range ends (inclusive)
 *
 * Walk the list of under-writeback pages of the given address space
 * in the given range and wait for all of them.
 */
/**
 * filemap_fdatawait_range - 等待写回完成
 * @mapping: 要等待的地址空间结构体
 * @start_byte: 范围的开始字节偏移
 * @end_byte: 范围的结束字节偏移（包含）
 *
 * 遍历给定地址空间在指定范围内正在写回的页面列表并等待所有这些页面。
 */
// 这个函数通过循环遍历给定范围内的所有页面，等待每个页面的写回操作完成，并检查是否有
// 任何页面在写回过程中出现错误。如果有错误发生，函数将返回错误代码。这个函数是内核中
// 处理文件数据同步的重要部分，确保数据的完整性。
int filemap_fdatawait_range(struct address_space *mapping, loff_t start_byte,
			    loff_t end_byte)
{
	// 计算起始和结束的页面索引
	pgoff_t index = start_byte >> PAGE_CACHE_SHIFT;
	pgoff_t end = end_byte >> PAGE_CACHE_SHIFT;
	struct pagevec pvec;
	int nr_pages;
	int ret = 0;

	// 如果结束字节小于开始字节，则直接返回，无需处理
	if (end_byte < start_byte)
		return 0;

	// 初始化页面向量
	pagevec_init(&pvec, 0);
	// 循环通过页面向量查找和处理所有标记为正在写回的页面
	while ((index <= end) &&
			(nr_pages = pagevec_lookup_tag(&pvec, mapping, &index,
			PAGECACHE_TAG_WRITEBACK,
			min(end - index, (pgoff_t)PAGEVEC_SIZE-1) + 1)) != 0) {
		unsigned i;

		// 遍历所有找到的页面
		for (i = 0; i < nr_pages; i++) {
			struct page *page = pvec.pages[i];

			/* until radix tree lookup accepts end_index */
			// 如果页面的索引超出了结束索引，则跳过
			if (page->index > end)
				continue;

			// 等待页面的写回完成
			wait_on_page_writeback(page);
			// 如果页面有错误，则设置返回值为 -EIO
			if (PageError(page))
				ret = -EIO;
		}
		// 释放页面向量
		pagevec_release(&pvec);
		// 调度点检查
		cond_resched();
	}

	/* Check for outstanding write errors */
	// 检查是否有未处理的写错误
	if (test_and_clear_bit(AS_ENOSPC, &mapping->flags))
		ret = -ENOSPC;
	if (test_and_clear_bit(AS_EIO, &mapping->flags))
		ret = -EIO;

	return ret;
}
EXPORT_SYMBOL(filemap_fdatawait_range);

/**
 * filemap_fdatawait - wait for all under-writeback pages to complete
 * @mapping: address space structure to wait for
 *
 * Walk the list of under-writeback pages of the given address space
 * and wait for all of them.
 */
// filemap_fdatawait - 等待所有正在写回的页面完成
// @mapping: 要等待的地址空间结构体
int filemap_fdatawait(struct address_space *mapping)
{
	// 读取文件大小
	loff_t i_size = i_size_read(mapping->host);

	// 如果文件大小为0，没有什么要等待的，直接返回成功
	if (i_size == 0)
		return 0;

	// 等待从0到文件大小的所有页面
	return filemap_fdatawait_range(mapping, 0, i_size - 1);
}
EXPORT_SYMBOL(filemap_fdatawait);

// filemap_write_and_wait - 写入并等待所有页面
int filemap_write_and_wait(struct address_space *mapping)
{
	int err = 0;

	// 如果有页面需要处理
	if (mapping->nrpages) {
		// 写入所有脏页
		err = filemap_fdatawrite(mapping);
		/*
		 * Even if the above returned error, the pages may be
		 * written partially (e.g. -ENOSPC), so we wait for it.
		 * But the -EIO is special case, it may indicate the worst
		 * thing (e.g. bug) happened, so we avoid waiting for it.
		 */
		if (err != -EIO) {	// 如果没有遇到I/O错误
			// 然后等待所有页面
			int err2 = filemap_fdatawait(mapping);
			// 如果之前没有错误，返回这个等待的结果
			if (!err)
				err = err2;
		}
	}
	return err;
}
EXPORT_SYMBOL(filemap_write_and_wait);

/**
 * filemap_write_and_wait_range - write out & wait on a file range
 * @mapping:	the address_space for the pages
 * @lstart:	offset in bytes where the range starts
 * @lend:	offset in bytes where the range ends (inclusive)
 *
 * Write out and wait upon file offsets lstart->lend, inclusive.
 *
 * Note that `lend' is inclusive (describes the last byte to be written) so
 * that this function can be used to write to the very end-of-file (end = -1).
 */
/**
 * filemap_write_and_wait_range - 写出并等待文件的一个区域
 * @mapping: 负责页面的地址空间
 * @lstart: 区域开始的字节偏移量
 * @lend: 区域结束的字节偏移量（包括在内）
 *
 * 写出并等待文件偏移量lstart到lend之间的数据，包括边界。
 *
 * 注意，`lend`是包含在内的（描述要写的最后一个字节），
 * 这样这个函数可以用来写文件的最末端（end = -1）。
 */
// filemap_write_and_wait_range - 写出并等待文件的一个范围
int filemap_write_and_wait_range(struct address_space *mapping,
				 loff_t lstart, loff_t lend)
{
	int err = 0;	// 初始化错误码为0

	// 如果有页面需要处理
	if (mapping->nrpages) {
		// 调用__filemap_fdatawrite_range函数，以同步方式写出指定范围的页面
		err = __filemap_fdatawrite_range(mapping, lstart, lend,
						 WB_SYNC_ALL);
		/* See comment of filemap_write_and_wait() */
		/* 参见 filemap_write_and_wait() 的注释 */
		// 如果没有遇到I/O错误
		if (err != -EIO) {
			// 等待这个范围内的页面写入完成
			int err2 = filemap_fdatawait_range(mapping,
						lstart, lend);
			// 如果之前没有错误，返回这个等待的结果
			if (!err)
				err = err2;
		}
	}
	return err;
}
EXPORT_SYMBOL(filemap_write_and_wait_range);

/**
 * add_to_page_cache_locked - add a locked page to the pagecache
 * @page:	page to add
 * @mapping:	the page's address_space
 * @offset:	page index
 * @gfp_mask:	page allocation mode
 *
 * This function is used to add a page to the pagecache. It must be locked.
 * This function does not add the page to the LRU.  The caller must do that.
 */
int add_to_page_cache_locked(struct page *page, struct address_space *mapping,
		pgoff_t offset, gfp_t gfp_mask)
{
	int error;

	VM_BUG_ON(!PageLocked(page));

	error = mem_cgroup_cache_charge(page, current->mm,
					gfp_mask & GFP_RECLAIM_MASK);
	if (error)
		goto out;

	error = radix_tree_preload(gfp_mask & ~__GFP_HIGHMEM);
	if (error == 0) {
		page_cache_get(page);
		page->mapping = mapping;
		page->index = offset;

		spin_lock_irq(&mapping->tree_lock);
		error = radix_tree_insert(&mapping->page_tree, offset, page);
		if (likely(!error)) {
			mapping->nrpages++;
			__inc_zone_page_state(page, NR_FILE_PAGES);
			if (PageSwapBacked(page))
				__inc_zone_page_state(page, NR_SHMEM);
			spin_unlock_irq(&mapping->tree_lock);
		} else {
			page->mapping = NULL;
			spin_unlock_irq(&mapping->tree_lock);
			mem_cgroup_uncharge_cache_page(page);
			page_cache_release(page);
		}
		radix_tree_preload_end();
	} else
		mem_cgroup_uncharge_cache_page(page);
out:
	return error;
}
EXPORT_SYMBOL(add_to_page_cache_locked);

// 通过page_cache_alloc_cold函数分配一个新页面，然后调用add_to_page_cache_lru将其加入到页面调整缓存。
// 用于将一个页面添加到页缓存和最近最少使用（LRU）列表中。
int add_to_page_cache_lru(struct page *page, struct address_space *mapping,
				pgoff_t offset, gfp_t gfp_mask)
{
	int ret;

	/*
	 * Splice_read and readahead add shmem/tmpfs pages into the page cache
	 * before shmem_readpage has a chance to mark them as SwapBacked: they
	 * need to go on the active_anon lru below, and mem_cgroup_cache_charge
	 * (called in add_to_page_cache) needs to know where they're going too.
	 */
	/*
	 * Splice_read 和 readahead 在 shmem_readpage 有机会标记它们为 SwapBacked 之前
	 * 就已经将 shmem/tmpfs 页面加入到页缓存中：它们需要被添加到下面的 active_anon lru 中，
	 * 并且 mem_cgroup_cache_charge（在 add_to_page_cache 中调用）也需要知道它们的去向。
	 */
	if (mapping_cap_swap_backed(mapping))
		// 如果映射支持交换后备，则设置页面的 SwapBacked 标志
		SetPageSwapBacked(page);

	// 尝试将页面添加到页缓存中
	ret = add_to_page_cache(page, mapping, offset, gfp_mask);
	if (ret == 0) {	// 如果添加成功
		if (page_is_file_cache(page))
			// 如果是文件缓存页面，将其添加到文件缓存的 LRU 列表中
			lru_cache_add_file(page);
		else
			// 否则，将其添加到活跃匿名页面的 LRU 列表中
			lru_cache_add_active_anon(page);
	}
	// 返回操作结果，0 表示成功，其他值表示错误代码
	return ret;
}
EXPORT_SYMBOL_GPL(add_to_page_cache_lru);

#ifdef CONFIG_NUMA
struct page *__page_cache_alloc(gfp_t gfp)
{
	if (cpuset_do_page_mem_spread()) {
		int n = cpuset_mem_spread_node();
		return alloc_pages_exact_node(n, gfp, 0);
	}
	return alloc_pages(gfp, 0);
}
EXPORT_SYMBOL(__page_cache_alloc);
#endif

static int __sleep_on_page_lock(void *word)
{
	io_schedule();
	return 0;
}

/*
 * In order to wait for pages to become available there must be
 * waitqueues associated with pages. By using a hash table of
 * waitqueues where the bucket discipline is to maintain all
 * waiters on the same queue and wake all when any of the pages
 * become available, and for the woken contexts to check to be
 * sure the appropriate page became available, this saves space
 * at a cost of "thundering herd" phenomena during rare hash
 * collisions.
 */
static wait_queue_head_t *page_waitqueue(struct page *page)
{
	const struct zone *zone = page_zone(page);

	return &zone->wait_table[hash_ptr(page, zone->wait_table_bits)];
}

static inline void wake_up_page(struct page *page, int bit)
{
	__wake_up_bit(page_waitqueue(page), &page->flags, bit);
}

void wait_on_page_bit(struct page *page, int bit_nr)
{
	DEFINE_WAIT_BIT(wait, &page->flags, bit_nr);

	if (test_bit(bit_nr, &page->flags))
		__wait_on_bit(page_waitqueue(page), &wait, sync_page,
							TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(wait_on_page_bit);

/**
 * add_page_wait_queue - Add an arbitrary waiter to a page's wait queue
 * @page: Page defining the wait queue of interest
 * @waiter: Waiter to add to the queue
 *
 * Add an arbitrary @waiter to the wait queue for the nominated @page.
 */
void add_page_wait_queue(struct page *page, wait_queue_t *waiter)
{
	wait_queue_head_t *q = page_waitqueue(page);
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	__add_wait_queue(q, waiter);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL_GPL(add_page_wait_queue);

/**
 * unlock_page - unlock a locked page
 * @page: the page
 *
 * Unlocks the page and wakes up sleepers in ___wait_on_page_locked().
 * Also wakes sleepers in wait_on_page_writeback() because the wakeup
 * mechananism between PageLocked pages and PageWriteback pages is shared.
 * But that's OK - sleepers in wait_on_page_writeback() just go back to sleep.
 *
 * The mb is necessary to enforce ordering between the clear_bit and the read
 * of the waitqueue (to avoid SMP races with a parallel wait_on_page_locked()).
 */
void unlock_page(struct page *page)
{
	VM_BUG_ON(!PageLocked(page));
	clear_bit_unlock(PG_locked, &page->flags);
	smp_mb__after_clear_bit();
	wake_up_page(page, PG_locked);
}
EXPORT_SYMBOL(unlock_page);

/**
 * end_page_writeback - end writeback against a page
 * @page: the page
 */
void end_page_writeback(struct page *page)
{
	if (TestClearPageReclaim(page))
		rotate_reclaimable_page(page);

	if (!test_clear_page_writeback(page))
		BUG();

	smp_mb__after_clear_bit();
	wake_up_page(page, PG_writeback);
}
EXPORT_SYMBOL(end_page_writeback);

/**
 * __lock_page - get a lock on the page, assuming we need to sleep to get it
 * @page: the page to lock
 *
 * Ugly. Running sync_page() in state TASK_UNINTERRUPTIBLE is scary.  If some
 * random driver's requestfn sets TASK_RUNNING, we could busywait.  However
 * chances are that on the second loop, the block layer's plug list is empty,
 * so sync_page() will then return in state TASK_UNINTERRUPTIBLE.
 */
void __lock_page(struct page *page)
{
	DEFINE_WAIT_BIT(wait, &page->flags, PG_locked);

	__wait_on_bit_lock(page_waitqueue(page), &wait, sync_page,
							TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(__lock_page);

int __lock_page_killable(struct page *page)
{
	DEFINE_WAIT_BIT(wait, &page->flags, PG_locked);

	return __wait_on_bit_lock(page_waitqueue(page), &wait,
					sync_page_killable, TASK_KILLABLE);
}
EXPORT_SYMBOL_GPL(__lock_page_killable);

/**
 * __lock_page_nosync - get a lock on the page, without calling sync_page()
 * @page: the page to lock
 *
 * Variant of lock_page that does not require the caller to hold a reference
 * on the page's mapping.
 */
void __lock_page_nosync(struct page *page)
{
	DEFINE_WAIT_BIT(wait, &page->flags, PG_locked);
	__wait_on_bit_lock(page_waitqueue(page), &wait, __sleep_on_page_lock,
							TASK_UNINTERRUPTIBLE);
}

/**
 * find_get_page - find and get a page reference
 * @mapping: the address_space to search
 * @offset: the page index
 *
 * Is there a pagecache struct page at the given (mapping, offset) tuple?
 * If yes, increment its refcount and return it; if no, return NULL.
 */
/**
 * find_get_page - 查找并获取一个页面引用
 * @mapping: 要搜索的地址空间
 * @offset: 页面索引
 *
 * 在给定的 (mapping, offset) 元组中是否存在一个页面缓存的 struct page？
 * 如果存在，增加其引用计数并返回它；如果不存在，返回 NULL。
 */
// mapping是指定地址空间，offset是文件中的指定位置，以页面为单位
struct page *find_get_page(struct address_space *mapping, pgoff_t offset)
{
	void **pagep;		// 用于指向基数树槽位的指针
	struct page *page;	// 用于存放找到的页面指针

	rcu_read_lock();	// 开始一个读取者区域，用于RCU同步
repeat:
	page = NULL;
	// 在基数树中查找给定偏移量的槽位
	pagep = radix_tree_lookup_slot(&mapping->page_tree, offset);
	if (pagep) {	// 如果找到槽位
	// 解引用槽位以获取页面指针
		page = radix_tree_deref_slot(pagep);
		if (unlikely(!page || page == RADIX_TREE_RETRY))
			goto repeat;	// 如果页面指针无效或需要重试，则重复查找

		if (!page_cache_get_speculative(page))
			goto repeat;	// 如果无法增加页面的引用计数，则重复查找

		/*
		 * Has the page moved?
		 * This is part of the lockless pagecache protocol. See
		 * include/linux/pagemap.h for details.
		 */
		/*
		 * 页面是否已移动？
		 * 这是无锁页面缓存协议的一部分。详细信息见 include/linux/pagemap.h。
		 */
		if (unlikely(page != *pagep)) {	// 检查页面指针是否与槽位中的指针不一致
			page_cache_release(page);	// 释放页面引用
			goto repeat;
		}
	}
	rcu_read_unlock();	// 结束RCU读取者区域

	return page;	// 返回找到的页面，或NULL
}
EXPORT_SYMBOL(find_get_page);

/**
 * find_lock_page - locate, pin and lock a pagecache page
 * @mapping: the address_space to search
 * @offset: the page index
 *
 * Locates the desired pagecache page, locks it, increments its reference
 * count and returns its address.
 *
 * Returns zero if the page was not present. find_lock_page() may sleep.
 */
struct page *find_lock_page(struct address_space *mapping, pgoff_t offset)
{
	struct page *page;

repeat:
	page = find_get_page(mapping, offset);
	if (page) {
		lock_page(page);
		/* Has the page been truncated? */
		if (unlikely(page->mapping != mapping)) {
			unlock_page(page);
			page_cache_release(page);
			goto repeat;
		}
		VM_BUG_ON(page->index != offset);
	}
	return page;
}
EXPORT_SYMBOL(find_lock_page);

/**
 * find_or_create_page - locate or add a pagecache page
 * @mapping: the page's address_space
 * @index: the page's index into the mapping
 * @gfp_mask: page allocation mode
 *
 * Locates a page in the pagecache.  If the page is not present, a new page
 * is allocated using @gfp_mask and is added to the pagecache and to the VM's
 * LRU list.  The returned page is locked and has its reference count
 * incremented.
 *
 * find_or_create_page() may sleep, even if @gfp_flags specifies an atomic
 * allocation!
 *
 * find_or_create_page() returns the desired page's address, or zero on
 * memory exhaustion.
 */
struct page *find_or_create_page(struct address_space *mapping,
		pgoff_t index, gfp_t gfp_mask)
{
	struct page *page;
	int err;
repeat:
	page = find_lock_page(mapping, index);
	if (!page) {
		page = __page_cache_alloc(gfp_mask);
		if (!page)
			return NULL;
		/*
		 * We want a regular kernel memory (not highmem or DMA etc)
		 * allocation for the radix tree nodes, but we need to honour
		 * the context-specific requirements the caller has asked for.
		 * GFP_RECLAIM_MASK collects those requirements.
		 */
		err = add_to_page_cache_lru(page, mapping, index,
			(gfp_mask & GFP_RECLAIM_MASK));
		if (unlikely(err)) {
			page_cache_release(page);
			page = NULL;
			if (err == -EEXIST)
				goto repeat;
		}
	}
	return page;
}
EXPORT_SYMBOL(find_or_create_page);

/**
 * find_get_pages - gang pagecache lookup
 * @mapping:	The address_space to search
 * @start:	The starting page index
 * @nr_pages:	The maximum number of pages
 * @pages:	Where the resulting pages are placed
 *
 * find_get_pages() will search for and return a group of up to
 * @nr_pages pages in the mapping.  The pages are placed at @pages.
 * find_get_pages() takes a reference against the returned pages.
 *
 * The search returns a group of mapping-contiguous pages with ascending
 * indexes.  There may be holes in the indices due to not-present pages.
 *
 * find_get_pages() returns the number of pages which were found.
 */
unsigned find_get_pages(struct address_space *mapping, pgoff_t start,
			    unsigned int nr_pages, struct page **pages)
{
	unsigned int i;
	unsigned int ret;
	unsigned int nr_found;

	rcu_read_lock();
restart:
	nr_found = radix_tree_gang_lookup_slot(&mapping->page_tree,
				(void ***)pages, start, nr_pages);
	ret = 0;
	for (i = 0; i < nr_found; i++) {
		struct page *page;
repeat:
		page = radix_tree_deref_slot((void **)pages[i]);
		if (unlikely(!page))
			continue;
		/*
		 * this can only trigger if nr_found == 1, making livelock
		 * a non issue.
		 */
		if (unlikely(page == RADIX_TREE_RETRY))
			goto restart;

		if (!page_cache_get_speculative(page))
			goto repeat;

		/* Has the page moved? */
		if (unlikely(page != *((void **)pages[i]))) {
			page_cache_release(page);
			goto repeat;
		}

		pages[ret] = page;
		ret++;
	}
	rcu_read_unlock();
	return ret;
}

/**
 * find_get_pages_contig - gang contiguous pagecache lookup
 * @mapping:	The address_space to search
 * @index:	The starting page index
 * @nr_pages:	The maximum number of pages
 * @pages:	Where the resulting pages are placed
 *
 * find_get_pages_contig() works exactly like find_get_pages(), except
 * that the returned number of pages are guaranteed to be contiguous.
 *
 * find_get_pages_contig() returns the number of pages which were found.
 */
unsigned find_get_pages_contig(struct address_space *mapping, pgoff_t index,
			       unsigned int nr_pages, struct page **pages)
{
	unsigned int i;
	unsigned int ret;
	unsigned int nr_found;

	rcu_read_lock();
restart:
	nr_found = radix_tree_gang_lookup_slot(&mapping->page_tree,
				(void ***)pages, index, nr_pages);
	ret = 0;
	for (i = 0; i < nr_found; i++) {
		struct page *page;
repeat:
		page = radix_tree_deref_slot((void **)pages[i]);
		if (unlikely(!page))
			continue;
		/*
		 * this can only trigger if nr_found == 1, making livelock
		 * a non issue.
		 */
		if (unlikely(page == RADIX_TREE_RETRY))
			goto restart;

		if (page->mapping == NULL || page->index != index)
			break;

		if (!page_cache_get_speculative(page))
			goto repeat;

		/* Has the page moved? */
		if (unlikely(page != *((void **)pages[i]))) {
			page_cache_release(page);
			goto repeat;
		}

		pages[ret] = page;
		ret++;
		index++;
	}
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL(find_get_pages_contig);

/**
 * find_get_pages_tag - find and return pages that match @tag
 * @mapping:	the address_space to search
 * @index:	the starting page index
 * @tag:	the tag index
 * @nr_pages:	the maximum number of pages
 * @pages:	where the resulting pages are placed
 *
 * Like find_get_pages, except we only return pages which are tagged with
 * @tag.   We update @index to index the next page for the traversal.
 */
unsigned find_get_pages_tag(struct address_space *mapping, pgoff_t *index,
			int tag, unsigned int nr_pages, struct page **pages)
{
	unsigned int i;
	unsigned int ret;
	unsigned int nr_found;

	rcu_read_lock();
restart:
	nr_found = radix_tree_gang_lookup_tag_slot(&mapping->page_tree,
				(void ***)pages, *index, nr_pages, tag);
	ret = 0;
	for (i = 0; i < nr_found; i++) {
		struct page *page;
repeat:
		page = radix_tree_deref_slot((void **)pages[i]);
		if (unlikely(!page))
			continue;
		/*
		 * this can only trigger if nr_found == 1, making livelock
		 * a non issue.
		 */
		if (unlikely(page == RADIX_TREE_RETRY))
			goto restart;

		if (!page_cache_get_speculative(page))
			goto repeat;

		/* Has the page moved? */
		if (unlikely(page != *((void **)pages[i]))) {
			page_cache_release(page);
			goto repeat;
		}

		pages[ret] = page;
		ret++;
	}
	rcu_read_unlock();

	if (ret)
		*index = pages[ret - 1]->index + 1;

	return ret;
}
EXPORT_SYMBOL(find_get_pages_tag);

/**
 * grab_cache_page_nowait - returns locked page at given index in given cache
 * @mapping: target address_space
 * @index: the page index
 *
 * Same as grab_cache_page(), but do not wait if the page is unavailable.
 * This is intended for speculative data generators, where the data can
 * be regenerated if the page couldn't be grabbed.  This routine should
 * be safe to call while holding the lock for another page.
 *
 * Clear __GFP_FS when allocating the page to avoid recursion into the fs
 * and deadlock against the caller's locked page.
 */
struct page *
grab_cache_page_nowait(struct address_space *mapping, pgoff_t index)
{
	struct page *page = find_get_page(mapping, index);

	if (page) {
		if (trylock_page(page))
			return page;
		page_cache_release(page);
		return NULL;
	}
	page = __page_cache_alloc(mapping_gfp_mask(mapping) & ~__GFP_FS);
	if (page && add_to_page_cache_lru(page, mapping, index, GFP_NOFS)) {
		page_cache_release(page);
		page = NULL;
	}
	return page;
}
EXPORT_SYMBOL(grab_cache_page_nowait);

/*
 * CD/DVDs are error prone. When a medium error occurs, the driver may fail
 * a _large_ part of the i/o request. Imagine the worst scenario:
 *
 *      ---R__________________________________________B__________
 *         ^ reading here                             ^ bad block(assume 4k)
 *
 * read(R) => miss => readahead(R...B) => media error => frustrating retries
 * => failing the whole request => read(R) => read(R+1) =>
 * readahead(R+1...B+1) => bang => read(R+2) => read(R+3) =>
 * readahead(R+3...B+2) => bang => read(R+3) => read(R+4) =>
 * readahead(R+4...B+3) => bang => read(R+4) => read(R+5) => ......
 *
 * It is going insane. Fix it by quickly scaling down the readahead size.
 */
static void shrink_readahead_size_eio(struct file *filp,
					struct file_ra_state *ra)
{
	ra->ra_pages /= 4;
}

/**
 * do_generic_file_read - generic file read routine
 * @filp:	the file to read
 * @ppos:	current file position
 * @desc:	read_descriptor
 * @actor:	read method
 *
 * This is a generic file read routine, and uses the
 * mapping->a_ops->readpage() function for the actual low-level stuff.
 *
 * This is really ugly. But the goto's actually try to clarify some
 * of the logic when it comes to error handling etc.
 */
/**
 * do_generic_file_read - 通用文件读取例程
 * @filp:	要读取的文件
 * @ppos:	当前文件位置
 * @desc:	读取描述符
 * @actor:	读取方法
 *
 * 这是所有可以直接使用页面缓存的文件系统的通用文件读取例程。
 * 它使用 mapping->a_ops->readpage() 函数进行实际的底层操作。
 *
 * 这实际上很复杂。但是goto语句实际上是为了在错误处理等逻辑时提供清晰度。
 */
static void do_generic_file_read(struct file *filp, loff_t *ppos,
		read_descriptor_t *desc, read_actor_t actor)
{
	/**
	 * // 在页高速缓存找需要的数据
	 * page = find_get_page(mapping, index)
	 * // 如果要找的页没在页高速缓存，find_get_page返回NULL。内核需要分配一个新页面，并
	 * // 将其加入到页高速缓存中
	 * page = page_cache_alloc_cold(mapping);
	 * if (!page) {
	 * // 内存分配出错
	 * }
	 * // 然后将其加入到页面调整缓存
	 * error = add_to_page_cache_lru(page, mapping, index, GFP_KERNEL);
	 * if (error) {
	 * // 页面被加入到高速缓存时出错
	 * }
	 */

	// 获取文件映射
	struct address_space *mapping = filp->f_mapping;
	// 获取inode
	struct inode *inode = mapping->host;
	// 预读状态
	struct file_ra_state *ra = &filp->f_ra;
	// 页帧索引
	pgoff_t index;
	// 最后页帧索引
	pgoff_t last_index;
	// 上一个页帧索引
	pgoff_t prev_index;
	/* 页面内的偏移量 */
	unsigned long offset;      /* offset into pagecache page */
	// 上一个偏移量
	unsigned int prev_offset;
	int error;	// 错误码

	// 计算当前偏移对应的页帧索引
	index = *ppos >> PAGE_CACHE_SHIFT;
	// 获取上一个位置的页帧索引
	prev_index = ra->prev_pos >> PAGE_CACHE_SHIFT;
	// 上一个偏移量
	prev_offset = ra->prev_pos & (PAGE_CACHE_SIZE-1);
	// 计算最后的页帧索引
	last_index = (*ppos + desc->count + PAGE_CACHE_SIZE-1) >> PAGE_CACHE_SHIFT;
	// 页面内的偏移量
	offset = *ppos & ~PAGE_CACHE_MASK;

	// 循环处理所有页帧
	for (;;) {
		// 页面指针
		struct page *page;
		// 文件结束的页帧索引
		pgoff_t end_index;
		// 文件大小
		loff_t isize;
		// 读取的字节数和返回值
		unsigned long nr, ret;

		cond_resched();	// 让出CPU
find_page:
		// 查找并获取页面
		page = find_get_page(mapping, index);
		if (!page) {	// 如果页面不存在
			// 同步预读
			page_cache_sync_readahead(mapping,
					ra, filp,
					index, last_index - index);
			// 重新获取页面
			page = find_get_page(mapping, index);
			// 如果还是没有，处理无缓存页面
			if (unlikely(page == NULL))
				goto no_cached_page;
		}
		// 如果页面在读取中
		if (PageReadahead(page)) {
			// 异步预读
			page_cache_async_readahead(mapping,
					ra, filp, page,
					index, last_index - index);
		}
		// 如果页面不是最新的
		if (!PageUptodate(page)) {
			if (inode->i_blkbits == PAGE_CACHE_SHIFT ||
					!mapping->a_ops->is_partially_uptodate)
				// 页面不是最新的，处理它
				goto page_not_up_to_date;
			if (!trylock_page(page))
				goto page_not_up_to_date;
			if (!mapping->a_ops->is_partially_uptodate(page,
								desc, offset))
				goto page_not_up_to_date_locked;
			unlock_page(page);
		}
page_ok:
		/*
		 * i_size must be checked after we know the page is Uptodate.
		 *
		 * Checking i_size after the check allows us to calculate
		 * the correct value for "nr", which means the zero-filled
		 * part of the page is not copied back to userspace (unless
		 * another truncate extends the file - this is desired though).
		 */

		// 以下代码检查文件大小后拷贝页面到用户空间
		
		// 读取文件大小
		isize = i_size_read(inode);
		// 计算结束的页帧索引
		end_index = (isize - 1) >> PAGE_CACHE_SHIFT;
		// 如果索引超过了文件大小
		if (unlikely(!isize || index > end_index)) {
			// 释放页面
			page_cache_release(page);
			goto out;	// 退出
		}

		/* nr is the maximum number of bytes to copy from this page */
		// 计算从这个页面可以复制的最大字节数
		// 默认从整个页面复制
		nr = PAGE_CACHE_SIZE;
		// 如果是最后一页
		if (index == end_index) {
			// 计算剩余的字节数
			nr = ((isize - 1) & ~PAGE_CACHE_MASK) + 1;
			// 如果偏移超过了文件大小
			if (nr <= offset) {
				// 释放页面
				page_cache_release(page);
				// 退出
				goto out;
			}
		}
		nr = nr - offset;	// 调整读取的字节数

		/* If users can be writing to this page using arbitrary
		 * virtual addresses, take care about potential aliasing
		 * before reading the page on the kernel side.
		 */
		// 如果映射可以写，确保页面在读取前是干净的
		if (mapping_writably_mapped(mapping))
			flush_dcache_page(page);

		/*
		 * When a sequential read accesses a page several times,
		 * only mark it as accessed the first time.
		 */
		// 如果是连续读取同一页面，只在第一次标记页面被访问
		if (prev_index != index || offset != prev_offset)
			mark_page_accessed(page);
		prev_index = index;	// 更新上一个索引

		/*
		 * Ok, we have the page, and it's up-to-date, so
		 * now we can copy it to user space...
		 *
		 * The actor routine returns how many bytes were actually used..
		 * NOTE! This may not be the same as how much of a user buffer
		 * we filled up (we may be padding etc), so we can only update
		 * "pos" here (the actor routine has to update the user buffer
		 * pointers and the remaining count).
		 */
		// 执行实际的读取操作
		// 调用传入的actor函数进行读取
		ret = actor(desc, page, offset, nr);
		// 更新偏移
		offset += ret;
		// 更新索引
		index += offset >> PAGE_CACHE_SHIFT;
		// 更新偏移
		offset &= ~PAGE_CACHE_MASK;
		// 更新上一个偏移
		prev_offset = offset;

		page_cache_release(page);	// 释放页面
		if (ret == nr && desc->count)	// 如果还有数据需要读取
			continue;
		goto out;	// 否则退出

page_not_up_to_date:	// 页面不是最新的，需要锁定并读取
		/* Get exclusive access to the page ... */
		// 获取对页面的独占访问
		error = lock_page_killable(page);
		if (unlikely(error))
			goto readpage_error;	// 错误处理

// 页面已锁定但不是最新的
page_not_up_to_date_locked:
		/* Did it get truncated before we got the lock? */
		// 检查页面是否在锁定前被截断
		if (!page->mapping) {
			unlock_page(page);	// 解锁页面
			page_cache_release(page);	// 释放页面
			continue;	// 继续处理下一个页面
		}

		/* Did somebody else fill it already? */
		// 检查其他人是否已经更新了页面
		if (PageUptodate(page)) {
			unlock_page(page);	// 解锁页面
			goto page_ok;	// 页面是最新的
		}

readpage:	// 读取页面
		/* Start the actual read. The read will unlock the page. */
		// 开始实际的读取，读取操作会解锁页面
		// 调用readpage方法
		error = mapping->a_ops->readpage(filp, page);

		if (unlikely(error)) {	// 如果读取出错
			if (error == AOP_TRUNCATED_PAGE) {
				page_cache_release(page);	// 释放页面
				goto find_page;	// 重新查找页面
			}
			goto readpage_error;	// 错误处理
		}

		if (!PageUptodate(page)) {	// 如果页面仍然不是最新的
		// 再次锁定页面
			error = lock_page_killable(page);
			if (unlikely(error))
				// 错误处理
				goto readpage_error;
			// 检查页面是否已更新
			if (!PageUptodate(page)) {
				// 页面已被invalidate_mapping_pages处理
				if (page->mapping == NULL) {
					/*
					 * invalidate_mapping_pages got it
					 */
					unlock_page(page);	// 解锁页面
					page_cache_release(page);	// 释放页面
					goto find_page;	// 重新查找页面
				}
				unlock_page(page);	// 解锁页面
				// 调整预读大小
				shrink_readahead_size_eio(filp, ra);
				// 设置错误码为I/O错误
				error = -EIO;
				goto readpage_error;	// 错误处理
			}
			unlock_page(page);	// 解锁页面
		}

		goto page_ok;	// 页面已更新，继续处理

readpage_error:	// 读取页面出错
		/* UHHUH! A synchronous read error occurred. Report it */
		// 报告同步读取错误
		desc->error = error;	// 设置读取描述符的错误码
		page_cache_release(page);	// 释放页面
		goto out;	// 退出处理

no_cached_page:	// 未缓存页面处理
		/*
		 * Ok, it wasn't cached, so we need to create a new
		 * page..
		 */
		// 页面未缓存，需要创建新页面
		page = page_cache_alloc_cold(mapping);	// 分配新页面
		if (!page) {	// 如果页面分配失败
		// 设置错误为内存不足
			desc->error = -ENOMEM;
			goto out;	// 退出处理
		}
		// 将页面添加到页面缓存
		error = add_to_page_cache_lru(page, mapping,
						index, GFP_KERNEL);
		if (error) {	// 如果添加失败
		// 释放页面
			page_cache_release(page);
			// 如果是因为页面已存在，则重新查找
			if (error == -EEXIST)
				goto find_page;
			desc->error = error;	// 设置错误码
			goto out;	// 退出处理
		}
		goto readpage;	// 读取页面
	}

out:	// 退出处理
	// 更新预读状态的上一个位置
	ra->prev_pos = prev_index;
	// 转换为字节偏移
	ra->prev_pos <<= PAGE_CACHE_SHIFT;
	// 加上偏移量
	ra->prev_pos |= prev_offset;

	// 更新文件位置
	*ppos = ((loff_t)index << PAGE_CACHE_SHIFT) + offset;
	// 标记文件为已访问
	file_accessed(filp);
}

// 从内核映射的页面到用户空间的数据复制操作，首先尝试使用原子操作进行快速复制，
// 如果失败则采用普通映射方式。通过调整缓冲区大小和处理缺页异常，
// 确保数据安全有效地从内核传输到用户程序。
int file_read_actor(read_descriptor_t *desc, struct page *page,
			unsigned long offset, unsigned long size)
{
	char *kaddr;	// 内核临时映射的地址
	// `left` 用于记录未复制的字节数
	unsigned long left, count = desc->count;

	// 调整 size 确保不会超过读描述符所要求的字节数
	if (size > count)
		size = count;

	/*
	 * Faults on the destination of a read are common, so do it before
	 * taking the kmap.
	 */
	/*
	 * 读操作目标页的缺页异常是常见的，所以在执行 kmap 之前先处理。
	 * 如果目标页写入时没有发生缺页异常，即先处理可能的用户空间页错误。
	 */
	if (!fault_in_pages_writeable(desc->arg.buf, size)) {
		// 使用原子操作映射页，KM_USER0 是类型，用于标记临时映射的用途
		kaddr = kmap_atomic(page, KM_USER0);
		// 尝试原子地将数据从内核空间复制到用户空间
		left = __copy_to_user_inatomic(desc->arg.buf,
						kaddr + offset, size);
		// 原子解除映射
		kunmap_atomic(kaddr, KM_USER0);
		// 如果全部复制成功，跳转到成功处理
		if (left == 0)
			goto success;
	}

	/* Do it the slow way */
	/* 慢速路径 */
	kaddr = kmap(page);	// 非原子地映射页
	// 将数据从内核复制到用户空间
	left = __copy_to_user(desc->arg.buf, kaddr + offset, size);
	kunmap(page);	// 解除映射

	if (left) {	// 如果还有未复制的数据
		size -= left;	// 调整成功复制的大小
		// 设置错误码
		desc->error = -EFAULT;
	}
success:
	// 更新描述符状态
	desc->count = count - size;
	// 更新剩余要读的字节数
	desc->written += size;
	// 移动缓冲区指针
	desc->arg.buf += size;
	// 返回这次操作复制的字节数
	return size;
}

/*
 * Performs necessary checks before doing a write
 * @iov:	io vector request
 * @nr_segs:	number of segments in the iovec
 * @count:	number of bytes to write
 * @access_flags: type of access: %VERIFY_READ or %VERIFY_WRITE
 *
 * Adjust number of segments and amount of bytes to write (nr_segs should be
 * properly initialized first). Returns appropriate error code that caller
 * should return or zero in case that write should be allowed.
 */
int generic_segment_checks(const struct iovec *iov,
			unsigned long *nr_segs, size_t *count, int access_flags)
{
	unsigned long   seg;
	size_t cnt = 0;
	for (seg = 0; seg < *nr_segs; seg++) {
		const struct iovec *iv = &iov[seg];

		/*
		 * If any segment has a negative length, or the cumulative
		 * length ever wraps negative then return -EINVAL.
		 */
		cnt += iv->iov_len;
		if (unlikely((ssize_t)(cnt|iv->iov_len) < 0))
			return -EINVAL;
		if (access_ok(access_flags, iv->iov_base, iv->iov_len))
			continue;
		if (seg == 0)
			return -EFAULT;
		*nr_segs = seg;
		cnt -= iv->iov_len;	/* This segment is no good */
		break;
	}
	*count = cnt;
	return 0;
}
EXPORT_SYMBOL(generic_segment_checks);

/**
 * generic_file_aio_read - generic filesystem read routine
 * @iocb:	kernel I/O control block
 * @iov:	io vector request
 * @nr_segs:	number of segments in the iovec
 * @pos:	current file position
 *
 * This is the "read()" routine for all filesystems
 * that can use the page cache directly.
 */
/**
 * 通用文件异步读取 - 所有可以直接使用页面缓存的文件系统的通用文件读取函数
 * @iocb:	内核I/O控制块
 * @iov:	I/O向量请求
 * @nr_segs:	iovec中的段数
 * @pos:	当前文件位置
 *
 * 这是所有可以直接使用页面缓存的文件系统的"read()"函数。
 */
// 通用文件异步读取函数
ssize_t
generic_file_aio_read(struct kiocb *iocb, const struct iovec *iov,
		unsigned long nr_segs, loff_t pos)
{
	// 获取文件结构体
	struct file *filp = iocb->ki_filp;
	ssize_t retval;	// 用于存放返回值
	unsigned long seg;	// 用于循环的索引
	size_t count;		// 总读取字节数
	// 当前文件位置的指针
	loff_t *ppos = &iocb->ki_pos;

	count = 0;	// 当前文件位置的指针
	// 检查和准备iovec结构体
	retval = generic_segment_checks(iov, &nr_segs, &count, VERIFY_WRITE);
	// 如果检查出错，返回错误
	if (retval)
		return retval;

	/* coalesce the iovecs and go direct-to-BIO for O_DIRECT */
	/* 如果是O_DIRECT标志，直接通过BIO进行读取 */
	if (filp->f_flags & O_DIRECT) {
		loff_t size;
		struct address_space *mapping;
		struct inode *inode;

		// 获取地址空间结构体
		mapping = filp->f_mapping;
		// 获取inode结构体
		inode = mapping->host;
		// 如果没有数据需要读取，则直接退出
		if (!count)
			goto out; /* skip atime */
		// 读取文件大小
		size = i_size_read(inode);
		// 如果当前位置小于文件大小
		if (pos < size) {
			retval = filemap_write_and_wait_range(mapping, pos,
					pos + iov_length(iov, nr_segs) - 1);
			// 如果没有错误
			if (!retval) {
					// 执行直接IO操作
				retval = mapping->a_ops->direct_IO(READ, iocb,
							iov, pos, nr_segs);
			}
			if (retval > 0)
				// 更新位置
				*ppos = pos + retval;
			if (retval) {
				// 更新文件访问时间
				file_accessed(filp);
				goto out;
			}
		}
	}

	// 循环处理每个iovec
	for (seg = 0; seg < nr_segs; seg++) {
		read_descriptor_t desc;

		desc.written = 0;
		desc.arg.buf = iov[seg].iov_base;
		desc.count = iov[seg].iov_len;
		// 如果当前段没有数据，则继续下一段
		if (desc.count == 0)
			continue;
		desc.error = 0;
		// 执行通用文件读取
		do_generic_file_read(filp, ppos, &desc, file_read_actor);
		retval += desc.written;	// 累加写入的字节数
		if (desc.error) {	// 如果有错误
			// 使用错误码作为返回值
			retval = retval ?: desc.error;
			break;
		}
		if (desc.count > 0)
			break;
	}
out:
	return retval;	// 返回读取结果
}
EXPORT_SYMBOL(generic_file_aio_read);

static ssize_t
do_readahead(struct address_space *mapping, struct file *filp,
	     pgoff_t index, unsigned long nr)
{
	if (!mapping || !mapping->a_ops || !mapping->a_ops->readpage)
		return -EINVAL;

	force_page_cache_readahead(mapping, filp, index, nr);
	return 0;
}

SYSCALL_DEFINE(readahead)(int fd, loff_t offset, size_t count)
{
	ssize_t ret;
	struct file *file;

	ret = -EBADF;
	file = fget(fd);
	if (file) {
		if (file->f_mode & FMODE_READ) {
			struct address_space *mapping = file->f_mapping;
			pgoff_t start = offset >> PAGE_CACHE_SHIFT;
			pgoff_t end = (offset + count - 1) >> PAGE_CACHE_SHIFT;
			unsigned long len = end - start + 1;
			ret = do_readahead(mapping, file, start, len);
		}
		fput(file);
	}
	return ret;
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_readahead(long fd, loff_t offset, long count)
{
	return SYSC_readahead((int) fd, offset, (size_t) count);
}
SYSCALL_ALIAS(sys_readahead, SyS_readahead);
#endif

#ifdef CONFIG_MMU
/**
 * page_cache_read - adds requested page to the page cache if not already there
 * @file:	file to read
 * @offset:	page index
 *
 * This adds the requested page to the page cache if it isn't already there,
 * and schedules an I/O to read in its contents from disk.
 */
static int page_cache_read(struct file *file, pgoff_t offset)
{
	struct address_space *mapping = file->f_mapping;
	struct page *page; 
	int ret;

	do {
		page = page_cache_alloc_cold(mapping);
		if (!page)
			return -ENOMEM;

		ret = add_to_page_cache_lru(page, mapping, offset, GFP_KERNEL);
		if (ret == 0)
			ret = mapping->a_ops->readpage(file, page);
		else if (ret == -EEXIST)
			ret = 0; /* losing race to add is OK */

		page_cache_release(page);

	} while (ret == AOP_TRUNCATED_PAGE);
		
	return ret;
}

#define MMAP_LOTSAMISS  (100)

/*
 * Synchronous readahead happens when we don't even find
 * a page in the page cache at all.
 */
static void do_sync_mmap_readahead(struct vm_area_struct *vma,
				   struct file_ra_state *ra,
				   struct file *file,
				   pgoff_t offset)
{
	unsigned long ra_pages;
	struct address_space *mapping = file->f_mapping;

	/* If we don't want any read-ahead, don't bother */
	if (VM_RandomReadHint(vma))
		return;

	if (VM_SequentialReadHint(vma) ||
			offset - 1 == (ra->prev_pos >> PAGE_CACHE_SHIFT)) {
		page_cache_sync_readahead(mapping, ra, file, offset,
					  ra->ra_pages);
		return;
	}

	if (ra->mmap_miss < INT_MAX)
		ra->mmap_miss++;

	/*
	 * Do we miss much more than hit in this file? If so,
	 * stop bothering with read-ahead. It will only hurt.
	 */
	if (ra->mmap_miss > MMAP_LOTSAMISS)
		return;

	/*
	 * mmap read-around
	 */
	ra_pages = max_sane_readahead(ra->ra_pages);
	if (ra_pages) {
		ra->start = max_t(long, 0, offset - ra_pages/2);
		ra->size = ra_pages;
		ra->async_size = 0;
		ra_submit(ra, mapping, file);
	}
}

/*
 * Asynchronous readahead happens when we find the page and PG_readahead,
 * so we want to possibly extend the readahead further..
 */
static void do_async_mmap_readahead(struct vm_area_struct *vma,
				    struct file_ra_state *ra,
				    struct file *file,
				    struct page *page,
				    pgoff_t offset)
{
	struct address_space *mapping = file->f_mapping;

	/* If we don't want any read-ahead, don't bother */
	if (VM_RandomReadHint(vma))
		return;
	if (ra->mmap_miss > 0)
		ra->mmap_miss--;
	if (PageReadahead(page))
		page_cache_async_readahead(mapping, ra, file,
					   page, offset, ra->ra_pages);
}

/**
 * filemap_fault - read in file data for page fault handling
 * @vma:	vma in which the fault was taken
 * @vmf:	struct vm_fault containing details of the fault
 *
 * filemap_fault() is invoked via the vma operations vector for a
 * mapped memory region to read in file data during a page fault.
 *
 * The goto's are kind of ugly, but this streamlines the normal case of having
 * it in the page cache, and handles the special cases reasonably without
 * having a lot of duplicated code.
 */
int filemap_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int error;
	struct file *file = vma->vm_file;
	struct address_space *mapping = file->f_mapping;
	struct file_ra_state *ra = &file->f_ra;
	struct inode *inode = mapping->host;
	pgoff_t offset = vmf->pgoff;
	struct page *page;
	pgoff_t size;
	int ret = 0;

	size = (i_size_read(inode) + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	if (offset >= size)
		return VM_FAULT_SIGBUS;

	/*
	 * Do we have something in the page cache already?
	 */
	page = find_get_page(mapping, offset);
	if (likely(page)) {
		/*
		 * We found the page, so try async readahead before
		 * waiting for the lock.
		 */
		do_async_mmap_readahead(vma, ra, file, page, offset);
		lock_page(page);

		/* Did it get truncated? */
		if (unlikely(page->mapping != mapping)) {
			unlock_page(page);
			put_page(page);
			goto no_cached_page;
		}
	} else {
		/* No page in the page cache at all */
		do_sync_mmap_readahead(vma, ra, file, offset);
		count_vm_event(PGMAJFAULT);
		ret = VM_FAULT_MAJOR;
retry_find:
		page = find_lock_page(mapping, offset);
		if (!page)
			goto no_cached_page;
	}

	/*
	 * We have a locked page in the page cache, now we need to check
	 * that it's up-to-date. If not, it is going to be due to an error.
	 */
	if (unlikely(!PageUptodate(page)))
		goto page_not_uptodate;

	/*
	 * Found the page and have a reference on it.
	 * We must recheck i_size under page lock.
	 */
	size = (i_size_read(inode) + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	if (unlikely(offset >= size)) {
		unlock_page(page);
		page_cache_release(page);
		return VM_FAULT_SIGBUS;
	}

	ra->prev_pos = (loff_t)offset << PAGE_CACHE_SHIFT;
	vmf->page = page;
	return ret | VM_FAULT_LOCKED;

no_cached_page:
	/*
	 * We're only likely to ever get here if MADV_RANDOM is in
	 * effect.
	 */
	error = page_cache_read(file, offset);

	/*
	 * The page we want has now been added to the page cache.
	 * In the unlikely event that someone removed it in the
	 * meantime, we'll just come back here and read it again.
	 */
	if (error >= 0)
		goto retry_find;

	/*
	 * An error return from page_cache_read can result if the
	 * system is low on memory, or a problem occurs while trying
	 * to schedule I/O.
	 */
	if (error == -ENOMEM)
		return VM_FAULT_OOM;
	return VM_FAULT_SIGBUS;

page_not_uptodate:
	/*
	 * Umm, take care of errors if the page isn't up-to-date.
	 * Try to re-read it _once_. We do this synchronously,
	 * because there really aren't any performance issues here
	 * and we need to check for errors.
	 */
	ClearPageError(page);
	error = mapping->a_ops->readpage(file, page);
	if (!error) {
		wait_on_page_locked(page);
		if (!PageUptodate(page))
			error = -EIO;
	}
	page_cache_release(page);

	if (!error || error == AOP_TRUNCATED_PAGE)
		goto retry_find;

	/* Things didn't work out. Return zero to tell the mm layer so. */
	shrink_readahead_size_eio(file, ra);
	return VM_FAULT_SIGBUS;
}
EXPORT_SYMBOL(filemap_fault);

const struct vm_operations_struct generic_file_vm_ops = {
	.fault		= filemap_fault,
};

/* This is used for a general mmap of a disk file */

int generic_file_mmap(struct file * file, struct vm_area_struct * vma)
{
	struct address_space *mapping = file->f_mapping;

	if (!mapping->a_ops->readpage)
		return -ENOEXEC;
	file_accessed(file);
	vma->vm_ops = &generic_file_vm_ops;
	vma->vm_flags |= VM_CAN_NONLINEAR;
	return 0;
}

/*
 * This is for filesystems which do not implement ->writepage.
 */
int generic_file_readonly_mmap(struct file *file, struct vm_area_struct *vma)
{
	if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_MAYWRITE))
		return -EINVAL;
	return generic_file_mmap(file, vma);
}
#else
int generic_file_mmap(struct file * file, struct vm_area_struct * vma)
{
	return -ENOSYS;
}
int generic_file_readonly_mmap(struct file * file, struct vm_area_struct * vma)
{
	return -ENOSYS;
}
#endif /* CONFIG_MMU */

EXPORT_SYMBOL(generic_file_mmap);
EXPORT_SYMBOL(generic_file_readonly_mmap);

static struct page *__read_cache_page(struct address_space *mapping,
				pgoff_t index,
				int (*filler)(void *,struct page*),
				void *data,
				gfp_t gfp)
{
	struct page *page;
	int err;
repeat:
	page = find_get_page(mapping, index);
	if (!page) {
		page = __page_cache_alloc(gfp | __GFP_COLD);
		if (!page)
			return ERR_PTR(-ENOMEM);
		err = add_to_page_cache_lru(page, mapping, index, GFP_KERNEL);
		if (unlikely(err)) {
			page_cache_release(page);
			if (err == -EEXIST)
				goto repeat;
			/* Presumably ENOMEM for radix tree node */
			return ERR_PTR(err);
		}
		err = filler(data, page);
		if (err < 0) {
			page_cache_release(page);
			page = ERR_PTR(err);
		}
	}
	return page;
}

static struct page *do_read_cache_page(struct address_space *mapping,
				pgoff_t index,
				int (*filler)(void *,struct page*),
				void *data,
				gfp_t gfp)

{
	struct page *page;
	int err;

retry:
	page = __read_cache_page(mapping, index, filler, data, gfp);
	if (IS_ERR(page))
		return page;
	if (PageUptodate(page))
		goto out;

	lock_page(page);
	if (!page->mapping) {
		unlock_page(page);
		page_cache_release(page);
		goto retry;
	}
	if (PageUptodate(page)) {
		unlock_page(page);
		goto out;
	}
	err = filler(data, page);
	if (err < 0) {
		page_cache_release(page);
		return ERR_PTR(err);
	}
out:
	mark_page_accessed(page);
	return page;
}

/**
 * read_cache_page_async - read into page cache, fill it if needed
 * @mapping:	the page's address_space
 * @index:	the page index
 * @filler:	function to perform the read
 * @data:	destination for read data
 *
 * Same as read_cache_page, but don't wait for page to become unlocked
 * after submitting it to the filler.
 *
 * Read into the page cache. If a page already exists, and PageUptodate() is
 * not set, try to fill the page but don't wait for it to become unlocked.
 *
 * If the page does not get brought uptodate, return -EIO.
 */
struct page *read_cache_page_async(struct address_space *mapping,
				pgoff_t index,
				int (*filler)(void *,struct page*),
				void *data)
{
	return do_read_cache_page(mapping, index, filler, data, mapping_gfp_mask(mapping));
}
EXPORT_SYMBOL(read_cache_page_async);

static struct page *wait_on_page_read(struct page *page)
{
	if (!IS_ERR(page)) {
		wait_on_page_locked(page);
		if (!PageUptodate(page)) {
			page_cache_release(page);
			page = ERR_PTR(-EIO);
		}
	}
	return page;
}

/**
 * read_cache_page_gfp - read into page cache, using specified page allocation flags.
 * @mapping:	the page's address_space
 * @index:	the page index
 * @gfp:	the page allocator flags to use if allocating
 *
 * This is the same as "read_mapping_page(mapping, index, NULL)", but with
 * any new page allocations done using the specified allocation flags. Note
 * that the Radix tree operations will still use GFP_KERNEL, so you can't
 * expect to do this atomically or anything like that - but you can pass in
 * other page requirements.
 *
 * If the page does not get brought uptodate, return -EIO.
 */
struct page *read_cache_page_gfp(struct address_space *mapping,
				pgoff_t index,
				gfp_t gfp)
{
	filler_t *filler = (filler_t *)mapping->a_ops->readpage;

	return wait_on_page_read(do_read_cache_page(mapping, index, filler, NULL, gfp));
}
EXPORT_SYMBOL(read_cache_page_gfp);

/**
 * read_cache_page - read into page cache, fill it if needed
 * @mapping:	the page's address_space
 * @index:	the page index
 * @filler:	function to perform the read
 * @data:	destination for read data
 *
 * Read into the page cache. If a page already exists, and PageUptodate() is
 * not set, try to fill the page then wait for it to become unlocked.
 *
 * If the page does not get brought uptodate, return -EIO.
 */
struct page *read_cache_page(struct address_space *mapping,
				pgoff_t index,
				int (*filler)(void *,struct page*),
				void *data)
{
	return wait_on_page_read(read_cache_page_async(mapping, index, filler, data));
}
EXPORT_SYMBOL(read_cache_page);

/*
 * The logic we want is
 *
 *	if suid or (sgid and xgrp)
 *		remove privs
 */
int should_remove_suid(struct dentry *dentry)
{
	mode_t mode = dentry->d_inode->i_mode;
	int kill = 0;

	/* suid always must be killed */
	if (unlikely(mode & S_ISUID))
		kill = ATTR_KILL_SUID;

	/*
	 * sgid without any exec bits is just a mandatory locking mark; leave
	 * it alone.  If some exec bits are set, it's a real sgid; kill it.
	 */
	if (unlikely((mode & S_ISGID) && (mode & S_IXGRP)))
		kill |= ATTR_KILL_SGID;

	if (unlikely(kill && !capable(CAP_FSETID) && S_ISREG(mode)))
		return kill;

	return 0;
}
EXPORT_SYMBOL(should_remove_suid);

static int __remove_suid(struct dentry *dentry, int kill)
{
	struct iattr newattrs;

	newattrs.ia_valid = ATTR_FORCE | kill;
	return notify_change(dentry, &newattrs);
}

int file_remove_suid(struct file *file)
{
	struct dentry *dentry = file->f_path.dentry;
	int killsuid = should_remove_suid(dentry);
	int killpriv = security_inode_need_killpriv(dentry);
	int error = 0;

	if (killpriv < 0)
		return killpriv;
	if (killpriv)
		error = security_inode_killpriv(dentry);
	if (!error && killsuid)
		error = __remove_suid(dentry, killsuid);

	return error;
}
EXPORT_SYMBOL(file_remove_suid);

static size_t __iovec_copy_from_user_inatomic(char *vaddr,
			const struct iovec *iov, size_t base, size_t bytes)
{
	// 已复制和剩余未复制的字节数
	size_t copied = 0, left = 0;

	while (bytes) {	// 当还有字节需要复制时
		// 用户空间的数据源地址
		char __user *buf = iov->iov_base + base;
		// 计算这次需要复制的字节数
		int copy = min(bytes, iov->iov_len - base);

		// 基础偏移设置为0，因为在第一次循环后应从iov的开始复制
		base = 0;
		// 从用户空间复制数据到内核空间
		left = __copy_from_user_inatomic(vaddr, buf, copy);
		// 更新已复制的字节数
		copied += copy;
		// 减少剩余需要复制的字节数
		bytes -= copy;
		// 移动目标内存地址
		vaddr += copy;
		// 移动到下一个iovec元素
		iov++;

		// 如果剩余未复制字节数不为零，即复制过程中出错
		if (unlikely(left))
			break;
	}
	// 返回成功复制的总字节数
	return copied - left;
}

/*
 * Copy as much as we can into the page and return the number of bytes which
 * were successfully copied.  If a fault is encountered then return the number of
 * bytes which were copied.
 */
/*
 * 复制尽可能多的数据到页面，并返回成功复制的字节数。如果遇到错误，则返回已复制的字节数。
 */
size_t iov_iter_copy_from_user_atomic(struct page *page,
		struct iov_iter *i, unsigned long offset, size_t bytes)
{
	char *kaddr;		// 内核空间的地址
	size_t copied;	// 已复制的字节数

	BUG_ON(!in_atomic());	// 确认当前在原子上下文中
	// 将页面映射到内核地址空间
	kaddr = kmap_atomic(page, KM_USER0);
	// 如果只有一个段
	if (likely(i->nr_segs == 1)) {
		int left;
		// 用户空间的数据源地址
		char __user *buf = i->iov->iov_base + i->iov_offset;
		// 从用户空间复制数据到内核空间
		left = __copy_from_user_inatomic(kaddr + offset, buf, bytes);
		// 计算成功复制的字节数
		copied = bytes - left;
	} else {	// 如果有多个段
		// 使用专门的函数处理多段复制
		copied = __iovec_copy_from_user_inatomic(kaddr + offset,
						i->iov, i->iov_offset, bytes);
	}
	kunmap_atomic(kaddr, KM_USER0);	// 取消映射

	return copied;	// 返回成功复制的字节数
}
EXPORT_SYMBOL(iov_iter_copy_from_user_atomic);

/*
 * This has the same sideeffects and return value as
 * iov_iter_copy_from_user_atomic().
 * The difference is that it attempts to resolve faults.
 * Page must not be locked.
 */
size_t iov_iter_copy_from_user(struct page *page,
		struct iov_iter *i, unsigned long offset, size_t bytes)
{
	char *kaddr;
	size_t copied;

	kaddr = kmap(page);
	if (likely(i->nr_segs == 1)) {
		int left;
		char __user *buf = i->iov->iov_base + i->iov_offset;
		left = __copy_from_user(kaddr + offset, buf, bytes);
		copied = bytes - left;
	} else {
		copied = __iovec_copy_from_user_inatomic(kaddr + offset,
						i->iov, i->iov_offset, bytes);
	}
	kunmap(page);
	return copied;
}
EXPORT_SYMBOL(iov_iter_copy_from_user);

void iov_iter_advance(struct iov_iter *i, size_t bytes)
{
	BUG_ON(i->count < bytes);

	if (likely(i->nr_segs == 1)) {
		i->iov_offset += bytes;
		i->count -= bytes;
	} else {
		const struct iovec *iov = i->iov;
		size_t base = i->iov_offset;

		/*
		 * The !iov->iov_len check ensures we skip over unlikely
		 * zero-length segments (without overruning the iovec).
		 */
		while (bytes || unlikely(i->count && !iov->iov_len)) {
			int copy;

			copy = min(bytes, iov->iov_len - base);
			BUG_ON(!i->count || i->count < copy);
			i->count -= copy;
			bytes -= copy;
			base += copy;
			if (iov->iov_len == base) {
				iov++;
				base = 0;
			}
		}
		i->iov = iov;
		i->iov_offset = base;
	}
}
EXPORT_SYMBOL(iov_iter_advance);

/*
 * Fault in the first iovec of the given iov_iter, to a maximum length
 * of bytes. Returns 0 on success, or non-zero if the memory could not be
 * accessed (ie. because it is an invalid address).
 *
 * writev-intensive code may want this to prefault several iovecs -- that
 * would be possible (callers must not rely on the fact that _only_ the
 * first iovec will be faulted with the current implementation).
 */
int iov_iter_fault_in_readable(struct iov_iter *i, size_t bytes)
{
	char __user *buf = i->iov->iov_base + i->iov_offset;
	bytes = min(bytes, i->iov->iov_len - i->iov_offset);
	return fault_in_pages_readable(buf, bytes);
}
EXPORT_SYMBOL(iov_iter_fault_in_readable);

/*
 * Return the count of just the current iov_iter segment.
 */
size_t iov_iter_single_seg_count(struct iov_iter *i)
{
	const struct iovec *iov = i->iov;
	if (i->nr_segs == 1)
		return i->count;
	else
		return min(i->count, iov->iov_len - i->iov_offset);
}
EXPORT_SYMBOL(iov_iter_single_seg_count);

/*
 * Performs necessary checks before doing a write
 *
 * Can adjust writing position or amount of bytes to write.
 * Returns appropriate error code that caller should return or
 * zero in case that write should be allowed.
 */
inline int generic_write_checks(struct file *file, loff_t *pos, size_t *count, int isblk)
{
	struct inode *inode = file->f_mapping->host;
	unsigned long limit = rlimit(RLIMIT_FSIZE);

        if (unlikely(*pos < 0))
                return -EINVAL;

	if (!isblk) {
		/* FIXME: this is for backwards compatibility with 2.4 */
		if (file->f_flags & O_APPEND)
                        *pos = i_size_read(inode);

		if (limit != RLIM_INFINITY) {
			if (*pos >= limit) {
				send_sig(SIGXFSZ, current, 0);
				return -EFBIG;
			}
			if (*count > limit - (typeof(limit))*pos) {
				*count = limit - (typeof(limit))*pos;
			}
		}
	}

	/*
	 * LFS rule
	 */
	if (unlikely(*pos + *count > MAX_NON_LFS &&
				!(file->f_flags & O_LARGEFILE))) {
		if (*pos >= MAX_NON_LFS) {
			return -EFBIG;
		}
		if (*count > MAX_NON_LFS - (unsigned long)*pos) {
			*count = MAX_NON_LFS - (unsigned long)*pos;
		}
	}

	/*
	 * Are we about to exceed the fs block limit ?
	 *
	 * If we have written data it becomes a short write.  If we have
	 * exceeded without writing data we send a signal and return EFBIG.
	 * Linus frestrict idea will clean these up nicely..
	 */
	if (likely(!isblk)) {
		if (unlikely(*pos >= inode->i_sb->s_maxbytes)) {
			if (*count || *pos > inode->i_sb->s_maxbytes) {
				return -EFBIG;
			}
			/* zero-length writes at ->s_maxbytes are OK */
		}

		if (unlikely(*pos + *count > inode->i_sb->s_maxbytes))
			*count = inode->i_sb->s_maxbytes - *pos;
	} else {
#ifdef CONFIG_BLOCK
		loff_t isize;
		if (bdev_read_only(I_BDEV(inode)))
			return -EPERM;
		isize = i_size_read(inode);
		if (*pos >= isize) {
			if (*count || *pos > isize)
				return -ENOSPC;
		}

		if (*pos + *count > isize)
			*count = isize - *pos;
#else
		return -EPERM;
#endif
	}
	return 0;
}
EXPORT_SYMBOL(generic_write_checks);

int pagecache_write_begin(struct file *file, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned flags,
				struct page **pagep, void **fsdata)
{
	const struct address_space_operations *aops = mapping->a_ops;

	return aops->write_begin(file, mapping, pos, len, flags,
							pagep, fsdata);
}
EXPORT_SYMBOL(pagecache_write_begin);

int pagecache_write_end(struct file *file, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned copied,
				struct page *page, void *fsdata)
{
	const struct address_space_operations *aops = mapping->a_ops;

	mark_page_accessed(page);
	return aops->write_end(file, mapping, pos, len, copied, page, fsdata);
}
EXPORT_SYMBOL(pagecache_write_end);

ssize_t
generic_file_direct_write(struct kiocb *iocb, const struct iovec *iov,
		unsigned long *nr_segs, loff_t pos, loff_t *ppos,
		size_t count, size_t ocount)
{
	struct file	*file = iocb->ki_filp;
	struct address_space *mapping = file->f_mapping;
	struct inode	*inode = mapping->host;
	ssize_t		written;
	size_t		write_len;
	pgoff_t		end;

	// 如果请求写入的长度小于原始长度，调整iov长度。
	if (count != ocount)
		*nr_segs = iov_shorten((struct iovec *)iov, *nr_segs, count);

	// 计算实际写入的长度。
	write_len = iov_length(iov, *nr_segs);
	end = (pos + write_len - 1) >> PAGE_CACHE_SHIFT;

	// 执行写入，等待文件映射区间中所有已有的写入完成。
	written = filemap_write_and_wait_range(mapping, pos, pos + write_len - 1);
	if (written)
		goto out;

	/*
	 * After a write we want buffered reads to be sure to go to disk to get
	 * the new data.  We invalidate clean cached page from the region we're
	 * about to write.  We do this *before* the write so that we can return
	 * without clobbering -EIOCBQUEUED from ->direct_IO().
	 */
	// 写入前，使缓存中的干净页无效，确保读取到的是新数据。
	if (mapping->nrpages) {
		written = invalidate_inode_pages2_range(mapping,
					pos >> PAGE_CACHE_SHIFT, end);
		/*
		 * If a page can not be invalidated, return 0 to fall back
		 * to buffered write.
		 */
		// 如果页无法无效化，返回0退回到缓冲写。
		if (written) {
			if (written == -EBUSY)
				return 0;
			goto out;
		}
	}

	// 执行直接I/O操作。
	written = mapping->a_ops->direct_IO(WRITE, iocb, iov, pos, *nr_segs);

	/*
	 * Finally, try again to invalidate clean pages which might have been
	 * cached by non-direct readahead, or faulted in by get_user_pages()
	 * if the source of the write was an mmap'ed region of the file
	 * we're writing.  Either one is a pretty crazy thing to do,
	 * so we don't support it 100%.  If this invalidation
	 * fails, tough, the write still worked...
	 */
	// 写入后，再次尝试使缓存中的干净页无效。
	if (mapping->nrpages) {
		invalidate_inode_pages2_range(mapping,
					      pos >> PAGE_CACHE_SHIFT, end);
	}

	// 如果写入成功，更新文件大小。
	if (written > 0) {
		loff_t end = pos + written;
		if (end > i_size_read(inode) && !S_ISBLK(inode->i_mode)) {
			i_size_write(inode,  end);
			mark_inode_dirty(inode);
		}
		*ppos = end;
	}
out:
	return written;
}
EXPORT_SYMBOL(generic_file_direct_write);

/*
 * Find or create a page at the given pagecache position. Return the locked
 * page. This function is specifically for buffered writes.
 */
struct page *grab_cache_page_write_begin(struct address_space *mapping,
					pgoff_t index, unsigned flags)
{
	int status;
	struct page *page;
	gfp_t gfp_notmask = 0;
	if (flags & AOP_FLAG_NOFS)
		gfp_notmask = __GFP_FS;
repeat:
	page = find_lock_page(mapping, index);
	if (likely(page))
		return page;

	page = __page_cache_alloc(mapping_gfp_mask(mapping) & ~gfp_notmask);
	if (!page)
		return NULL;
	status = add_to_page_cache_lru(page, mapping, index,
						GFP_KERNEL & ~gfp_notmask);
	if (unlikely(status)) {
		page_cache_release(page);
		if (status == -EEXIST)
			goto repeat;
		return NULL;
	}
	return page;
}
EXPORT_SYMBOL(grab_cache_page_write_begin);

// 执行文件写入操作。它处理了从用户空间到文件页缓存的数据拷贡，并且处理了相关的页缓存管理和脏页的平衡。
static ssize_t generic_perform_write(struct file *file,
				struct iov_iter *i, loff_t pos)
{
	// 获取文件的地址空间
	struct address_space *mapping = file->f_mapping;
	// 地址空间操作集
	const struct address_space_operations *a_ops = mapping->a_ops;
	long status = 0;	// 状态变量，用于记录操作的结果
	ssize_t written = 0;	// 已写入的字节数
	unsigned int flags = 0;	// 标志位

	/*
	 * Copies from kernel address space cannot fail (NFSD is a big user).
	 */
	// 如果当前上下文是内核数据段，则设置不可中断标志
	if (segment_eq(get_fs(), KERNEL_DS))
		flags |= AOP_FLAG_UNINTERRUPTIBLE;

	do {
		// 页面指针
		struct page *page;
		// 当前页在页缓存中的索引
		pgoff_t index;		/* Pagecache index for current page */
		// 页内偏移
		unsigned long offset;	/* Offset into pagecache page */
		// 要写入的字节数
		unsigned long bytes;	/* Bytes to write to page */
		// 实际从用户空间拷贡的字节数
		size_t copied;		/* Bytes copied from user */
		// 文件系统相关数据，由write_begin返回
		void *fsdata;

		// 计算页内偏移
		offset = (pos & (PAGE_CACHE_SIZE - 1));
		// 计算页索引
		index = pos >> PAGE_CACHE_SHIFT;
		// 计算此次操作的最大字节数
		bytes = min_t(unsigned long, PAGE_CACHE_SIZE - offset,
						iov_iter_count(i));

again:

		/*
		 * Bring in the user page that we will copy from _first_.
		 * Otherwise there's a nasty deadlock on copying from the
		 * same page as we're writing to, without it being marked
		 * up-to-date.
		 *
		 * Not only is this an optimisation, but it is also required
		 * to check that the address is actually valid, when atomic
		 * usercopies are used, below.
		 */
		// 检查是否有足够的用户空间可以读取
		if (unlikely(iov_iter_fault_in_readable(i, bytes))) {
			status = -EFAULT;	// 地址错误
			break;
		}

		// 开始写操作之前的准备，可能包括锁定页等
		status = a_ops->write_begin(file, mapping, pos, bytes, flags,
						&page, &fsdata);
		if (unlikely(status))
			break;

		// 如果地址空间被映射为可写，则刷新页面缓存
		if (mapping_writably_mapped(mapping))
			flush_dcache_page(page);

		pagefault_disable();	// 禁用页错误
		// 从用户空间拷贡数据
		copied = iov_iter_copy_from_user_atomic(page, i, offset, bytes);
		pagefault_enable();	// 启用页错误
		flush_dcache_page(page);	// 刷新页面缓存

		mark_page_accessed(page);	// 标记页面为已访问
		// 完成写操作，解锁页等
		status = a_ops->write_end(file, mapping, pos, bytes, copied,
						page, fsdata);
		if (unlikely(status < 0))
			break;
		copied = status;

		// 条件调度，允许其他进程运行
		cond_resched();

		// 推进迭代器
		iov_iter_advance(i, copied);
		if (unlikely(copied == 0)) {
			/*
			 * If we were unable to copy any data at all, we must
			 * fall back to a single segment length write.
			 *
			 * If we didn't fallback here, we could livelock
			 * because not all segments in the iov can be copied at
			 * once without a pagefault.
			 */
			bytes = min_t(unsigned long, PAGE_CACHE_SIZE - offset,
						iov_iter_single_seg_count(i));
			// 如果未拷贡任何数据，尝试更小的单片段写入
			goto again;
		}
		pos += copied;	 // 更新位置
		written += copied;	// 更新写入字节数

		// 平衡脏页，按需写入
		balance_dirty_pages_ratelimited(mapping);

		// 如果还有数据未写入，则继续循环
	} while (iov_iter_count(i));

	// 返回写入的字节数或错误状态
	return written ? written : status;
}

ssize_t
generic_file_buffered_write(struct kiocb *iocb, const struct iovec *iov,
		unsigned long nr_segs, loff_t pos, loff_t *ppos,
		size_t count, ssize_t written)
{
	// 从IO控制块获取文件指针
	struct file *file = iocb->ki_filp;
	// 用来存储写操作的返回状态
	ssize_t status;
	// iov迭代器，用于处理多段IO
	struct iov_iter i;

	// 初始化iov迭代器，设置其起始位置和长度
	iov_iter_init(&i, iov, nr_segs, count, written);
	// 执行缓冲写操作
	status = generic_perform_write(file, &i, pos);

	// 检查写操作的状态，如果成功，则更新已写入的总字节数和文件位置指针
	if (likely(status >= 0)) {
		written += status;	// 累加已写入字节数
		*ppos = pos + status;	// 更新文件位置指针
  	}
	
	// 返回已写入的字节数或错误状态
	return written ? written : status;
}
EXPORT_SYMBOL(generic_file_buffered_write);

/**
 * __generic_file_aio_write - write data to a file
 * @iocb:	IO state structure (file, offset, etc.)
 * @iov:	vector with data to write
 * @nr_segs:	number of segments in the vector
 * @ppos:	position where to write
 *
 * This function does all the work needed for actually writing data to a
 * file. It does all basic checks, removes SUID from the file, updates
 * modification times and calls proper subroutines depending on whether we
 * do direct IO or a standard buffered write.
 *
 * It expects i_mutex to be grabbed unless we work on a block device or similar
 * object which does not need locking at all.
 *
 * This function does *not* take care of syncing data in case of O_SYNC write.
 * A caller has to handle it. This is mainly due to the fact that we want to
 * avoid syncing under i_mutex.
 */
/**
 * __generic_file_aio_write - 实际将数据写入文件的函数
 * @iocb: 描述IO状态的结构体（包含文件、偏移等信息）
 * @iov: 包含要写入数据的向量
 * @nr_segs: 向量中段的数量
 * @ppos: 要写入的位置
 *
 * 该函数完成将数据实际写入文件所需的所有工作。它进行所有基本检查，移除文件的SUID，更新修改时间，
 * 并根据是否执行直接IO或标准缓冲写入调用相应的子程序。
 *
 * 除非我们操作的是块设备或类似不需要锁定的对象，否则期望在调用此函数之前已获取i_mutex。
 *
 * 该函数不负责同步数据以处理O_SYNC写入。调用者需要处理它。这主要是因为我们想避免在i_mutex下同步。
 */
ssize_t __generic_file_aio_write(struct kiocb *iocb, const struct iovec *iov,
				 unsigned long nr_segs, loff_t *ppos)
{
	struct file *file = iocb->ki_filp;
	struct address_space * mapping = file->f_mapping;
	/* 原始计数 */
	size_t ocount;		/* original count */
	/* 文件限制检查后的计数 */
	size_t count;		/* after file limit checks */
	struct inode 	*inode = mapping->host;
	loff_t		pos;
	ssize_t		written;
	ssize_t		err;

	// 对iov进行基本的安全性检查，确认读取操作是安全的
	ocount = 0;
	err = generic_segment_checks(iov, &nr_segs, &ocount, VERIFY_READ);
	if (err)
		return err;

	count = ocount;
	pos = *ppos;

	// 检查文件系统是否冻结
	vfs_check_frozen(inode->i_sb, SB_FREEZE_WRITE);

	/* We can write back this queue in page reclaim */
	// 关联当前任务的后备设备信息
	current->backing_dev_info = mapping->backing_dev_info;
	written = 0;

	// 检查写入操作是否合法（文件大小、权限等）
	err = generic_write_checks(file, &pos, &count, S_ISBLK(inode->i_mode));
	if (err)
		goto out;

	// 如果没有什么要写的，直接退出
	if (count == 0)
		goto out;

	// 移除文件的set-user-ID和set-group-ID位
	err = file_remove_suid(file);
	if (err)
		goto out;

	// 更新文件的访问时间和修改时间
	file_update_time(file);

	/* coalesce the iovecs and go direct-to-BIO for O_DIRECT */
	// 如果文件以O_DIRECT标志打开，使用直接I/O
	if (unlikely(file->f_flags & O_DIRECT)) {
		loff_t endbyte;
		ssize_t written_buffered;

		// 执行直接写入
		written = generic_file_direct_write(iocb, iov, &nr_segs, pos,
							ppos, count, ocount);
		if (written < 0 || written == count)
			goto out;
		/*
		 * direct-io write to a hole: fall through to buffered I/O
		 * for completing the rest of the request.
		 */
		// 如果直接I/O只完成了部分写入，则使用缓冲写入完成剩余部分
		pos += written;
		count -= written;
		written_buffered = generic_file_buffered_write(iocb, iov,
						nr_segs, pos, ppos, count,
						written);
		/*
		 * If generic_file_buffered_write() retuned a synchronous error
		 * then we want to return the number of bytes which were
		 * direct-written, or the error code if that was zero.  Note
		 * that this differs from normal direct-io semantics, which
		 * will return -EFOO even if some bytes were written.
		 */
		// 如果缓冲写入发生错误，则处理错误
		if (written_buffered < 0) {
			err = written_buffered;
			goto out;
		}

		/*
		 * We need to ensure that the page cache pages are written to
		 * disk and invalidated to preserve the expected O_DIRECT
		 * semantics.
		 */
		// 确保缓存中的页被写入磁盘并且失效
		endbyte = pos + written_buffered - written - 1;
		err = filemap_write_and_wait_range(file->f_mapping, pos, endbyte);
		if (err == 0) {
			written = written_buffered;
			invalidate_mapping_pages(mapping,
						 pos >> PAGE_CACHE_SHIFT,
						 endbyte >> PAGE_CACHE_SHIFT);
		} else {
			/*
			 * We don't know how much we wrote, so just return
			 * the number of bytes which were direct-written
			 */
			// 只返回直接写入的字节数
		}
	} else {
		// 执行缓冲写入
		written = generic_file_buffered_write(iocb, iov, nr_segs,
				pos, ppos, count, written);
	}
out:
	// 清除任务的后备设备信息
	current->backing_dev_info = NULL;
	return written ? written : err;
}
EXPORT_SYMBOL(__generic_file_aio_write);

/**
 * generic_file_aio_write - write data to a file
 * @iocb:	IO state structure
 * @iov:	vector with data to write
 * @nr_segs:	number of segments in the vector
 * @pos:	position in file where to write
 *
 * This is a wrapper around __generic_file_aio_write() to be used by most
 * filesystems. It takes care of syncing the file in case of O_SYNC file
 * and acquires i_mutex as needed.
 */
/**
 * generic_file_aio_write - 向文件写入数据
 * @iocb:	IO 状态结构
 * @iov:	包含待写数据的向量
 * @nr_segs:	向量中的段数
 * @pos:	写入文件的位置
 *
 * 这是 __generic_file_aio_write() 的一个包装函数，大多数文件系统都应使用它。
 * 它处理了 O_SYNC 文件的同步写入，并在需要时获取 i_mutex。
 */
ssize_t generic_file_aio_write(struct kiocb *iocb, const struct iovec *iov,
		unsigned long nr_segs, loff_t pos)
{
	// 从 IO 控制块获取文件指针
	struct file *file = iocb->ki_filp;
	// 从文件映射中获取 inode
	struct inode *inode = file->f_mapping->host;
	ssize_t ret;

	// 确保传入的位置与 IO 控制块中的位置一致
	BUG_ON(iocb->ki_pos != pos);

	// 锁定 inode 互斥锁以保护文件写入操作
	mutex_lock(&inode->i_mutex);
	// 执行实际的异步写操作
	ret = __generic_file_aio_write(iocb, iov, nr_segs, &iocb->ki_pos);
	// 解锁 inode 互斥锁
	mutex_unlock(&inode->i_mutex);

	// 如果写入成功或操作已排队
	if (ret > 0 || ret == -EIOCBQUEUED) {
		ssize_t err;

		// 执行写入后的同步操作
		err = generic_write_sync(file, pos, ret);
		// 如果同步操作失败但写入成功，返回错误
		if (err < 0 && ret > 0)
			ret = err;
	}
	return ret;	// 返回写入结果
}
EXPORT_SYMBOL(generic_file_aio_write);

/**
 * try_to_release_page() - release old fs-specific metadata on a page
 *
 * @page: the page which the kernel is trying to free
 * @gfp_mask: memory allocation flags (and I/O mode)
 *
 * The address_space is to try to release any data against the page
 * (presumably at page->private).  If the release was successful, return `1'.
 * Otherwise return zero.
 *
 * This may also be called if PG_fscache is set on a page, indicating that the
 * page is known to the local caching routines.
 *
 * The @gfp_mask argument specifies whether I/O may be performed to release
 * this page (__GFP_IO), and whether the call may block (__GFP_WAIT & __GFP_FS).
 *
 */
int try_to_release_page(struct page *page, gfp_t gfp_mask)
{
	struct address_space * const mapping = page->mapping;

	BUG_ON(!PageLocked(page));
	if (PageWriteback(page))
		return 0;

	if (mapping && mapping->a_ops->releasepage)
		return mapping->a_ops->releasepage(page, gfp_mask);
	return try_to_free_buffers(page);
}

EXPORT_SYMBOL(try_to_release_page);
