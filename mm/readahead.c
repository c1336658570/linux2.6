/*
 * mm/readahead.c - address_space-level file readahead.
 *
 * Copyright (C) 2002, Linus Torvalds
 *
 * 09Apr2002	Andrew Morton
 *		Initial version.
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/pagevec.h>
#include <linux/pagemap.h>

/*
 * Initialise a struct file's readahead state.  Assumes that the caller has
 * memset *ra to zero.
 */
/*
 * 初始化 struct file 的预读状态。假定调用者已将 *ra 置零。
 */
// 用于初始化文件的预读状态，主要设置了预读页面数和前一个位置的初始状态。
void
file_ra_state_init(struct file_ra_state *ra, struct address_space *mapping)
{
	// 设置预读页面数，从关联的设备信息中获取
	ra->ra_pages = mapping->backing_dev_info->ra_pages;
	// 初始化前一个位置为 -1，表示没有前一个位置
	ra->prev_pos = -1;
}
EXPORT_SYMBOL_GPL(file_ra_state_init);

// 宏定义，用于从链表头获取对应的 struct page 结构体
#define list_to_page(head) (list_entry((head)->prev, struct page, lru))

/*
 * see if a page needs releasing upon read_cache_pages() failure
 * - the caller of read_cache_pages() may have set PG_private or PG_fscache
 *   before calling, such as the NFS fs marking pages that are cached locally
 *   on disk, thus we need to give the fs a chance to clean up in the event of
 *   an error
 */
/*
 * 检查在 read_cache_pages() 失败时是否需要释放页面
 * - read_cache_pages() 的调用者可能在调用前已设置了 PG_private 或 PG_fscache
 *   例如 NFS 文件系统标记本地缓存到磁盘的页面，因此在出现错误时，需要给文件系统一个机会来清理
 */
// 用于处理读缓存页面失败时的清理工作，如果页面有私有标志，则进行页面锁定和解锁，
// 执行页面失效处理，最后释放页面资源。这些操作确保了在发生错误时，文件系统有机会进行
// 适当的清理，防止资源泄露和数据不一致。
static void read_cache_pages_invalidate_page(struct address_space *mapping,
					     struct page *page)
{
	// 检查页面是否设置了私有标志
	if (page_has_private(page)) {
		// 尝试锁定页面，如果失败则抛出异常
		if (!trylock_page(page))
			BUG();
		// 设置页面的映射
		page->mapping = mapping;
		// 执行页面失效处理
		do_invalidatepage(page, 0);
		// 清除页面的映射
		page->mapping = NULL;
		// 解锁页面
		unlock_page(page);
	}
	// 释放页面
	page_cache_release(page);
}

/*
 * release a list of pages, invalidating them first if need be
 */
/*
 * 释放一个页面列表，如果需要的话先使它们无效
 */
// 负责在需要时使页面列表无效。
static void read_cache_pages_invalidate_pages(struct address_space *mapping,
					      struct list_head *pages)
{
	struct page *victim;

	// 当页面列表不为空时循环
	while (!list_empty(pages)) {
		// 从列表中获取一个页面
		victim = list_to_page(pages);
		list_del(&victim->lru);	// 从列表中删除该页面
		// 将页面置为无效
		read_cache_pages_invalidate_page(mapping, victim);
	}
}

/**
 * read_cache_pages - populate an address space with some pages & start reads against them
 * @mapping: the address_space
 * @pages: The address of a list_head which contains the target pages.  These
 *   pages have their ->index populated and are otherwise uninitialised.
 * @filler: callback routine for filling a single page.
 * @data: private data for the callback routine.
 *
 * Hides the details of the LRU cache etc from the filesystems.
 */
/**
 * read_cache_pages - 使用一些页面填充地址空间并启动对它们的读取
 * @mapping: 地址空间
 * @pages: 包含目标页面的 list_head 的地址。这些页面的 ->index 已经设置，其他未初始化。
 * @filler: 填充单个页面的回调例程。
 * @data: 回调例程的私有数据。
 *
 * 对文件系统隐藏了LRU缓存等细节。
 */
//  负责将未初始化的页面列表填充数据，并启动对这些页面的读操作。这里涉及到地址空间和
// 页面缓存管理的细节，通过回调函数 filler 允许调用者自定义如何处理每个页面。
int read_cache_pages(struct address_space *mapping, struct list_head *pages,
			int (*filler)(void *, struct page *), void *data)
{
	struct page *page;
	int ret = 0;

	// 当页面列表不为空时循环
	while (!list_empty(pages)) {
		// 从列表中获取一个页面
		page = list_to_page(pages);
		// 从列表中删除该页面
		list_del(&page->lru);
		// 尝试添加页面到缓存
		if (add_to_page_cache_lru(page, mapping,
					page->index, GFP_KERNEL)) {
			// 如果添加失败，则使页面无效
			read_cache_pages_invalidate_page(mapping, page);
			continue;
		}
		// 释放页面
		page_cache_release(page);

		// 使用提供的填充函数填充页面
		ret = filler(data, page);
		if (unlikely(ret)) {	// 如果填充失败
			// 使所有页面无效
			read_cache_pages_invalidate_pages(mapping, pages);
			break;
		}
		// 记录读取的数据量
		task_io_account_read(PAGE_CACHE_SIZE);
	}
	return ret;	// 返回结果
}

EXPORT_SYMBOL(read_cache_pages);

// 通过直接或通过回调函数处理页面列表的方式来读取文件数据，具体使用哪种方式取决于
// address_space结构中是否定义了readpages方法。如果定义了readpages方法，就直接
// 调用它处理所有页面；如果没有定义，就遍历页面列表，逐个处理每个页面。处理页面时首
// 先尝试将其加入页面缓存，如果加入成功则调用readpage方法从文件中读取数据到页面中。
// 处理完所有页面后，通过返回值告知调用者操作结果。
static int read_pages(struct address_space *mapping, struct file *filp,
		struct list_head *pages, unsigned nr_pages)
{
	unsigned page_idx;	// 用于循环的页面索引
	int ret;		// 用来存放函数的返回值

	// 如果存在readpages操作，则调用之
	if (mapping->a_ops->readpages) {
		ret = mapping->a_ops->readpages(filp, mapping, pages, nr_pages);
		/* Clean up the remaining pages */
		put_pages_list(pages);	// 清理剩余的页面列表
		goto out;	// 跳转到函数结束部分
	}

	// 如果没有readpages操作，对每个页面进行处理
	for (page_idx = 0; page_idx < nr_pages; page_idx++) {
		// 从页面列表中取出一个页面
		struct page *page = list_to_page(pages);
		// 从列表中删除这个页面
		list_del(&page->lru);
		// 尝试将页面加入到页面缓存
		if (!add_to_page_cache_lru(page, mapping,
					page->index, GFP_KERNEL)) {
			// 如果成功，则调用readpage操作
			mapping->a_ops->readpage(filp, page);
		}
		page_cache_release(page);	// 释放页面
	}
	ret = 0;		// 设置返回值为0，表示成功
out:
	return ret;	// 返回操作结果
}

/*
 * __do_page_cache_readahead() actually reads a chunk of disk.  It allocates all
 * the pages first, then submits them all for I/O. This avoids the very bad
 * behaviour which would occur if page allocations are causing VM writeback.
 * We really don't want to intermingle reads and writes like that.
 *
 * Returns the number of pages requested, or the maximum amount of I/O allowed.
 */
/*
 * __do_page_cache_readahead() 实际上是读取了一大块磁盘数据。它首先分配所有
 * 需要的页面，然后一起提交它们进行I/O。这样做是为了避免由页面分配引起的虚拟内存写回
 * 导致的非常糟糕的行为。我们真的不希望将读和写操作混合在一起。
 *
 * 返回请求的页面数量，或允许的最大I/O量。
 */
// 执行实际的页面缓存预读的函数
static int
__do_page_cache_readahead(struct address_space *mapping, struct file *filp,
			pgoff_t offset, unsigned long nr_to_read,
			unsigned long lookahead_size)
{
	// 获取文件的inode
	struct inode *inode = mapping->host;
	struct page *page;	// 用于存储分配的页面
	/* 我们希望读取的最后一页 */
	unsigned long end_index;	/* The last page we want to read */
	// 初始化用于收集页面的列表
	LIST_HEAD(page_pool);	// 初始化页面池链表
	int page_idx;		// 页索引变量
	int ret = 0;	// 用于记录分配的页面数
	loff_t isize = i_size_read(inode);	// 文件大小

	if (isize == 0)	// 如果文件大小为0，则直接退出
		goto out;

	// 计算最后一页的索引
	end_index = ((isize - 1) >> PAGE_CACHE_SHIFT);

	/*
	 * Preallocate as many pages as we will need.
	 */
	/*
	 * 预分配所需的所有页面。
	 */
	for (page_idx = 0; page_idx < nr_to_read; page_idx++) {
		// 计算当前页的偏移
		pgoff_t page_offset = offset + page_idx;

		// 如果当前页超出了文件大小，终止循环
		if (page_offset > end_index)
			break;

		rcu_read_lock();
		// 查找是否已有该页面
		page = radix_tree_lookup(&mapping->page_tree, page_offset);
		rcu_read_unlock();
		if (page)	// 如果页面已存在，则跳过
			continue;

		// 分配一个新的页面
		page = page_cache_alloc_cold(mapping);
		if (!page)
			break;
		page->index = page_offset;
		// 将新页面添加到列表
		list_add(&page->lru, &page_pool);
		if (page_idx == nr_to_read - lookahead_size)
			SetPageReadahead(page);	// 标记为预读页面
		ret++;
	}

	/*
	 * Now start the IO.  We ignore I/O errors - if the page is not
	 * uptodate then the caller will launch readpage again, and
	 * will then handle the error.
	 */
	/*
	 * 现在开始I/O。我们忽略I/O错误 - 如果页面不是最新的，调用者将再次启动readpage，
	 * 然后处理错误。
	 */
	if (ret)
		// 提交所有页面进行读取
		read_pages(mapping, filp, &page_pool, ret);
	BUG_ON(!list_empty(&page_pool));	// 确保所有页面都已处理
out:
	return ret;	// 返回分配并提交读取的页面数量
}

/*
 * Chunk the readahead into 2 megabyte units, so that we don't pin too much
 * memory at once.
 */
/*
 * 将预读分成2MB的单元，这样我们就不会一次性固定太多内存。
 */
// 用于强制执行页面缓存预读的函数。
// 将每块限制在2MB内，并在执行读取操作前进行页面数合理性的限制。
// 这样可以避免由于一次请求过多页面而导致的内存占用过高，同时通过逐块处理保证了系统的响应性。
int force_page_cache_readahead(struct address_space *mapping, struct file *filp,
		pgoff_t offset, unsigned long nr_to_read)
{
	// 记录总共读取的页面数
	int ret = 0;

	// 如果没有定义读取页面的操作，则返回错误
	if (unlikely(!mapping->a_ops->readpage && !mapping->a_ops->readpages))
		return -EINVAL;

	// 调整读取页面的数量
	nr_to_read = max_sane_readahead(nr_to_read);
	while (nr_to_read) {
		int err;

		// 计算要读的2MB的块所对应的页数量
		unsigned long this_chunk = (2 * 1024 * 1024) / PAGE_CACHE_SIZE;
		
		// 如果计算出的块大小大于剩余要读的页数，则调整
		if (this_chunk > nr_to_read)
			this_chunk = nr_to_read;
		// 执行预读
		err = __do_page_cache_readahead(mapping, filp,
						offset, this_chunk, 0);
		if (err < 0) {	// 如果发生错误，更新返回值并退出循环
			ret = err;
			break;
		}
		ret += err;	// 累加成功读取的页面数
		offset += this_chunk;	// 增加偏移量
		nr_to_read -= this_chunk;	// 减少剩余要读的页面数
	}
	return ret;	// 返回结果
}

/*
 * Given a desired number of PAGE_CACHE_SIZE readahead pages, return a
 * sensible upper limit.
 */
/*
 * 给定期望的以PAGE_CACHE_SIZE为单位的预读页面数量，返回一个合理的上限。
 */
unsigned long max_sane_readahead(unsigned long nr)
{
	// 返回较小的值，即请求的页面数或者节点上空闲和非活跃文件页面数的一半
	return min(nr, (node_page_state(numa_node_id(), NR_INACTIVE_FILE)
		+ node_page_state(numa_node_id(), NR_FREE_PAGES)) / 2);
}

/*
 * Submit IO for the read-ahead request in file_ra_state.
 */
/*
 * 提交文件预读请求中的IO。
 */
unsigned long ra_submit(struct file_ra_state *ra,
		       struct address_space *mapping, struct file *filp)
{
	int actual;	// 用于存储实际执行的预读页面数量

	// 调用 __do_page_cache_readahead 函数执行实际的预读
	actual = __do_page_cache_readahead(mapping, filp,
					ra->start, ra->size, ra->async_size);

	return actual;	// 返回实际预读的页面数量
}

/*
 * Set the initial window size, round to next power of 2 and square
 * for small size, x 4 for medium, and x 2 for large
 * for 128k (32 page) max ra
 * 1-8 page = 32k initial, > 8 page = 128k initial
 */
/*
 * 设置初始窗口大小，向上取至下一个2的幂次方，并根据大小不同进行放大。
 * 对于小窗口，放大至原来的4倍；对于中等大小的窗口，放大至原来的2倍；
 * 对于大窗口，使用最大值 max。
 * 最大预读为128k（32页）：1-8页 = 初始32k，大于8页 = 初始128k。
 */
static unsigned long get_init_ra_size(unsigned long size, unsigned long max)
{
	// 将 size 向上取至最接近的2的幂次方
	unsigned long newsize = roundup_pow_of_two(size);

	// 如果新尺寸较小，则放大4倍
	if (newsize <= max / 32)
		newsize = newsize * 4;
	// 如果新尺寸中等，则放大2倍
	else if (newsize <= max / 4)
		newsize = newsize * 2;
	// 如果新尺寸很大，使用最大值
	else
		newsize = max;

	// 返回调整后的窗口大小
	return newsize;
}

/*
 *  Get the previous window size, ramp it up, and
 *  return it as the new window size.
 */
/*
 * 获取当前窗口大小，增加它，并返回作为新的窗口大小。
 */
static unsigned long get_next_ra_size(struct file_ra_state *ra,
						unsigned long max)
{
	unsigned long cur = ra->size;	// 当前窗口大小
	unsigned long newsize;

	// 如果当前大小较小，放大4倍
	if (cur < max / 16)
		newsize = 4 * cur;
	// 否则放大2倍
	else
		newsize = 2 * cur;

	// 返回新大小，但不超过最大值
	return min(newsize, max);
}

/*
 * On-demand readahead design.
 *
 * The fields in struct file_ra_state represent the most-recently-executed
 * readahead attempt:
 *
 *                        |<----- async_size ---------|
 *     |------------------- size -------------------->|
 *     |==================#===========================|
 *     ^start             ^page marked with PG_readahead
 *
 * To overlap application thinking time and disk I/O time, we do
 * `readahead pipelining': Do not wait until the application consumed all
 * readahead pages and stalled on the missing page at readahead_index;
 * Instead, submit an asynchronous readahead I/O as soon as there are
 * only async_size pages left in the readahead window. Normally async_size
 * will be equal to size, for maximum pipelining.
 *
 * In interleaved sequential reads, concurrent streams on the same fd can
 * be invalidating each other's readahead state. So we flag the new readahead
 * page at (start+size-async_size) with PG_readahead, and use it as readahead
 * indicator. The flag won't be set on already cached pages, to avoid the
 * readahead-for-nothing fuss, saving pointless page cache lookups.
 *
 * prev_pos tracks the last visited byte in the _previous_ read request.
 * It should be maintained by the caller, and will be used for detecting
 * small random reads. Note that the readahead algorithm checks loosely
 * for sequential patterns. Hence interleaved reads might be served as
 * sequential ones.
 *
 * There is a special-case: if the first page which the application tries to
 * read happens to be the first page of the file, it is assumed that a linear
 * read is about to happen and the window is immediately set to the initial size
 * based on I/O request size and the max_readahead.
 *
 * The code ramps up the readahead size aggressively at first, but slow down as
 * it approaches max_readhead.
 */

/*
 * Count contiguously cached pages from @offset-1 to @offset-@max,
 * this count is a conservative estimation of
 * 	- length of the sequential read sequence, or
 * 	- thrashing threshold in memory tight systems
 */
/*
 * 按需预读设计。
 *
 * struct file_ra_state 中的字段代表最近一次执行的预读尝试：
 *
 *                        |<----- async_size ---------|
 *     |------------------- size -------------------->|
 *     |==================#===========================|
 *     ^start             ^标记为 PG_readahead 的页
 *
 * 为了重叠应用程序的思考时间和磁盘I/O时间，我们进行 `预读流水线`：不等到应用程序
 * 消耗所有预读页并在 readahead_index 处的缺失页上停止；而是一旦预读窗口中只剩下
 * async_size 页时，就提交一个异步预读I/O。通常 async_size 将等于 size，以实现最大的流水线效果。
 *
 * 在交错的顺序读取中，同一个文件描述符上的并发流可能会使彼此的预读状态失效。
 * 因此，我们在 (start+size-async_size) 处标记新的预读页为 PG_readahead，
 * 并将其用作预读指示器。已缓存的页面不会设置该标志，以避免无谓的预读和无意义的
 * 页缓存查找。
 *
 * prev_pos 跟踪上一个读请求中访问的最后一个字节。它应由调用者维护，并将用于检测
 * 小的随机读取。注意，预读算法松散地检查顺序模式。因此，交错的读取可能会被视为
 * 顺序读取。
 *
 * 有一个特殊情况：如果应用程序尝试读取的第一页恰好是文件的第一页，假设将要进行线性读取，
 * 窗口会立即设置为基于 I/O 请求大小和最大预读大小的初始大小。
 *
 * 代码在开始时会积极增加预读大小，但在接近最大预读值时会放慢速度。
 */

/*
 * 从 @offset-1 到 @offset-@max 计算连续缓存的页数，
 * 这个计数是对以下内容的保守估计：
 * 	- 顺序读取序列的长度，或
 * 	- 内存紧张系统中的抖动阈值
 */
// 通过动态调整预读窗口大小来优化顺序和随机读取的性能。通过标记 PG_readahead 页面并
// 利用历史页面计数来判断是否需要扩展预读窗口，从而提前加载可能会被访问的数据，
// 减少了因等待数据加载而产生的延迟。
static pgoff_t count_history_pages(struct address_space *mapping,
				   struct file_ra_state *ra,
				   pgoff_t offset, unsigned long max)
{
	pgoff_t head;	// 用于存储查找结果

	// 进行RCU锁定以进行并发访问保护
	rcu_read_lock();
	// 在 radix 树中查找前一个空洞（未缓存的页）
	head = radix_tree_prev_hole(&mapping->page_tree, offset - 1, max);
	rcu_read_unlock();	// 释放RCU锁

	// 计算从指定偏移到第一个空洞之间的页数
	return offset - 1 - head;
}

/*
 * page cache context based read-ahead
 */
/*
 * 基于页缓存上下文的预读。
 */
// 通过分析当前请求的上下文来决定是否进行预读。它首先计算当前请求前的连续缓存页数，
// 如果没有连续页，可能是随机读取，不执行预读。如果有连续页，尤其是从文件开始的连续页，
// 将预读范围增加，这通常表明是顺序读取或整个文件的读取。然后，根据这些条件动态设置预读
// 窗口的大小，并尝试预读数据以提高后续访问的效率。这种方法旨在智能地预测读取模式，从而
// 减少I/O延迟并提高系统性能。
static int try_context_readahead(struct address_space *mapping,
				 struct file_ra_state *ra,
				 pgoff_t offset,
				 unsigned long req_size,
				 unsigned long max)
{
	// 用于存储历史页计数
	pgoff_t size;

	// 计算从给定偏移开始的历史连续页数
	size = count_history_pages(mapping, ra, offset, max);

	/*
	 * no history pages:
	 * it could be a random read
	 */
	/*
	 * 如果没有历史页：
	 * 可能是随机读取
	 */
	if (!size)
		return 0;

	/*
	 * starts from beginning of file:
	 * it is a strong indication of long-run stream (or whole-file-read)
	 */
	/*
	 * 从文件开始：
	 * 强烈表明这是一个长期流（或全文件读取）
	 */
	// 如果历史页数量大于等于当前偏移，可能是一个顺序读取，增加预读大小
	if (size >= offset)
		size *= 2;

	// 设置预读状态
	ra->start = offset;	// 设置预读开始位置
	// 计算并设置初始预读大小
	ra->size = get_init_ra_size(size + req_size, max);
	// 设置异步预读大小为预读大小
	ra->async_size = ra->size;

	return 1;	// 返回1，表示进行了基于上下文的预读
}

/*
 * A minimal readahead algorithm for trivial sequential/random reads.
 */
/*
 * 为简单的顺序/随机读取实现一个最小化的预读算法。
 */
static unsigned long
ondemand_readahead(struct address_space *mapping,
		   struct file_ra_state *ra, struct file *filp,
		   bool hit_readahead_marker, pgoff_t offset,
		   unsigned long req_size)
{
	// 计算合理的最大预读大小。
	unsigned long max = max_sane_readahead(ra->ra_pages);

	/*
	 * start of file
	 */
	/*
	 * 文件开始
	 */
	if (!offset)	// 如果偏移为0，表示从文件开始处读取
		goto initial_readahead;

	/*
	 * It's the expected callback offset, assume sequential access.
	 * Ramp up sizes, and push forward the readahead window.
	 */
	/*
	 * 如果是预期的回调偏移，假定为顺序访问。
	 * 增加大小，并推动预读窗口前进。
	 */
	if ((offset == (ra->start + ra->size - ra->async_size) ||
	     offset == (ra->start + ra->size))) {
		ra->start += ra->size;	 // 更新预读起始位置。
		// 获取下一个预读大小。
		ra->size = get_next_ra_size(ra, max);
		ra->async_size = ra->size;	// 设置异步预读大小。
		goto readit;	// 跳转到读取操作。
	}

	/*
	 * Hit a marked page without valid readahead state.
	 * E.g. interleaved reads.
	 * Query the pagecache for async_size, which normally equals to
	 * readahead size. Ramp it up and use it as the new readahead size.
	 */
	/*
	 * 遇到一个标记的页面，但没有有效的预读状态。
	 * 如交错读取。查询页面缓存以获得 async_size，通常等于预读大小。
	 * 增加它并将其作为新的预读大小。
	 */
	if (hit_readahead_marker) {
		pgoff_t start;

		rcu_read_lock();
		// 查询下一个空洞。
		start = radix_tree_next_hole(&mapping->page_tree, offset+1,max);
		rcu_read_unlock();

		// 如果没有找到或者太远，则不进行预读。
		if (!start || start - offset > max)
			return 0;

		// 设置预读的起始位置。
		ra->start = start;
		/* 旧的 async_size */
		// 更新预读大小。
		ra->size = start - offset;	/* old async_size */
		// 增加请求大小。
		ra->size += req_size;
		// 获取下一次预读大小。
		ra->size = get_next_ra_size(ra, max);
		// 更新异步预读大小。
		ra->async_size = ra->size;
		// 进行预读。
		goto readit;
	}

	/*
	 * oversize read
	 */
	/*
	 * 过大的读取
	 */
	// 如果请求大小超过最大预读大小，进行初始预读。
	if (req_size > max)
		goto initial_readahead;

	/*
	 * sequential cache miss
	 */
	/*
	 * 顺序缓存未命中
	 */
	// 如果是连续的缓存未命中，进行初始预读。
	if (offset - (ra->prev_pos >> PAGE_CACHE_SHIFT) <= 1UL)
		goto initial_readahead;

	/*
	 * Query the page cache and look for the traces(cached history pages)
	 * that a sequential stream would leave behind.
	 */
	/*
	 * 查询页面缓存，查找顺序流留下的痕迹（缓存的历史页面）。
	 */
	if (try_context_readahead(mapping, ra, offset, req_size, max))
		goto readit;	// 如果上下文预读成功，进行预读。

	/*
	 * standalone, small random read
	 * Read as is, and do not pollute the readahead state.
	 */
	/*
	 * 独立的小随机读取
	 * 原样读取，不污染预读状态。
	 */
	// 直接进行页缓存预读，不更新预读状态。
	return __do_page_cache_readahead(mapping, filp, offset, req_size, 0);

initial_readahead:
	ra->start = offset;	// 设置预读的起始位置。
	// 获取初始预读大小。
	ra->size = get_init_ra_size(req_size, max);
	// 设置异步预读大小。
	ra->async_size = ra->size > req_size ? ra->size - req_size : ra->size;

readit:
	/*
	 * Will this read hit the readahead marker made by itself?
	 * If so, trigger the readahead marker hit now, and merge
	 * the resulted next readahead window into the current one.
	 */
	/*
	 * 这次读取会触发它自己设置的预读标记吗？
	 * 如果是，现在就触发预读标记，并将结果合并到当前窗口。
	 */
	if (offset == ra->start && ra->size == ra->async_size) {
		// 获取下一个预读大小。
		ra->async_size = get_next_ra_size(ra, max);
		// 更新预读大小。
		ra->size += ra->async_size;
	}

	return ra_submit(ra, mapping, filp);	// 提交预读请求。
}

/**
 * page_cache_sync_readahead - generic file readahead
 * @mapping: address_space which holds the pagecache and I/O vectors
 * @ra: file_ra_state which holds the readahead state
 * @filp: passed on to ->readpage() and ->readpages()
 * @offset: start offset into @mapping, in pagecache page-sized units
 * @req_size: hint: total size of the read which the caller is performing in
 *            pagecache pages
 *
 * page_cache_sync_readahead() should be called when a cache miss happened:
 * it will submit the read.  The readahead logic may decide to piggyback more
 * pages onto the read request if access patterns suggest it will improve
 * performance.
 */
/**
 * page_cache_sync_readahead - 通用文件预读函数
 * @mapping: 包含页面缓存和I/O向量的地址空间
 * @ra: 包含预读状态的file_ra_state结构
 * @filp: 传递给->readpage()和->readpages()
 * @offset: @mapping中的起始偏移量，以页面缓存页面大小为单位
 * @req_size: 提示：调用者执行的读操作的总大小，以页面缓存页面为单位
 *
 * 当发生缓存未命中时应调用 page_cache_sync_readahead()：它将提交读请求。
 * 根据访问模式，预读逻辑可能决定将更多页面加入读请求以提升性能。
 */
/**
 * 实现了一个通用文件预读机制。根据文件的访问模式（随机或顺序）和预读状态，
 * 函数会决定是否进行预读以及预读的范围。如果是随机访问模式，将采用强制预读；
 * 否则，根据需要进行按需预读。这有助于提高顺序访问文件时的性能，因为预读可以
 * 减少将来的I/O等待时间。
 */
void page_cache_sync_readahead(struct address_space *mapping,
			       struct file_ra_state *ra, struct file *filp,
			       pgoff_t offset, unsigned long req_size)
{
	/* no read-ahead */
	/* 如果没有预读页，则不进行预读 */
	if (!ra->ra_pages)
		return;

	/* be dumb */
	/* 若文件被标记为随机访问模式，则使用强制预读 */
	if (filp && (filp->f_mode & FMODE_RANDOM)) {
		force_page_cache_readahead(mapping, filp, offset, req_size);
		return;
	}

	/* do read-ahead */
	/* 执行预读 */
	ondemand_readahead(mapping, ra, filp, false, offset, req_size);
}
EXPORT_SYMBOL_GPL(page_cache_sync_readahead);

/**
 * page_cache_async_readahead - file readahead for marked pages
 * @mapping: address_space which holds the pagecache and I/O vectors
 * @ra: file_ra_state which holds the readahead state
 * @filp: passed on to ->readpage() and ->readpages()
 * @page: the page at @offset which has the PG_readahead flag set
 * @offset: start offset into @mapping, in pagecache page-sized units
 * @req_size: hint: total size of the read which the caller is performing in
 *            pagecache pages
 *
 * page_cache_async_ondemand() should be called when a page is used which
 * has the PG_readahead flag; this is a marker to suggest that the application
 * has used up enough of the readahead window that we should start pulling in
 * more pages.
 */
/**
 * page_cache_async_readahead - 异步文件预读，用于标记页面
 * @mapping: 包含页缓存和I/O向量的address_space
 * @ra: 保存预读状态的file_ra_state
 * @filp: 传递给->readpage()和->readpages()的文件指针
 * @page: 在@offset处设置了PG_readahead标志的页面
 * @offset: 在@mapping中的起始偏移量，以页缓存页大小为单位
 * @req_size: 提示：调用者执行的读取总大小，以页缓存页为单位
 *
 * 当使用带有PG_readahead标志的页面时，应调用page_cache_async_readahead()；
 * 这是一个标记，表明应用程序已经使用了足够的预读窗口，我们应该开始拉入更多的页面。
 */
void
page_cache_async_readahead(struct address_space *mapping,
			   struct file_ra_state *ra, struct file *filp,
			   struct page *page, pgoff_t offset,
			   unsigned long req_size)
{
	/* no read-ahead */
	/* 不进行预读 */
	if (!ra->ra_pages)
		return;

	/*
	 * Same bit is used for PG_readahead and PG_reclaim.
	 */
	/*
	 * 相同的位用于PG_readahead和PG_reclaim。
	 */
	if (PageWriteback(page))
		return;

	// 清除页面的PG_readahead标志
	ClearPageReadahead(page);

	/*
	 * Defer asynchronous read-ahead on IO congestion.
	 */
	/*
	 * 在IO拥塞时推迟异步预读。
	 */
	if (bdi_read_congested(mapping->backing_dev_info))
		return;

	/* do read-ahead */
	/* 进行预读 */
	ondemand_readahead(mapping, ra, filp, true, offset, req_size);

#ifdef CONFIG_BLOCK
	/*
	 * Normally the current page is !uptodate and lock_page() will be
	 * immediately called to implicitly unplug the device. However this
	 * is not always true for RAID conifgurations, where data arrives
	 * not strictly in their submission order. In this case we need to
	 * explicitly kick off the IO.
	 */
	/*
	 * 通常当前页面不是uptodate，lock_page()将被立即调用以隐式地解锁设备。
	 * 但在RAID配置中，数据不会严格按提交顺序到达。在这种情况下，我们需要显式地启动IO。
	 */
	if (PageUptodate(page))
		blk_run_backing_dev(mapping->backing_dev_info, NULL);
#endif
}
EXPORT_SYMBOL_GPL(page_cache_async_readahead);
