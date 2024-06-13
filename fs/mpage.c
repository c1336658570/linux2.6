/*
 * fs/mpage.c
 *
 * Copyright (C) 2002, Linus Torvalds.
 *
 * Contains functions related to preparing and submitting BIOs which contain
 * multiple pagecache pages.
 *
 * 15May2002	Andrew Morton
 *		Initial version
 * 27Jun2002	axboe@suse.de
 *		use bio_add_page() to build bio's just the right size
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/kdev_t.h>
#include <linux/gfp.h>
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/prefetch.h>
#include <linux/mpage.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/pagevec.h>

/*
 * I/O completion handler for multipage BIOs.
 *
 * The mpage code never puts partial pages into a BIO (except for end-of-file).
 * If a page does not map to a contiguous run of blocks then it simply falls
 * back to block_read_full_page().
 *
 * Why is this?  If a page's completion depends on a number of different BIOs
 * which can complete in any order (or at the same time) then determining the
 * status of that page is hard.  See end_buffer_async_read() for the details.
 * There is no point in duplicating all that complexity.
 */
/*
 * I/O完成处理程序，用于多页BIO。
 *
 * mpage代码从不将部分页面放入BIO（除了文件末尾）。
 * 如果页面没有映射到连续的块，则它会退回到block_read_full_page()。
 *
 * 为什么会这样？如果一个页面的完成依赖于多个不同的BIO，而这些BIO可以以任何顺序（或同时）完成，那么确定该页面的状态将非常困难。
 * 参见end_buffer_async_read()了解详细信息。没有必要重复所有这些复杂性。
 */
static void mpage_end_io_read(struct bio *bio, int err)
{
	// 检查BIO是否更新
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	// 获取最后一个bio_vec
	struct bio_vec *bvec = bio->bi_io_vec + bio->bi_vcnt - 1;

	do {
		// 获取bio_vec中的页面
		struct page *page = bvec->bv_page;
		
		// 如果有下一个bio_vec，提前取回其flags
		if (--bvec >= bio->bi_io_vec)
			prefetchw(&bvec->bv_page->flags);
		
		// 如果BIO是最新的
		if (uptodate) {
			SetPageUptodate(page);	// 设置页面为最新
		} else {
			// 清除页面的最新标志
			ClearPageUptodate(page);
			// 设置页面错误
			SetPageError(page);
		}
		unlock_page(page);	// 解锁页面
		// 循环处理所有bio_vec
	} while (bvec >= bio->bi_io_vec);
	bio_put(bio);	// 释放BIO
}

static void mpage_end_io_write(struct bio *bio, int err)
{
	// 检查BIO是否更新
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	// 获取最后一个bio_vec
	struct bio_vec *bvec = bio->bi_io_vec + bio->bi_vcnt - 1;

	do {
		// 获取bio_vec中的页面
		struct page *page = bvec->bv_page;

		// 如果有下一个bio_vec，提前取回其flags
		if (--bvec >= bio->bi_io_vec)
			prefetchw(&bvec->bv_page->flags);

		// 如果BIO不是最新的
		if (!uptodate){
			// 设置页面错误
			SetPageError(page);
			if (page->mapping)
				// 设置映射错误标志
				set_bit(AS_EIO, &page->mapping->flags);
		}
		// 结束页面回写
		end_page_writeback(page);
		// 循环处理所有bio_vec
	} while (bvec >= bio->bi_io_vec);
	bio_put(bio);	// 释放BIO
}

static struct bio *mpage_bio_submit(int rw, struct bio *bio)
{
	// 设置读取的完成处理程序
	bio->bi_end_io = mpage_end_io_read;
	if (rw == WRITE)	// 如果是写操作
	// 设置写入的完成处理程序
		bio->bi_end_io = mpage_end_io_write;
	submit_bio(rw, bio);	// 提交BIO
	return NULL;					// 返回NULL
}

// 分配一个新的BIO结构并初始化它
static struct bio *
mpage_alloc(struct block_device *bdev,
		sector_t first_sector, int nr_vecs,
		gfp_t gfp_flags)
{
	struct bio *bio;

	// 尝试分配一个新的BIO结构
	bio = bio_alloc(gfp_flags, nr_vecs);

	// 如果内存分配失败，并且当前进程有PF_MEMALLOC标志
	if (bio == NULL && (current->flags & PF_MEMALLOC)) {
		while (!bio && (nr_vecs /= 2))	// 尝试减少向量数量并重新分配BIO
			bio = bio_alloc(gfp_flags, nr_vecs);
	}

	if (bio) {
		// 设置BIO的块设备
		bio->bi_bdev = bdev;
		// 设置BIO的起始扇区
		bio->bi_sector = first_sector;
	}
	return bio;	// 返回分配的BIO结构
}

/*
 * support function for mpage_readpages.  The fs supplied get_block might
 * return an up to date buffer.  This is used to map that buffer into
 * the page, which allows readpage to avoid triggering a duplicate call
 * to get_block.
 *
 * The idea is to avoid adding buffers to pages that don't already have
 * them.  So when the buffer is up to date and the page size == block size,
 * this marks the page up to date instead of adding new buffers.
 */
/*
 * 为mpage_readpages提供的支持函数。文件系统提供的get_block可能会返回一个最新的缓冲区。
 * 这用于将该缓冲区映射到页面中，这样readpage就可以避免触发重复的get_block调用。
 *
 * 其目的是避免向没有缓冲区的页面添加缓冲区。因此，当缓冲区是最新的并且页面大小等于块大小时，
 * 这会将页面标记为最新，而不是添加新的缓冲区。
 */
static void 
map_buffer_to_page(struct page *page, struct buffer_head *bh, int page_block) 
{
	// 获取页面所属的inode
	struct inode *inode = page->mapping->host;
	struct buffer_head *page_bh, *head;
	int block = 0;

	if (!page_has_buffers(page)) {
		/*
		 * don't make any buffers if there is only one buffer on
		 * the page and the page just needs to be set up to date
		 */
		/*
		 * 如果页面上只有一个缓冲区，并且页面只需要设置为最新，则不创建任何缓冲区
		 */
		if (inode->i_blkbits == PAGE_CACHE_SHIFT && 
		    buffer_uptodate(bh)) {
			// 将页面设置为最新
			SetPageUptodate(page);    
			return;
		}
		// 创建空缓冲区
		create_empty_buffers(page, 1 << inode->i_blkbits, 0);
	}
	// 获取页面的缓冲区头
	head = page_buffers(page);
	page_bh = head;
	do {
		if (block == page_block) {
			// 设置页面缓冲区的状态
			page_bh->b_state = bh->b_state;
			// 设置页面缓冲区的块设备
			page_bh->b_bdev = bh->b_bdev;
			// 设置页面缓冲区的块号
			page_bh->b_blocknr = bh->b_blocknr;
			break;
		}
		// 获取下一个页面缓冲区
		page_bh = page_bh->b_this_page;
		block++;
		// 遍历所有页面缓冲区
	} while (page_bh != head);
}

/*
 * This is the worker routine which does all the work of mapping the disk
 * blocks and constructs largest possible bios, submits them for IO if the
 * blocks are not contiguous on the disk.
 *
 * We pass a buffer_head back and forth and use its buffer_mapped() flag to
 * represent the validity of its disk mapping and to decide when to do the next
 * get_block() call.
 */
/*
 * 这是一个工作例程，它完成了所有映射磁盘块的工作，构建最大可能的BIO，
 * 并在磁盘上的块不连续时提交它们进行IO。
 *
 * 我们来回传递一个buffer_head，并使用其buffer_mapped()标志来表示其磁盘
 * 映射的有效性，并决定何时进行下一个get_block()调用。
 */
static struct bio *
do_mpage_readpage(struct bio *bio, struct page *page, unsigned nr_pages,
		sector_t *last_block_in_bio, struct buffer_head *map_bh,
		unsigned long *first_logical_block, get_block_t get_block)
{
	// 获取页面所属的inode
	struct inode *inode = page->mapping->host;
	// 获取块大小的位数
	const unsigned blkbits = inode->i_blkbits;
	 // 每页包含的块数
	const unsigned blocks_per_page = PAGE_CACHE_SIZE >> blkbits;
	// 块大小
	const unsigned blocksize = 1 << blkbits;
	sector_t block_in_file;	// 文件中的块号
	sector_t last_block;		// 最后一个块号
	sector_t last_block_in_file;	// 最后一个块号
	// 存储块号的数组
	sector_t blocks[MAX_BUF_PER_PAGE];
	unsigned page_block;	// 页块号
	// 第一个空洞块号
	unsigned first_hole = blocks_per_page;
	// 块设备指针
	struct block_device *bdev = NULL;
	int length;	// 长度
	int fully_mapped = 1;	// 是否完全映射标志
	unsigned nblocks;		// 块数
	unsigned relative_block;	// 相对块号

	// 如果页面已经有缓冲区
	if (page_has_buffers(page))
		goto confused;

	// 计算文件中的块号
	block_in_file = (sector_t)page->index << (PAGE_CACHE_SHIFT - blkbits);
	// 计算最后一个块号
	last_block = block_in_file + nr_pages * blocks_per_page;
	// 计算文件中的最后一个块号
	last_block_in_file = (i_size_read(inode) + blocksize - 1) >> blkbits;
	// 如果超出文件末尾
	if (last_block > last_block_in_file)
		// 调整最后一个块号
		last_block = last_block_in_file;
	// 初始化页块号
	page_block = 0;

	/*
	 * Map blocks using the result from the previous get_blocks call first.
	 */
	/*
	 * 首先使用上一次get_blocks调用的结果来映射块。
	 */
	nblocks = map_bh->b_size >> blkbits;	// 计算缓冲区的块数
	// 如果缓冲区已映射且块号在范围内
	if (buffer_mapped(map_bh) && block_in_file > *first_logical_block &&
			block_in_file < (*first_logical_block + nblocks)) {
		// 计算映射偏移量
		unsigned map_offset = block_in_file - *first_logical_block;
		// 计算剩余块数
		unsigned last = nblocks - map_offset;

		// 遍历相对块
		for (relative_block = 0; ; relative_block++) {
			// 如果达到最后一个块
			if (relative_block == last) {
				// 清除映射标志
				clear_buffer_mapped(map_bh);
				break;
			}
			// 如果达到页面的块数
			if (page_block == blocks_per_page)
				break;
			// 映射块号
			blocks[page_block] = map_bh->b_blocknr + map_offset +
						relative_block;
			// 增加页块号
			page_block++;
			block_in_file++;	// 增加文件块号
		}
		bdev = map_bh->b_bdev;	// 设置块设备指针
	}

	/*
	 * Then do more get_blocks calls until we are done with this page.
	 */
	/*
	 * 然后进行更多的get_blocks调用，直到处理完这个页面。
	 */
	map_bh->b_page = page;	// 设置缓冲区的页面
	// 如果还有未处理的页块
	while (page_block < blocks_per_page) {
		map_bh->b_state = 0;	// 重置缓冲区状态
		map_bh->b_size = 0;		// 重置缓冲区大小

		// 如果文件块号在范围内
		if (block_in_file < last_block) {
			// 设置缓冲区大小
			map_bh->b_size = (last_block-block_in_file) << blkbits;
			// 获取块映射
			if (get_block(inode, block_in_file, map_bh, 0))
				goto confused;
			// 更新第一个逻辑块号
			*first_logical_block = block_in_file;
		}

		// 如果缓冲区未映射
		if (!buffer_mapped(map_bh)) {
			// 设置未完全映射标志
			fully_mapped = 0;
			// 如果是第一个空洞块
			if (first_hole == blocks_per_page)
				// 设置第一个空洞块号
				first_hole = page_block;
			page_block++;			// 增加页块号
			block_in_file++;	// 增加文件块号
			continue;
		}

		/* some filesystems will copy data into the page during
		 * the get_block call, in which case we don't want to
		 * read it again.  map_buffer_to_page copies the data
		 * we just collected from get_block into the page's buffers
		 * so readpage doesn't have to repeat the get_block call
		 */
		/* 一些文件系统将在get_block调用期间将数据复制到页面，
		 * 在这种情况下，我们不希望再次读取它。
		 * map_buffer_to_page将我们刚刚从get_block收集的数据复制到页面的缓冲区中，
		 * 这样readpage就不必重复get_block调用
		 */
		 // 如果缓冲区是最新的
		if (buffer_uptodate(map_bh)) {
			// 映射缓冲区到页面
			map_buffer_to_page(page, map_bh, page_block);
			goto confused;
		}

		// 如果存在空洞块
		if (first_hole != blocks_per_page)
			// 跳到错误处理
			goto confused;		/* hole -> non-hole */

		/* Contiguous blocks? */
		/* 连续块？ */
		// 如果块不连续
		if (page_block && blocks[page_block-1] != map_bh->b_blocknr-1)
			goto confused;
		// 计算缓冲区的块数
		nblocks = map_bh->b_size >> blkbits;
		// 遍历相对块
		for (relative_block = 0; ; relative_block++) {
			// 如果达到最后一个块
			if (relative_block == nblocks) {
				// 清除映射标志
				clear_buffer_mapped(map_bh);
				break;
			// 如果达到页面的块数
			} else if (page_block == blocks_per_page)
				break;
			// 映射块号
			blocks[page_block] = map_bh->b_blocknr+relative_block;
			page_block++;	// 增加页块号
			block_in_file++;	// 增加文件块号
		}
		bdev = map_bh->b_bdev;	// 设置块设备指针
	}

	// 如果存在空洞块
	if (first_hole != blocks_per_page) {
		// 清零空洞块
		zero_user_segment(page, first_hole << blkbits, PAGE_CACHE_SIZE);
		// 如果第一个块是空洞块
		if (first_hole == 0) {
			// 设置页面为最新
			SetPageUptodate(page);
			// 解锁页面
			unlock_page(page);
			goto out;	// 跳到退出
		}
	} else if (fully_mapped) {	// 如果完全映射
		// 设置页面映射到磁盘
		SetPageMappedToDisk(page);
	}

	/*
	 * This page will go to BIO.  Do we need to send this BIO off first?
	 */
	/*
	 * 这个页面将进入BIO。我们是否需要先发送这个BIO？
	 */
	// 如果BIO存在且最后一个块不连续
	if (bio && (*last_block_in_bio != blocks[0] - 1))
		// 提交BIO
		bio = mpage_bio_submit(READ, bio);

alloc_new:
	// 如果BIO为空
	if (bio == NULL) {
		// 分配新的BIO
		bio = mpage_alloc(bdev, blocks[0] << (blkbits - 9),
			  	min_t(int, nr_pages, bio_get_nr_vecs(bdev)),
				GFP_KERNEL);
		if (bio == NULL)	// 如果分配失败
			goto confused;	// 跳到错误处理
	}

	// 计算长度
	length = first_hole << blkbits;
	// 添加页面到BIO
	if (bio_add_page(bio, page, length, 0) < length) {
		// 提交BIO
		bio = mpage_bio_submit(READ, bio);
		goto alloc_new;	// 跳到分配新的BIO
	}

	// 计算相对块号
	relative_block = block_in_file - *first_logical_block;
	// 计算块数
	nblocks = map_bh->b_size >> blkbits;
	// 如果缓冲区边界或存在空洞块
	if ((buffer_boundary(map_bh) && relative_block == nblocks) ||
	    (first_hole != blocks_per_page))
		// 提交BIO
		bio = mpage_bio_submit(READ, bio);
	else
		// 更新最后一个块号
		*last_block_in_bio = blocks[blocks_per_page - 1];
out:
	return bio;	// 返回BIO

confused:
	if (bio)	// 如果BIO存在
		// 提交BIO
		bio = mpage_bio_submit(READ, bio);
	// 如果页面不是最新的
	if (!PageUptodate(page))
		// 读取整个页面
		block_read_full_page(page, get_block);
	else
		unlock_page(page);	// 解锁页面
	goto out;	// 跳到退出
}

/**
 * mpage_readpages - populate an address space with some pages & start reads against them
 * @mapping: the address_space
 * @pages: The address of a list_head which contains the target pages.  These
 *   pages have their ->index populated and are otherwise uninitialised.
 *   The page at @pages->prev has the lowest file offset, and reads should be
 *   issued in @pages->prev to @pages->next order.
 * @nr_pages: The number of pages at *@pages
 * @get_block: The filesystem's block mapper function.
 *
 * This function walks the pages and the blocks within each page, building and
 * emitting large BIOs.
 *
 * If anything unusual happens, such as:
 *
 * - encountering a page which has buffers
 * - encountering a page which has a non-hole after a hole
 * - encountering a page with non-contiguous blocks
 *
 * then this code just gives up and calls the buffer_head-based read function.
 * It does handle a page which has holes at the end - that is a common case:
 * the end-of-file on blocksize < PAGE_CACHE_SIZE setups.
 *
 * BH_Boundary explanation:
 *
 * There is a problem.  The mpage read code assembles several pages, gets all
 * their disk mappings, and then submits them all.  That's fine, but obtaining
 * the disk mappings may require I/O.  Reads of indirect blocks, for example.
 *
 * So an mpage read of the first 16 blocks of an ext2 file will cause I/O to be
 * submitted in the following order:
 * 	12 0 1 2 3 4 5 6 7 8 9 10 11 13 14 15 16
 *
 * because the indirect block has to be read to get the mappings of blocks
 * 13,14,15,16.  Obviously, this impacts performance.
 *
 * So what we do it to allow the filesystem's get_block() function to set
 * BH_Boundary when it maps block 11.  BH_Boundary says: mapping of the block
 * after this one will require I/O against a block which is probably close to
 * this one.  So you should push what I/O you have currently accumulated.
 *
 * This all causes the disk requests to be issued in the correct order.
 */
/**
 * mpage_readpages - 使用一些页面填充地址空间并启动对它们的读取
 * @mapping: 地址空间
 * @pages: 包含目标页面的list_head地址。这些页面的index字段已填充，其他未初始化。
 *   @pages->prev处的页面具有最低的文件偏移量，读取应按@pages->prev到@pages->next的顺序发出。
 * @nr_pages: *@pages处的页面数量
 * @get_block: 文件系统的块映射函数。
 *
 * 该函数遍历页面及每个页面中的块，构建并发出大BIOs。
 *
 * 如果发生任何异常情况，例如：
 *
 * - 遇到具有缓冲区的页面
 * - 遇到在空洞后有非空洞的页面
 * - 遇到具有非连续块的页面
 *
 * 那么该代码会放弃并调用基于buffer_head的读取函数。它确实会处理末尾有空洞的页面 - 这是一个常见情况：
 * 适用于块大小小于PAGE_CACHE_SIZE的设置。
 *
 * BH_Boundary解释：
 *
 * 存在一个问题。mpage读取代码组装了多个页面，获取它们的磁盘映射，然后提交它们。这很好，但获取磁盘映射可能需要I/O。
 * 例如，读取间接块。
 *
 * 因此，ext2文件的前16个块的mpage读取将导致按以下顺序提交I/O：
 *  12 0 1 2 3 4 5 6 7 8 9 10 11 13 14 15 16
 *
 * 因为需要读取间接块以获取块13,14,15,16的映射。这显然影响了性能。
 *
 * 因此，我们允许文件系统的get_block()函数在映射块11时设置BH_Boundary。
 * BH_Boundary表示：映射此块后的块将需要对可能靠近此块的块进行I/O。
 * 因此，您应该推送当前累积的I/O。
 *
 * 这使得磁盘请求以正确的顺序发出。
 */
int
mpage_readpages(struct address_space *mapping, struct list_head *pages,
				unsigned nr_pages, get_block_t get_block)
{
	struct bio *bio = NULL;	// 指向BIO结构的指针，初始为NULL
	unsigned page_idx;			// 页面索引
	// BIO中的最后一个块
	sector_t last_block_in_bio = 0;
	// 缓冲头结构
	struct buffer_head map_bh;
	// 第一个逻辑块
	unsigned long first_logical_block = 0;

	map_bh.b_state = 0;	// 初始化缓冲头状态
	map_bh.b_size = 0;	// 初始化缓冲头大小
	// 遍历页面
	for (page_idx = 0; page_idx < nr_pages; page_idx++) {
		// 获取页面
		struct page *page = list_entry(pages->prev, struct page, lru);

		prefetchw(&page->flags);	// 预取页面标志
		list_del(&page->lru);			// 从链表中删除页面
		// 添加页面到页面缓存LRU
		if (!add_to_page_cache_lru(page, mapping,
					page->index, GFP_KERNEL)) {
			// 执行多页读取
			bio = do_mpage_readpage(bio, page,
					nr_pages - page_idx,
					&last_block_in_bio, &map_bh,
					&first_logical_block,
					get_block);
		}
		// 释放页面缓存
		page_cache_release(page);
	}
	// 如果页面列表不为空，触发BUG
	BUG_ON(!list_empty(pages));
	if (bio)
		// 提交BIO读取
		mpage_bio_submit(READ, bio);
	return 0;	// 返回0表示成功
}
EXPORT_SYMBOL(mpage_readpages);

/*
 * This isn't called much at all
 */
/*
 * 这个函数很少被调用
 */
int mpage_readpage(struct page *page, get_block_t get_block)
{
	// 指向BIO结构的指针，初始为NULL
	struct bio *bio = NULL;
	// BIO中的最后一个块
	sector_t last_block_in_bio = 0;
	// 缓冲头结构
	struct buffer_head map_bh;
	// 第一个逻辑块
	unsigned long first_logical_block = 0;

	// 初始化缓冲头状态
	map_bh.b_state = 0;
	// 初始化缓冲头大小
	map_bh.b_size = 0;
	// 执行单页读取
	bio = do_mpage_readpage(bio, page, 1, &last_block_in_bio,
			&map_bh, &first_logical_block, get_block);
	if (bio)
		// 提交BIO读取
		mpage_bio_submit(READ, bio);
	return 0;	// 返回0表示成功
}
EXPORT_SYMBOL(mpage_readpage);

/*
 * Writing is not so simple.
 *
 * If the page has buffers then they will be used for obtaining the disk
 * mapping.  We only support pages which are fully mapped-and-dirty, with a
 * special case for pages which are unmapped at the end: end-of-file.
 *
 * If the page has no buffers (preferred) then the page is mapped here.
 *
 * If all blocks are found to be contiguous then the page can go into the
 * BIO.  Otherwise fall back to the mapping's writepage().
 * 
 * FIXME: This code wants an estimate of how many pages are still to be
 * written, so it can intelligently allocate a suitably-sized BIO.  For now,
 * just allocate full-size (16-page) BIOs.
 */

struct mpage_data {
	struct bio *bio;
	sector_t last_block_in_bio;
	get_block_t *get_block;
	unsigned use_writepage;
};

static int __mpage_writepage(struct page *page, struct writeback_control *wbc,
		      void *data)
{
	struct mpage_data *mpd = data;
	struct bio *bio = mpd->bio;
	struct address_space *mapping = page->mapping;
	struct inode *inode = page->mapping->host;
	const unsigned blkbits = inode->i_blkbits;
	unsigned long end_index;
	const unsigned blocks_per_page = PAGE_CACHE_SIZE >> blkbits;
	sector_t last_block;
	sector_t block_in_file;
	sector_t blocks[MAX_BUF_PER_PAGE];
	unsigned page_block;
	unsigned first_unmapped = blocks_per_page;
	struct block_device *bdev = NULL;
	int boundary = 0;
	sector_t boundary_block = 0;
	struct block_device *boundary_bdev = NULL;
	int length;
	struct buffer_head map_bh;
	loff_t i_size = i_size_read(inode);
	int ret = 0;

	if (page_has_buffers(page)) {
		struct buffer_head *head = page_buffers(page);
		struct buffer_head *bh = head;

		/* If they're all mapped and dirty, do it */
		page_block = 0;
		do {
			BUG_ON(buffer_locked(bh));
			if (!buffer_mapped(bh)) {
				/*
				 * unmapped dirty buffers are created by
				 * __set_page_dirty_buffers -> mmapped data
				 */
				if (buffer_dirty(bh))
					goto confused;
				if (first_unmapped == blocks_per_page)
					first_unmapped = page_block;
				continue;
			}

			if (first_unmapped != blocks_per_page)
				goto confused;	/* hole -> non-hole */

			if (!buffer_dirty(bh) || !buffer_uptodate(bh))
				goto confused;
			if (page_block) {
				if (bh->b_blocknr != blocks[page_block-1] + 1)
					goto confused;
			}
			blocks[page_block++] = bh->b_blocknr;
			boundary = buffer_boundary(bh);
			if (boundary) {
				boundary_block = bh->b_blocknr;
				boundary_bdev = bh->b_bdev;
			}
			bdev = bh->b_bdev;
		} while ((bh = bh->b_this_page) != head);

		if (first_unmapped)
			goto page_is_mapped;

		/*
		 * Page has buffers, but they are all unmapped. The page was
		 * created by pagein or read over a hole which was handled by
		 * block_read_full_page().  If this address_space is also
		 * using mpage_readpages then this can rarely happen.
		 */
		goto confused;
	}

	/*
	 * The page has no buffers: map it to disk
	 */
	BUG_ON(!PageUptodate(page));
	block_in_file = (sector_t)page->index << (PAGE_CACHE_SHIFT - blkbits);
	last_block = (i_size - 1) >> blkbits;
	map_bh.b_page = page;
	for (page_block = 0; page_block < blocks_per_page; ) {

		map_bh.b_state = 0;
		map_bh.b_size = 1 << blkbits;
		if (mpd->get_block(inode, block_in_file, &map_bh, 1))
			goto confused;
		if (buffer_new(&map_bh))
			unmap_underlying_metadata(map_bh.b_bdev,
						map_bh.b_blocknr);
		if (buffer_boundary(&map_bh)) {
			boundary_block = map_bh.b_blocknr;
			boundary_bdev = map_bh.b_bdev;
		}
		if (page_block) {
			if (map_bh.b_blocknr != blocks[page_block-1] + 1)
				goto confused;
		}
		blocks[page_block++] = map_bh.b_blocknr;
		boundary = buffer_boundary(&map_bh);
		bdev = map_bh.b_bdev;
		if (block_in_file == last_block)
			break;
		block_in_file++;
	}
	BUG_ON(page_block == 0);

	first_unmapped = page_block;

page_is_mapped:
	end_index = i_size >> PAGE_CACHE_SHIFT;
	if (page->index >= end_index) {
		/*
		 * The page straddles i_size.  It must be zeroed out on each
		 * and every writepage invocation because it may be mmapped.
		 * "A file is mapped in multiples of the page size.  For a file
		 * that is not a multiple of the page size, the remaining memory
		 * is zeroed when mapped, and writes to that region are not
		 * written out to the file."
		 */
		unsigned offset = i_size & (PAGE_CACHE_SIZE - 1);

		if (page->index > end_index || !offset)
			goto confused;
		zero_user_segment(page, offset, PAGE_CACHE_SIZE);
	}

	/*
	 * This page will go to BIO.  Do we need to send this BIO off first?
	 */
	if (bio && mpd->last_block_in_bio != blocks[0] - 1)
		bio = mpage_bio_submit(WRITE, bio);

alloc_new:
	if (bio == NULL) {
		bio = mpage_alloc(bdev, blocks[0] << (blkbits - 9),
				bio_get_nr_vecs(bdev), GFP_NOFS|__GFP_HIGH);
		if (bio == NULL)
			goto confused;
	}

	/*
	 * Must try to add the page before marking the buffer clean or
	 * the confused fail path above (OOM) will be very confused when
	 * it finds all bh marked clean (i.e. it will not write anything)
	 */
	length = first_unmapped << blkbits;
	if (bio_add_page(bio, page, length, 0) < length) {
		bio = mpage_bio_submit(WRITE, bio);
		goto alloc_new;
	}

	/*
	 * OK, we have our BIO, so we can now mark the buffers clean.  Make
	 * sure to only clean buffers which we know we'll be writing.
	 */
	if (page_has_buffers(page)) {
		struct buffer_head *head = page_buffers(page);
		struct buffer_head *bh = head;
		unsigned buffer_counter = 0;

		do {
			if (buffer_counter++ == first_unmapped)
				break;
			clear_buffer_dirty(bh);
			bh = bh->b_this_page;
		} while (bh != head);

		/*
		 * we cannot drop the bh if the page is not uptodate
		 * or a concurrent readpage would fail to serialize with the bh
		 * and it would read from disk before we reach the platter.
		 */
		if (buffer_heads_over_limit && PageUptodate(page))
			try_to_free_buffers(page);
	}

	BUG_ON(PageWriteback(page));
	set_page_writeback(page);
	unlock_page(page);
	if (boundary || (first_unmapped != blocks_per_page)) {
		bio = mpage_bio_submit(WRITE, bio);
		if (boundary_block) {
			write_boundary_block(boundary_bdev,
					boundary_block, 1 << blkbits);
		}
	} else {
		mpd->last_block_in_bio = blocks[blocks_per_page - 1];
	}
	goto out;

confused:
	if (bio)
		bio = mpage_bio_submit(WRITE, bio);

	if (mpd->use_writepage) {
		ret = mapping->a_ops->writepage(page, wbc);
	} else {
		ret = -EAGAIN;
		goto out;
	}
	/*
	 * The caller has a ref on the inode, so *mapping is stable
	 */
	mapping_set_error(mapping, ret);
out:
	mpd->bio = bio;
	return ret;
}

/**
 * mpage_writepages - walk the list of dirty pages of the given address space & writepage() all of them
 * @mapping: address space structure to write
 * @wbc: subtract the number of written pages from *@wbc->nr_to_write
 * @get_block: the filesystem's block mapper function.
 *             If this is NULL then use a_ops->writepage.  Otherwise, go
 *             direct-to-BIO.
 *
 * This is a library function, which implements the writepages()
 * address_space_operation.
 *
 * If a page is already under I/O, generic_writepages() skips it, even
 * if it's dirty.  This is desirable behaviour for memory-cleaning writeback,
 * but it is INCORRECT for data-integrity system calls such as fsync().  fsync()
 * and msync() need to guarantee that all the data which was dirty at the time
 * the call was made get new I/O started against them.  If wbc->sync_mode is
 * WB_SYNC_ALL then we were called for data integrity and we must wait for
 * existing IO to complete.
 */
/**
 * mpage_writepages - 遍历给定地址空间的脏页列表并写入所有这些页
 * @mapping: 要写入的地址空间结构
 * @wbc: 从 *@wbc->nr_to_write 减去已写入的页数
 * @get_block: 文件系统的块映射函数。
 *             如果此参数为 NULL，则使用 a_ops->writepage。否则，直接使用BIO方式。
 *
 * 这是一个库函数，它实现了 writepages() 地址空间操作。
 *
 * 如果某页已经在进行 I/O，generic_writepages() 会跳过它，即使它是脏的。
 * 这对于清理内存的写回是理想的行为，但对于像 fsync() 这样的数据完整性系统调用来说是错误的。
 * fsync() 和 msync() 需要保证在调用发生时所有脏数据都启动了新的 I/O。
 * 如果 wbc->sync_mode 是 WB_SYNC_ALL，那么我们是为了数据完整性而被调用的，我们必须等待现有的IO完成。
 */
int
mpage_writepages(struct address_space *mapping,
		struct writeback_control *wbc, get_block_t get_block)
{
	int ret;

	// 如果没有提供特定的块映射函数，使用通用的writepages方法
	if (!get_block)
		ret = generic_writepages(mapping, wbc);
	else {
		struct mpage_data mpd = {
			.bio = NULL,	// 初始化bio结构为NULL
			.last_block_in_bio = 0,	// 最后一个bio块的索引初始化为0
			.get_block = get_block,	// 设置块映射函数
			.use_writepage = 1,			// 设置使用writepage的标志
		};

		// 执行具体的页面写操作
		ret = write_cache_pages(mapping, wbc, __mpage_writepage, &mpd);
		// 如果bio不为空，则提交bio
		if (mpd.bio)
			mpage_bio_submit(WRITE, mpd.bio);
	}
	return ret;
}
EXPORT_SYMBOL(mpage_writepages);

int mpage_writepage(struct page *page, get_block_t get_block,
	struct writeback_control *wbc)
{
	struct mpage_data mpd = {
		.bio = NULL,	// 初始化bio结构为NULL
		.last_block_in_bio = 0,	// 最后一个bio块的索引初始化为0
		.get_block = get_block,	// 设置块映射函数
		.use_writepage = 0,			// 设置不使用writepage的标志
	};
	// 调用 __mpage_writepage 函数进行页的写操作
	int ret = __mpage_writepage(page, wbc, &mpd);
	// 如果bio不为空，则提交bio
	if (mpd.bio)
		mpage_bio_submit(WRITE, mpd.bio);
	return ret;
}
EXPORT_SYMBOL(mpage_writepage);
