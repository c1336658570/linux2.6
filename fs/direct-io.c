/*
 * fs/direct-io.c
 *
 * Copyright (C) 2002, Linus Torvalds.
 *
 * O_DIRECT
 *
 * 04Jul2002	Andrew Morton
 *		Initial version
 * 11Sep2002	janetinc@us.ibm.com
 * 		added readv/writev support.
 * 29Oct2002	Andrew Morton
 *		rewrote bio_add_page() support.
 * 30Oct2002	pbadari@us.ibm.com
 *		added support for non-aligned IO.
 * 06Nov2002	pbadari@us.ibm.com
 *		added asynchronous IO support.
 * 21Jul2003	nathans@sgi.com
 *		added IO completion notifier.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/bio.h>
#include <linux/wait.h>
#include <linux/err.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/rwsem.h>
#include <linux/uio.h>
#include <asm/atomic.h>

/*
 * How many user pages to map in one call to get_user_pages().  This determines
 * the size of a structure on the stack.
 */
#define DIO_PAGES	64

/*
 * This code generally works in units of "dio_blocks".  A dio_block is
 * somewhere between the hard sector size and the filesystem block size.  it
 * is determined on a per-invocation basis.   When talking to the filesystem
 * we need to convert dio_blocks to fs_blocks by scaling the dio_block quantity
 * down by dio->blkfactor.  Similarly, fs-blocksize quantities are converted
 * to bio_block quantities by shifting left by blkfactor.
 *
 * If blkfactor is zero then the user's request was aligned to the filesystem's
 * blocksize.
 */

struct dio {
	/* BIO submission state */
	struct bio *bio;		/* bio under assembly */
	struct inode *inode;
	int rw;
	loff_t i_size;			/* i_size when submitted */
	int flags;			/* doesn't change */
	unsigned blkbits;		/* doesn't change */
	unsigned blkfactor;		/* When we're using an alignment which
					   is finer than the filesystem's soft
					   blocksize, this specifies how much
					   finer.  blkfactor=2 means 1/4-block
					   alignment.  Does not change */
	unsigned start_zero_done;	/* flag: sub-blocksize zeroing has
					   been performed at the start of a
					   write */
	int pages_in_io;		/* approximate total IO pages */
	size_t	size;			/* total request size (doesn't change)*/
	sector_t block_in_file;		/* Current offset into the underlying
					   file in dio_block units. */
	unsigned blocks_available;	/* At block_in_file.  changes */
	sector_t final_block_in_request;/* doesn't change */
	unsigned first_block_in_page;	/* doesn't change, Used only once */
	int boundary;			/* prev block is at a boundary */
	int reap_counter;		/* rate limit reaping */
	get_block_t *get_block;		/* block mapping function */
	dio_iodone_t *end_io;		/* IO completion function */
	sector_t final_block_in_bio;	/* current final block in bio + 1 */
	sector_t next_block_for_io;	/* next block to be put under IO,
					   in dio_blocks units */
	struct buffer_head map_bh;	/* last get_block() result */

	/*
	 * Deferred addition of a page to the dio.  These variables are
	 * private to dio_send_cur_page(), submit_page_section() and
	 * dio_bio_add_page().
	 */
	struct page *cur_page;		/* The page */
	unsigned cur_page_offset;	/* Offset into it, in bytes */
	unsigned cur_page_len;		/* Nr of bytes at cur_page_offset */
	sector_t cur_page_block;	/* Where it starts */

	/* BIO completion state */
	spinlock_t bio_lock;		/* protects BIO fields below */
	unsigned long refcount;		/* direct_io_worker() and bios */
	struct bio *bio_list;		/* singly linked via bi_private */
	struct task_struct *waiter;	/* waiting task (NULL if none) */

	/* AIO related stuff */
	struct kiocb *iocb;		/* kiocb */
	int is_async;			/* is IO async ? */
	int io_error;			/* IO error in completion path */
	ssize_t result;                 /* IO result */

	/*
	 * Page fetching state. These variables belong to dio_refill_pages().
	 */
	int curr_page;			/* changes */
	int total_pages;		/* doesn't change */
	unsigned long curr_user_address;/* changes */

	/*
	 * Page queue.  These variables belong to dio_refill_pages() and
	 * dio_get_page().
	 */
	unsigned head;			/* next page to process */
	unsigned tail;			/* last valid page + 1 */
	int page_errors;		/* errno from get_user_pages() */

	/*
	 * pages[] (and any fields placed after it) are not zeroed out at
	 * allocation time.  Don't add new fields after pages[] unless you
	 * wish that they not be zeroed.
	 */
	struct page *pages[DIO_PAGES];	/* page buffer */
};

/*
 * How many pages are in the queue?
 */
static inline unsigned dio_pages_present(struct dio *dio)
{
	return dio->tail - dio->head;
}

/*
 * Go grab and pin some userspace pages.   Typically we'll get 64 at a time.
 */
static int dio_refill_pages(struct dio *dio)
{
	int ret;
	int nr_pages;

	nr_pages = min(dio->total_pages - dio->curr_page, DIO_PAGES);
	ret = get_user_pages_fast(
		dio->curr_user_address,		/* Where from? */
		nr_pages,			/* How many pages? */
		dio->rw == READ,		/* Write to memory? */
		&dio->pages[0]);		/* Put results here */

	if (ret < 0 && dio->blocks_available && (dio->rw & WRITE)) {
		struct page *page = ZERO_PAGE(0);
		/*
		 * A memory fault, but the filesystem has some outstanding
		 * mapped blocks.  We need to use those blocks up to avoid
		 * leaking stale data in the file.
		 */
		if (dio->page_errors == 0)
			dio->page_errors = ret;
		page_cache_get(page);
		dio->pages[0] = page;
		dio->head = 0;
		dio->tail = 1;
		ret = 0;
		goto out;
	}

	if (ret >= 0) {
		dio->curr_user_address += ret * PAGE_SIZE;
		dio->curr_page += ret;
		dio->head = 0;
		dio->tail = ret;
		ret = 0;
	}
out:
	return ret;	
}

/*
 * Get another userspace page.  Returns an ERR_PTR on error.  Pages are
 * buffered inside the dio so that we can call get_user_pages() against a
 * decent number of pages, less frequently.  To provide nicer use of the
 * L1 cache.
 */
static struct page *dio_get_page(struct dio *dio)
{
	if (dio_pages_present(dio) == 0) {
		int ret;

		ret = dio_refill_pages(dio);
		if (ret)
			return ERR_PTR(ret);
		BUG_ON(dio_pages_present(dio) == 0);
	}
	return dio->pages[dio->head++];
}

/**
 * dio_complete() - called when all DIO BIO I/O has been completed
 * @offset: the byte offset in the file of the completed operation
 *
 * This releases locks as dictated by the locking type, lets interested parties
 * know that a DIO operation has completed, and calculates the resulting return
 * code for the operation.
 *
 * It lets the filesystem know if it registered an interest earlier via
 * get_block.  Pass the private field of the map buffer_head so that
 * filesystems can use it to hold additional state between get_block calls and
 * dio_complete.
 */
static int dio_complete(struct dio *dio, loff_t offset, int ret)
{
	ssize_t transferred = 0;

	/*
	 * AIO submission can race with bio completion to get here while
	 * expecting to have the last io completed by bio completion.
	 * In that case -EIOCBQUEUED is in fact not an error we want
	 * to preserve through this call.
	 */
	if (ret == -EIOCBQUEUED)
		ret = 0;

	if (dio->result) {
		transferred = dio->result;

		/* Check for short read case */
		if ((dio->rw == READ) && ((offset + transferred) > dio->i_size))
			transferred = dio->i_size - offset;
	}

	if (dio->end_io && dio->result)
		dio->end_io(dio->iocb, offset, transferred,
			    dio->map_bh.b_private);

	if (dio->flags & DIO_LOCKING)
		/* lockdep: non-owner release */
		up_read_non_owner(&dio->inode->i_alloc_sem);

	if (ret == 0)
		ret = dio->page_errors;
	if (ret == 0)
		ret = dio->io_error;
	if (ret == 0)
		ret = transferred;

	return ret;
}

static int dio_bio_complete(struct dio *dio, struct bio *bio);
/*
 * Asynchronous IO callback. 
 */
static void dio_bio_end_aio(struct bio *bio, int error)
{
	struct dio *dio = bio->bi_private;
	unsigned long remaining;
	unsigned long flags;

	/* cleanup the bio */
	dio_bio_complete(dio, bio);

	spin_lock_irqsave(&dio->bio_lock, flags);
	remaining = --dio->refcount;
	if (remaining == 1 && dio->waiter)
		wake_up_process(dio->waiter);
	spin_unlock_irqrestore(&dio->bio_lock, flags);

	if (remaining == 0) {
		int ret = dio_complete(dio, dio->iocb->ki_pos, 0);
		aio_complete(dio->iocb, ret, 0);
		kfree(dio);
	}
}

/*
 * The BIO completion handler simply queues the BIO up for the process-context
 * handler.
 *
 * During I/O bi_private points at the dio.  After I/O, bi_private is used to
 * implement a singly-linked list of completed BIOs, at dio->bio_list.
 */
static void dio_bio_end_io(struct bio *bio, int error)
{
	struct dio *dio = bio->bi_private;
	unsigned long flags;

	spin_lock_irqsave(&dio->bio_lock, flags);
	bio->bi_private = dio->bio_list;
	dio->bio_list = bio;
	if (--dio->refcount == 1 && dio->waiter)
		wake_up_process(dio->waiter);
	spin_unlock_irqrestore(&dio->bio_lock, flags);
}

static int
dio_bio_alloc(struct dio *dio, struct block_device *bdev,
		sector_t first_sector, int nr_vecs)
{
	struct bio *bio;

	bio = bio_alloc(GFP_KERNEL, nr_vecs);

	bio->bi_bdev = bdev;
	bio->bi_sector = first_sector;
	if (dio->is_async)
		bio->bi_end_io = dio_bio_end_aio;
	else
		bio->bi_end_io = dio_bio_end_io;

	dio->bio = bio;
	return 0;
}

/*
 * In the AIO read case we speculatively dirty the pages before starting IO.
 * During IO completion, any of these pages which happen to have been written
 * back will be redirtied by bio_check_pages_dirty().
 *
 * bios hold a dio reference between submit_bio and ->end_io.
 */
/*
 * 在异步IO（AIO）读取情况下，我们在启动IO之前推测性地将页面标记为脏。
 * 在IO完成期间，任何已经被写回的页面将通过 bio_check_pages_dirty() 再次被标记为脏。
 *
 * bios 在 submit_bio 和 ->end_io 之间持有一个 dio 引用。
 */
static void dio_bio_submit(struct dio *dio)
{
	// 获取 dio 结构中的 bio
	struct bio *bio = dio->bio;
	unsigned long flags;

	// 将 dio 结构设置为 bio 的私有数据
	bio->bi_private = dio;

	// 通过自旋锁保护 dio 结构的引用计数操作，防止并发修改
	spin_lock_irqsave(&dio->bio_lock, flags);
	dio->refcount++;	// 增加 dio 的引用计数
	// 解锁
	spin_unlock_irqrestore(&dio->bio_lock, flags);

	// 如果是异步读操作
	if (dio->is_async && dio->rw == READ)
		// 将 bio 中的所有页面标记为脏
		bio_set_pages_dirty(bio);

	// 提交 bio 到块设备层进行处理
	submit_bio(dio->rw, bio);

	// 清除 dio 结构中的 bio 指针，表示 bio 已提交
	dio->bio = NULL;
	// 重置边界标记
	dio->boundary = 0;
}

/*
 * Release any resources in case of a failure
 */
static void dio_cleanup(struct dio *dio)
{
	while (dio_pages_present(dio))
		page_cache_release(dio_get_page(dio));
}

/*
 * Wait for the next BIO to complete.  Remove it and return it.  NULL is
 * returned once all BIOs have been completed.  This must only be called once
 * all bios have been issued so that dio->refcount can only decrease.  This
 * requires that that the caller hold a reference on the dio.
 */
static struct bio *dio_await_one(struct dio *dio)
{
	unsigned long flags;
	struct bio *bio = NULL;

	spin_lock_irqsave(&dio->bio_lock, flags);

	/*
	 * Wait as long as the list is empty and there are bios in flight.  bio
	 * completion drops the count, maybe adds to the list, and wakes while
	 * holding the bio_lock so we don't need set_current_state()'s barrier
	 * and can call it after testing our condition.
	 */
	while (dio->refcount > 1 && dio->bio_list == NULL) {
		__set_current_state(TASK_UNINTERRUPTIBLE);
		dio->waiter = current;
		spin_unlock_irqrestore(&dio->bio_lock, flags);
		io_schedule();
		/* wake up sets us TASK_RUNNING */
		spin_lock_irqsave(&dio->bio_lock, flags);
		dio->waiter = NULL;
	}
	if (dio->bio_list) {
		bio = dio->bio_list;
		dio->bio_list = bio->bi_private;
	}
	spin_unlock_irqrestore(&dio->bio_lock, flags);
	return bio;
}

/*
 * Process one completed BIO.  No locks are held.
 */
static int dio_bio_complete(struct dio *dio, struct bio *bio)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct bio_vec *bvec = bio->bi_io_vec;
	int page_no;

	if (!uptodate)
		dio->io_error = -EIO;

	if (dio->is_async && dio->rw == READ) {
		bio_check_pages_dirty(bio);	/* transfers ownership */
	} else {
		for (page_no = 0; page_no < bio->bi_vcnt; page_no++) {
			struct page *page = bvec[page_no].bv_page;

			if (dio->rw == READ && !PageCompound(page))
				set_page_dirty_lock(page);
			page_cache_release(page);
		}
		bio_put(bio);
	}
	return uptodate ? 0 : -EIO;
}

/*
 * Wait on and process all in-flight BIOs.  This must only be called once
 * all bios have been issued so that the refcount can only decrease.
 * This just waits for all bios to make it through dio_bio_complete.  IO
 * errors are propagated through dio->io_error and should be propagated via
 * dio_complete().
 */
static void dio_await_completion(struct dio *dio)
{
	struct bio *bio;
	do {
		bio = dio_await_one(dio);
		if (bio)
			dio_bio_complete(dio, bio);
	} while (bio);
}

/*
 * A really large O_DIRECT read or write can generate a lot of BIOs.  So
 * to keep the memory consumption sane we periodically reap any completed BIOs
 * during the BIO generation phase.
 *
 * This also helps to limit the peak amount of pinned userspace memory.
 */
static int dio_bio_reap(struct dio *dio)
{
	int ret = 0;

	if (dio->reap_counter++ >= 64) {
		while (dio->bio_list) {
			unsigned long flags;
			struct bio *bio;
			int ret2;

			spin_lock_irqsave(&dio->bio_lock, flags);
			bio = dio->bio_list;
			dio->bio_list = bio->bi_private;
			spin_unlock_irqrestore(&dio->bio_lock, flags);
			ret2 = dio_bio_complete(dio, bio);
			if (ret == 0)
				ret = ret2;
		}
		dio->reap_counter = 0;
	}
	return ret;
}

/*
 * Call into the fs to map some more disk blocks.  We record the current number
 * of available blocks at dio->blocks_available.  These are in units of the
 * fs blocksize, (1 << inode->i_blkbits).
 *
 * The fs is allowed to map lots of blocks at once.  If it wants to do that,
 * it uses the passed inode-relative block number as the file offset, as usual.
 *
 * get_block() is passed the number of i_blkbits-sized blocks which direct_io
 * has remaining to do.  The fs should not map more than this number of blocks.
 *
 * If the fs has mapped a lot of blocks, it should populate bh->b_size to
 * indicate how much contiguous disk space has been made available at
 * bh->b_blocknr.
 *
 * If *any* of the mapped blocks are new, then the fs must set buffer_new().
 * This isn't very efficient...
 *
 * In the case of filesystem holes: the fs may return an arbitrarily-large
 * hole by returning an appropriate value in b_size and by clearing
 * buffer_mapped().  However the direct-io code will only process holes one
 * block at a time - it will repeatedly call get_block() as it walks the hole.
 */
static int get_more_blocks(struct dio *dio)
{
	int ret;
	struct buffer_head *map_bh = &dio->map_bh;
	sector_t fs_startblk;	/* Into file, in filesystem-sized blocks */
	unsigned long fs_count;	/* Number of filesystem-sized blocks */
	unsigned long dio_count;/* Number of dio_block-sized blocks */
	unsigned long blkmask;
	int create;

	/*
	 * If there was a memory error and we've overwritten all the
	 * mapped blocks then we can now return that memory error
	 */
	ret = dio->page_errors;
	if (ret == 0) {
		BUG_ON(dio->block_in_file >= dio->final_block_in_request);
		fs_startblk = dio->block_in_file >> dio->blkfactor;
		dio_count = dio->final_block_in_request - dio->block_in_file;
		fs_count = dio_count >> dio->blkfactor;
		blkmask = (1 << dio->blkfactor) - 1;
		if (dio_count & blkmask)	
			fs_count++;

		map_bh->b_state = 0;
		map_bh->b_size = fs_count << dio->inode->i_blkbits;

		/*
		 * For writes inside i_size on a DIO_SKIP_HOLES filesystem we
		 * forbid block creations: only overwrites are permitted.
		 * We will return early to the caller once we see an
		 * unmapped buffer head returned, and the caller will fall
		 * back to buffered I/O.
		 *
		 * Otherwise the decision is left to the get_blocks method,
		 * which may decide to handle it or also return an unmapped
		 * buffer head.
		 */
		create = dio->rw & WRITE;
		if (dio->flags & DIO_SKIP_HOLES) {
			if (dio->block_in_file < (i_size_read(dio->inode) >>
							dio->blkbits))
				create = 0;
		}

		ret = (*dio->get_block)(dio->inode, fs_startblk,
						map_bh, create);
	}
	return ret;
}

/*
 * There is no bio.  Make one now.
 */
static int dio_new_bio(struct dio *dio, sector_t start_sector)
{
	sector_t sector;
	int ret, nr_pages;

	ret = dio_bio_reap(dio);
	if (ret)
		goto out;
	sector = start_sector << (dio->blkbits - 9);
	nr_pages = min(dio->pages_in_io, bio_get_nr_vecs(dio->map_bh.b_bdev));
	BUG_ON(nr_pages <= 0);
	ret = dio_bio_alloc(dio, dio->map_bh.b_bdev, sector, nr_pages);
	dio->boundary = 0;
out:
	return ret;
}

/*
 * Attempt to put the current chunk of 'cur_page' into the current BIO.  If
 * that was successful then update final_block_in_bio and take a ref against
 * the just-added page.
 *
 * Return zero on success.  Non-zero means the caller needs to start a new BIO.
 */
static int dio_bio_add_page(struct dio *dio)
{
	int ret;

	ret = bio_add_page(dio->bio, dio->cur_page,
			dio->cur_page_len, dio->cur_page_offset);
	if (ret == dio->cur_page_len) {
		/*
		 * Decrement count only, if we are done with this page
		 */
		if ((dio->cur_page_len + dio->cur_page_offset) == PAGE_SIZE)
			dio->pages_in_io--;
		page_cache_get(dio->cur_page);
		dio->final_block_in_bio = dio->cur_page_block +
			(dio->cur_page_len >> dio->blkbits);
		ret = 0;
	} else {
		ret = 1;
	}
	return ret;
}
		
/*
 * Put cur_page under IO.  The section of cur_page which is described by
 * cur_page_offset,cur_page_len is put into a BIO.  The section of cur_page
 * starts on-disk at cur_page_block.
 *
 * We take a ref against the page here (on behalf of its presence in the bio).
 *
 * The caller of this function is responsible for removing cur_page from the
 * dio, and for dropping the refcount which came from that presence.
 */
/*
 * 将 cur_page 提交到 I/O。cur_page 中由 cur_page_offset 和 cur_page_len 描述的部分
 * 被放入一个 BIO 中。cur_page 的这一部分在磁盘上开始于 cur_page_block。
 *
 * 我们在此为页面的 BIO 存在增加一个引用计数。
 *
 * 调用此函数的代码负责从 dio 中移除 cur_page，并且负责减少由于此存在而增加的引用计数。
 */
static int dio_send_cur_page(struct dio *dio)
{
	int ret = 0;

	if (dio->bio) {
		/*
		 * See whether this new request is contiguous with the old
		 */
		/*
		 * 检查这个新请求是否与旧的请求连续
		 * 如果当前 BIO 存在，并且新请求的块与上一个 BIO 的最后一个块不连续
		 */
		if (dio->final_block_in_bio != dio->cur_page_block)
			dio_bio_submit(dio);	// 提交当前 BIO
		/*
		 * Submit now if the underlying fs is about to perform a
		 * metadata read
		 */
		/*
		 * 如果底层文件系统即将执行元数据读取，则立即提交
		 * 当需要处理文件系统边界（如分区边界）时，立即提交 BIO
		 */
		if (dio->boundary)
			dio_bio_submit(dio);
	}

	if (dio->bio == NULL) {
		// 如果当前没有活跃的 BIO，创建一个新的
		ret = dio_new_bio(dio, dio->cur_page_block);
		if (ret)	// 如果创建 BIO 失败，直接跳转到结束处理
			goto out;
	}

	if (dio_bio_add_page(dio) != 0) {
		// 尝试将当前页添加到 BIO，如果失败（例如，BIO 已满），则提交当前 BIO
		dio_bio_submit(dio);
		// 之后，创建一个新的 BIO 并尝试再次添加页面
		ret = dio_new_bio(dio, dio->cur_page_block);
		if (ret == 0) {
			ret = dio_bio_add_page(dio);
			BUG_ON(ret != 0);	// 断言添加页面成功
		}
	}
out:
	return ret;	// 返回操作结果
}

/*
 * An autonomous function to put a chunk of a page under deferred IO.
 *
 * The caller doesn't actually know (or care) whether this piece of page is in
 * a BIO, or is under IO or whatever.  We just take care of all possible 
 * situations here.  The separation between the logic of do_direct_IO() and
 * that of submit_page_section() is important for clarity.  Please don't break.
 *
 * The chunk of page starts on-disk at blocknr.
 *
 * We perform deferred IO, by recording the last-submitted page inside our
 * private part of the dio structure.  If possible, we just expand the IO
 * across that page here.
 *
 * If that doesn't work out then we put the old page into the bio and add this
 * page to the dio instead.
 */
/*
 * 一个独立的函数，用于将页面的一部分放入延迟IO处理。
 *
 * 调用者实际上不知道（或不关心）这部分页面是否在BIO中，是否正在进行IO，或其他情况。
 * 我们在这里处理所有可能的情况。do_direct_IO()的逻辑和submit_page_section()的逻辑
 * 之间的区分对于保持代码清晰非常重要。请不要打破这种分隔。
 *
 * 页面的这一部分从磁盘上的blocknr开始。
 *
 * 我们通过在dio结构的私有部分记录最后提交的页面来执行延迟IO。如果可能的话，我们只是
 * 在这里扩展跨越该页面的IO。
 *
 * 如果这样做不可行，那么我们将旧页面放入bio，并将这个页面添加到dio中。
 */
static int
submit_page_section(struct dio *dio, struct page *page,
		unsigned offset, unsigned len, sector_t blocknr)
{
	int ret = 0;

	if (dio->rw & WRITE) {
		/*
		 * Read accounting is performed in submit_bio()
		 */
		/*
		 * 读取统计在submit_bio()中完成
		 * 记录写入的字节，用于I/O统计
		 */
		task_io_account_write(len);
	}

	/*
	 * Can we just grow the current page's presence in the dio?
	 */
	/*
	 * 我们可以简单地增加当前页面在dio中的存在吗？
	 */
		/*
	 	 * 我们可以简单地增加当前页面在dio中的存在吗？
	 	 * 检查是否可以将当前页面的部分简单地追加到已存在的dio中。
	 	 */
	if (	(dio->cur_page == page) &&
		(dio->cur_page_offset + dio->cur_page_len == offset) &&
		(dio->cur_page_block +
			(dio->cur_page_len >> dio->blkbits) == blocknr)) {
		dio->cur_page_len += len;

		/*
		 * If dio->boundary then we want to schedule the IO now to
		 * avoid metadata seeks.
		 */
		/*
		 * 如果dio->boundary为真，则我们想要立即调度IO，
		 * 以避免元数据搜索。
		 */
		/*
		 * 如果dio->boundary为真，则我们想要立即调度IO，
		 * 以避免元数据搜索。
		 * 如果设置了边界标志，立即发送当前页以避免元数据操作引起的延迟。
		 */
		if (dio->boundary) {
			ret = dio_send_cur_page(dio);
			page_cache_release(dio->cur_page);
			dio->cur_page = NULL;
		}
		goto out;
	}

	/*
	 * If there's a deferred page already there then send it.
	 */
	/*
	 * 如果有已经延迟的页面存在，则发送它。
	 */
	/*
	 * 如果有已经延迟的页面存在，则发送它。
	 * 如果已经有一个延迟处理的页面存在，则发送它。
	 */
	if (dio->cur_page) {
		ret = dio_send_cur_page(dio);
		page_cache_release(dio->cur_page);
		dio->cur_page = NULL;
		if (ret)
			goto out;
	}

	/* 它在dio中 */
	/* 为了dio，增加页面的引用计数 */
	page_cache_get(page);		/* It is in dio */
	/* 设置当前处理的页面 */
	dio->cur_page = page;
	/* 设置当前页面的偏移 */
	dio->cur_page_offset = offset;
	/* 设置处理的长度 */
	dio->cur_page_len = len;
	/* 设置起始块号 */
	dio->cur_page_block = blocknr;
out:
	return ret;	/* 返回处理结果 */
}

/*
 * Clean any dirty buffers in the blockdev mapping which alias newly-created
 * file blocks.  Only called for S_ISREG files - blockdevs do not set
 * buffer_new
 */
static void clean_blockdev_aliases(struct dio *dio)
{
	unsigned i;
	unsigned nblocks;

	nblocks = dio->map_bh.b_size >> dio->inode->i_blkbits;

	for (i = 0; i < nblocks; i++) {
		unmap_underlying_metadata(dio->map_bh.b_bdev,
					dio->map_bh.b_blocknr + i);
	}
}

/*
 * If we are not writing the entire block and get_block() allocated
 * the block for us, we need to fill-in the unused portion of the
 * block with zeros. This happens only if user-buffer, fileoffset or
 * io length is not filesystem block-size multiple.
 *
 * `end' is zero if we're doing the start of the IO, 1 at the end of the
 * IO.
 */
static void dio_zero_block(struct dio *dio, int end)
{
	unsigned dio_blocks_per_fs_block;
	unsigned this_chunk_blocks;	/* In dio_blocks */
	unsigned this_chunk_bytes;
	struct page *page;

	dio->start_zero_done = 1;
	if (!dio->blkfactor || !buffer_new(&dio->map_bh))
		return;

	dio_blocks_per_fs_block = 1 << dio->blkfactor;
	this_chunk_blocks = dio->block_in_file & (dio_blocks_per_fs_block - 1);

	if (!this_chunk_blocks)
		return;

	/*
	 * We need to zero out part of an fs block.  It is either at the
	 * beginning or the end of the fs block.
	 */
	if (end) 
		this_chunk_blocks = dio_blocks_per_fs_block - this_chunk_blocks;

	this_chunk_bytes = this_chunk_blocks << dio->blkbits;

	page = ZERO_PAGE(0);
	if (submit_page_section(dio, page, 0, this_chunk_bytes, 
				dio->next_block_for_io))
		return;

	dio->next_block_for_io += this_chunk_blocks;
}

/*
 * Walk the user pages, and the file, mapping blocks to disk and generating
 * a sequence of (page,offset,len,block) mappings.  These mappings are injected
 * into submit_page_section(), which takes care of the next stage of submission
 *
 * Direct IO against a blockdev is different from a file.  Because we can
 * happily perform page-sized but 512-byte aligned IOs.  It is important that
 * blockdev IO be able to have fine alignment and large sizes.
 *
 * So what we do is to permit the ->get_block function to populate bh.b_size
 * with the size of IO which is permitted at this offset and this i_blkbits.
 *
 * For best results, the blockdev should be set up with 512-byte i_blkbits and
 * it should set b_size to PAGE_SIZE or more inside get_block().  This gives
 * fine alignment but still allows this function to work in PAGE_SIZE units.
 */
/*
 * 遍历用户页和文件，将块映射到磁盘并生成一系列(page, offset, len, block)映射。
 * 这些映射被注入到submit_page_section()，后者负责提交的下一个阶段。
 *
 * 直接对块设备的IO与对文件的IO不同。因为我们可以愉快地执行页面大小但512字节对齐的IO。
 * 重要的是块设备IO能够有细粒度的对齐和大尺寸。
 *
 * 所以我们所做的是允许->get_block函数填充bh.b_size
 * 用在这个偏移和这个i_blkbits允许的IO大小。
 *
 * 为了最佳效果，块设备应该以512字节的i_blkbits设置，并且
 * 它应该在get_block()中将b_size设置为PAGE_SIZE或更大。
 * 这提供了精细的对齐，但仍允许这个函数以PAGE_SIZE单位工作。
 */
static int do_direct_IO(struct dio *dio)
{
	// 块大小（位数）
	const unsigned blkbits = dio->blkbits;
	// 每页中的块数
	const unsigned blocks_per_page = PAGE_SIZE >> blkbits;
	// 用于操作的页面指针
	struct page *page;
	// 页面中的当前块索引
	unsigned block_in_page;
	// 映射块头
	struct buffer_head *map_bh = &dio->map_bh;
	int ret = 0;	// 返回值

	/* The I/O can start at any block offset within the first page */
	/* I/O可以在第一页的任何块偏移处开始 */
	// 设置起始块偏移
	block_in_page = dio->first_block_in_page;

	// 遍历所有请求的块
	while (dio->block_in_file < dio->final_block_in_request) {
		page = dio_get_page(dio);	// 获取当前操作的页
		if (IS_ERR(page)) {	// 检查页面获取是否出错
			// 设置错误码
			ret = PTR_ERR(page);
			goto out;	// 跳出循环处理
		}

		// 遍历页内的块
		while (block_in_page < blocks_per_page) {
			// 计算块在页内的偏移
			unsigned offset_in_page = block_in_page << blkbits;
			/* 映射的字节数 */
			unsigned this_chunk_bytes;	/* # of bytes mapped */
			/* 映射的块数 */
			unsigned this_chunk_blocks;	/* # of blocks */
			unsigned u;

			if (dio->blocks_available == 0) {	// 检查是否需要映射更多的磁盘块
				/*
				 * Need to go and map some more disk
				 */
				/*
				 * 需要去映射更多磁盘
				 */
				unsigned long blkmask;	 // 块掩码
				unsigned long dio_remainder;	// 剩余块数

				// 获取更多的块
				ret = get_more_blocks(dio);
				if (ret) {	// 检查是否成功
					page_cache_release(page);	// 释放页面
					goto out;	// 跳出循环处理
				}
				if (!buffer_mapped(map_bh))	// 检查是否有映射
					goto do_holes;	// 处理空洞

				// 更新可用块数和下一个I/O块
				dio->blocks_available =
						map_bh->b_size >> dio->blkbits;
				dio->next_block_for_io =
					map_bh->b_blocknr << dio->blkfactor;
				if (buffer_new(map_bh))	// 如果是新映射的块
					clean_blockdev_aliases(dio);	// 清理块设备别名

				// 处理块对齐
				if (!dio->blkfactor)	// 如果没有块因子
					goto do_holes;	// 处理空洞

				// 计算块掩码和剩余块数
				blkmask = (1 << dio->blkfactor) - 1;
				dio_remainder = (dio->block_in_file & blkmask);

				/*
				 * If we are at the start of IO and that IO
				 * starts partway into a fs-block,
				 * dio_remainder will be non-zero.  If the IO
				 * is a read then we can simply advance the IO
				 * cursor to the first block which is to be
				 * read.  But if the IO is a write and the
				 * block was newly allocated we cannot do that;
				 * the start of the fs block must be zeroed out
				 * on-disk
				 */
				/*
				 * 如果我们在IO开始时处于IO的中间，并且该IO部分地进入了一个fs-block,
				 * dio_remainder将是非零的。如果IO是读操作，我们可以简单地将IO光标前进到
				 * 要读取的第一个块。但如果IO是写操作，并且块是新分配的，我们不能这样做；
				 * 必须在磁盘上清零fs块的开始。
				 */
				// 处理部分块和新块的情况
				if (!buffer_new(map_bh))
					dio->next_block_for_io += dio_remainder;
				// 块数调整
				dio->blocks_available -= dio_remainder;
			}
do_holes:
			/* Handle holes */
			/* 处理空洞 */
			// 处理文件中的空洞（未映射的区域）
			if (!buffer_mapped(map_bh)) {
				loff_t i_size_aligned;

				/* AKPM: eargh, -ENOTBLK is a hack */
				// 如果是写操作，但处理空洞不可行
				if (dio->rw & WRITE) {
					page_cache_release(page);
					return -ENOTBLK;
				}

				/*
				 * Be sure to account for a partial block as the
				 * last block in the file
				 */
				/*
				 * 一定要考虑文件中最后一个块的部分块
				 */
				// 确保即使是文件的最后一个部分块也被处理
				i_size_aligned = ALIGN(i_size_read(dio->inode),
							1 << blkbits);
				// 如果超过文件大小，表示我们已经到达文件末尾
				if (dio->block_in_file >=
						i_size_aligned >> blkbits) {
					/* We hit eof */
					/* 我们到达了文件末尾 */
					page_cache_release(page);
					goto out;
				}
				// 将用户页中对应的块区域置零
				zero_user(page, block_in_page << blkbits,
						1 << blkbits);
				dio->block_in_file++;
				block_in_page++;
				goto next_block;
			}

			/*
			 * If we're performing IO which has an alignment which
			 * is finer than the underlying fs, go check to see if
			 * we must zero out the start of this block.
			 */
			/*
			 * 如果我们正在执行精度比底层文件系统更高的IO，检查是否需要
			 * 清零这个块的开始。
			 */
			// 检查块设备的精细对齐问题，必要时清零块的开始部分
			if (unlikely(dio->blkfactor && !dio->start_zero_done))
				dio_zero_block(dio, 0);

			/*
			 * Work out, in this_chunk_blocks, how much disk we
			 * can add to this page
			 */
			/*
			 * 计算这个块在这一页中能添加多少磁盘
			 */
			// 计算这次可以处理的块数和字节
			this_chunk_blocks = dio->blocks_available;
			u = (PAGE_SIZE - offset_in_page) >> blkbits;
			if (this_chunk_blocks > u)
				this_chunk_blocks = u;
			u = dio->final_block_in_request - dio->block_in_file;
			if (this_chunk_blocks > u)
				this_chunk_blocks = u;
			this_chunk_bytes = this_chunk_blocks << blkbits;
			BUG_ON(this_chunk_bytes == 0);

			// 设置边界条件
			dio->boundary = buffer_boundary(map_bh);
			/**
			 * 遍历用户页和文件，将块映射到磁盘并生成一系列(page, offset, len, block)映射。
			 * 这些映射被注入到submit_page_section()，后者负责提交的下一个阶段。
			 */
			// 提交页中的部分区段到块设备
			ret = submit_page_section(dio, page, offset_in_page,
				this_chunk_bytes, dio->next_block_for_io);
			if (ret) {
				page_cache_release(page);
				goto out;
			}
			// 更新计数器和块位置
			dio->next_block_for_io += this_chunk_blocks;

			dio->block_in_file += this_chunk_blocks;
			block_in_page += this_chunk_blocks;
			dio->blocks_available -= this_chunk_blocks;
next_block:
			BUG_ON(dio->block_in_file > dio->final_block_in_request);
			// 如果处理完所有请求的块，结束循环
			if (dio->block_in_file == dio->final_block_in_request)
				break;
		}

		/* Drop the ref which was taken in get_user_pages() */
		/* 释放在get_user_pages()中取得的引用 */
		page_cache_release(page);
		block_in_page = 0;
	}
out:
	return ret;
}

/*
 * Releases both i_mutex and i_alloc_sem
 */
/*
 * 释放 i_mutex 和 i_alloc_sem
 */
static ssize_t
direct_io_worker(int rw, struct kiocb *iocb, struct inode *inode, 
	const struct iovec *iov, loff_t offset, unsigned long nr_segs, 
	unsigned blkbits, get_block_t get_block, dio_iodone_t end_io,
	struct dio *dio)
{
	unsigned long user_addr; 	// 用户空间地址
	unsigned long flags;			// 用于保存中断状态
	int seg;	// 段计数
	ssize_t ret = 0;	// 返回值初始化
	ssize_t ret2;			// 第二个返回值
	size_t bytes;			// 字节数

	/* 初始化 dio 结构体 */
	dio->inode = inode;
	dio->rw = rw;
	dio->blkbits = blkbits;
	dio->blkfactor = inode->i_blkbits - blkbits;
	dio->block_in_file = offset >> blkbits;

	dio->get_block = get_block;
	dio->end_io = end_io;
	dio->final_block_in_bio = -1;
	dio->next_block_for_io = -1;

	dio->iocb = iocb;
	dio->i_size = i_size_read(inode);

	// 初始化 dio 的自旋锁
	spin_lock_init(&dio->bio_lock);
	dio->refcount = 1;	// 初始化引用计数

	/*
	 * In case of non-aligned buffers, we may need 2 more
	 * pages since we need to zero out first and last block.
	 */
	/*
	 * 对于非对齐缓冲区，我们可能需要两个额外的页，
	 * 因为我们需要清零第一个和最后一个块。
	 */
	if (unlikely(dio->blkfactor))
		dio->pages_in_io = 2;

	for (seg = 0; seg < nr_segs; seg++) {
		user_addr = (unsigned long)iov[seg].iov_base;
		dio->pages_in_io +=
			((user_addr+iov[seg].iov_len +PAGE_SIZE-1)/PAGE_SIZE
				- user_addr/PAGE_SIZE);
	}

	for (seg = 0; seg < nr_segs; seg++) {
		user_addr = (unsigned long)iov[seg].iov_base;
		dio->size += bytes = iov[seg].iov_len;

		/* Index into the first page of the first block */
		/* 计算第一个页内的第一个块的索引 */
		dio->first_block_in_page = (user_addr & ~PAGE_MASK) >> blkbits;
		dio->final_block_in_request = dio->block_in_file +
						(bytes >> blkbits);
		/* Page fetching state */
		/* 页面获取状态 */
		dio->head = 0;
		dio->tail = 0;
		dio->curr_page = 0;

		dio->total_pages = 0;
		if (user_addr & (PAGE_SIZE-1)) {
			dio->total_pages++;
			bytes -= PAGE_SIZE - (user_addr & (PAGE_SIZE - 1));
		}
		dio->total_pages += (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
		dio->curr_user_address = user_addr;
		
		// 执行直接 IO
		ret = do_direct_IO(dio);

		dio->result += iov[seg].iov_len -
			((dio->final_block_in_request - dio->block_in_file) <<
					blkbits);

		if (ret) {
			dio_cleanup(dio);	// 清理 dio
			break;
		}
	} /* end iovec loop */	/* 结束 iovec 循环 */

	if (ret == -ENOTBLK && (rw & WRITE)) {
		/*
		 * The remaining part of the request will be
		 * be handled by buffered I/O when we return
		 */
		/*
		 * 剩余请求的部分将由缓冲 I/O 处理
		 */
		ret = 0;
	}
	/*
	 * There may be some unwritten disk at the end of a part-written
	 * fs-block-sized block.  Go zero that now.
	 */
	/*
	 * 可能在部分写入的块的末尾存在未写入的磁盘。现在去清零它。
	 */
	dio_zero_block(dio, 1);

	if (dio->cur_page) {
		ret2 = dio_send_cur_page(dio);
		if (ret == 0)
			ret = ret2;
		page_cache_release(dio->cur_page);
		dio->cur_page = NULL;
	}
	// 提交 bio
	if (dio->bio)
		dio_bio_submit(dio);

	/*
	 * It is possible that, we return short IO due to end of file.
	 * In that case, we need to release all the pages we got hold on.
	 */
	/*
	 * 由于文件末尾可能导致返回短 IO。在这种情况下，我们需要释放我们持有的所有页面。
	 */
	dio_cleanup(dio);

	/*
	 * All block lookups have been performed. For READ requests
	 * we can let i_mutex go now that its achieved its purpose
	 * of protecting us from looking up uninitialized blocks.
	 */
	/*
	 * 所有块查找已完成。对于 READ 请求，现在可以释放 i_mutex 以保护我们不查看未初始化的块。
	 */
	if (rw == READ && (dio->flags & DIO_LOCKING))
		mutex_unlock(&dio->inode->i_mutex);

	/*
	 * The only time we want to leave bios in flight is when a successful
	 * partial aio read or full aio write have been setup.  In that case
	 * bio completion will call aio_complete.  The only time it's safe to
	 * call aio_complete is when we return -EIOCBQUEUED, so we key on that.
	 * This had *better* be the only place that raises -EIOCBQUEUED.
	 */
	/*
	 * 我们希望保留飞行中的 bios，只有当成功设置部分 aio 读取或完整 aio 写入时。
	 * 如果所有的 bios 都在我们到达这里之前完成，那么在这种情况下，dio_complete() 将
	 * -EIOCBQUEUED 转换为调用者将交给 aio_complete() 的适当返回码。
	 */
	BUG_ON(ret == -EIOCBQUEUED);
	if (dio->is_async && ret == 0 && dio->result &&
	    ((rw & READ) || (dio->result == dio->size)))
		ret = -EIOCBQUEUED;

	if (ret != -EIOCBQUEUED) {
		/* All IO is now issued, send it on its way */
		/* 所有 IO 都已发出，让它开始执行 */
		blk_run_address_space(inode->i_mapping);
		dio_await_completion(dio);	// 等待完成
	}

	/*
	 * Sync will always be dropping the final ref and completing the
	 * operation.  AIO can if it was a broken operation described above or
	 * in fact if all the bios race to complete before we get here.  In
	 * that case dio_complete() translates the EIOCBQUEUED into the proper
	 * return code that the caller will hand to aio_complete().
	 *
	 * This is managed by the bio_lock instead of being an atomic_t so that
	 * completion paths can drop their ref and use the remaining count to
	 * decide to wake the submission path atomically.
	 */
	/*
	 * 同步总是会放下最后一个引用并完成操作。AIO 可以在上述描述的破碱操作中或实际上
	 * 如果所有 bios 都在到达这里之前赛跑完成。在这种情况下，dio_complete() 将
	 * -EIOCBQUEUED 转换为调用者将交给 aio_complete() 的正确返回码。
	 * 
	 * 这是通过 bio_lock 管理而不是 atomic_t，以便完成路径可以放下它们的引用并使用
	 * 剩余的计数来决定是否唤醒提交路径。
	 */
	spin_lock_irqsave(&dio->bio_lock, flags);
	ret2 = --dio->refcount;
	spin_unlock_irqrestore(&dio->bio_lock, flags);

	if (ret2 == 0) {
		// 完成处理
		ret = dio_complete(dio, offset, ret);
		kfree(dio);	// 释放 dio
	} else
		BUG_ON(ret != -EIOCBQUEUED);

	return ret;
}

/*
 * This is a library function for use by filesystem drivers.
 *
 * The locking rules are governed by the flags parameter:
 *  - if the flags value contains DIO_LOCKING we use a fancy locking
 *    scheme for dumb filesystems.
 *    For writes this function is called under i_mutex and returns with
 *    i_mutex held, for reads, i_mutex is not held on entry, but it is
 *    taken and dropped again before returning.
 *    For reads and writes i_alloc_sem is taken in shared mode and released
 *    on I/O completion (which may happen asynchronously after returning to
 *    the caller).
 *
 *  - if the flags value does NOT contain DIO_LOCKING we don't use any
 *    internal locking but rather rely on the filesystem to synchronize
 *    direct I/O reads/writes versus each other and truncate.
 *    For reads and writes both i_mutex and i_alloc_sem are not held on
 *    entry and are never taken.
 */
/*
 * 这是一个给文件系统驱动使用的库函数。
 *
 * 锁定规则由flags参数控制：
 *  - 如果flags值包含DIO_LOCKING，我们对不太聪明的文件系统使用一个复杂的锁定方案。
 *    对于写操作，此函数在持有i_mutex的情况下调用，并在返回时保持i_mutex，
 *    对于读操作，进入时不持有i_mutex，但在返回前会取得并再次释放。
 *    对于读写操作，i_alloc_sem在I/O完成时（可能在返回给调用者后异步发生）释放。
 *
 *  - 如果flags值不包含DIO_LOCKING，我们不使用任何内部锁定，而是依赖文件系统来同步
 *    直接I/O读写操作和截断。
 *    对于读写操作，进入时不会持有i_mutex和i_alloc_sem，也永远不会获取。
 */
/*
 * __blockdev_direct_IO - 直接IO操作的函数实现
 * @rw: 读写标志
 * @iocb: IO控制块，包含文件信息和位置
 * @inode: inode对象，包含文件的元数据
 * @bdev: 块设备对象
 * @iov: 包含待写入数据的向量
 * @offset: 文件中的偏移量
 * @nr_segs: 向量中的段数
 * @get_block: 回调函数，用于获取块的位置
 * @end_io: 结束时的回调函数
 * @flags: 控制锁定行为的标志
 *
 * 本函数用于块设备的直接IO操作。如果设置了DIO_LOCKING标志，它会使用特殊的锁定机制，
 * 对于写操作，在调用前需要持有i_mutex锁，并在返回时保持锁定；对于读操作，调用前不持有i_mutex锁，
 * 但会在执行期间临时加锁并在返回前释放。对于直接IO的读写操作，i_alloc_sem在IO完成时释放（可能是异步的）。
 *
 * 如果未设置DIO_LOCKING标志，不使用内部锁定，依赖于文件系统来同步直接IO读写操作和截断操作。
 * 读写操作不会持有i_mutex和i_alloc_sem锁。
 */
ssize_t
__blockdev_direct_IO(int rw, struct kiocb *iocb, struct inode *inode,
	struct block_device *bdev, const struct iovec *iov, loff_t offset, 
	unsigned long nr_segs, get_block_t get_block, dio_iodone_t end_io,
	int flags)
{
	int seg;
	size_t size;
	unsigned long addr;
	unsigned blkbits = inode->i_blkbits;	// 块大小（位数）
	unsigned bdev_blkbits = 0;
	unsigned blocksize_mask = (1 << blkbits) - 1;
	ssize_t retval = -EINVAL;
	loff_t end = offset;
	struct dio *dio;

	if (rw & WRITE)
		// 设置直写模式的写操作
		rw = WRITE_ODIRECT_PLUG;

	if (bdev)
		// 获取设备块大小
		bdev_blkbits = blksize_bits(bdev_logical_block_size(bdev));

	// 检查偏移是否对齐
	if (offset & blocksize_mask) {
		if (bdev)
			 blkbits = bdev_blkbits;
		blocksize_mask = (1 << blkbits) - 1;
		// 如果未对齐，直接退出
		if (offset & blocksize_mask)
			goto out;
	}

	/* Check the memory alignment.  Blocks cannot straddle pages */
	// 检查内存对齐，块不应跨越页面
	for (seg = 0; seg < nr_segs; seg++) {
		addr = (unsigned long)iov[seg].iov_base;
		size = iov[seg].iov_len;
		end += size;
		if ((addr & blocksize_mask) || (size & blocksize_mask))  {
			if (bdev)
				 blkbits = bdev_blkbits;
			blocksize_mask = (1 << blkbits) - 1;
			if ((addr & blocksize_mask) || (size & blocksize_mask))  
				goto out;
		}
	}

	// 分配直接IO结构体
	dio = kmalloc(sizeof(*dio), GFP_KERNEL);
	retval = -ENOMEM;
	if (!dio)
		goto out;
	/*
	 * Believe it or not, zeroing out the page array caused a .5%
	 * performance regression in a database benchmark.  So, we take
	 * care to only zero out what's needed.
	 */
	// 初始化dio结构
	memset(dio, 0, offsetof(struct dio, pages));

	dio->flags = flags;
	// 处理DIO_LOCKING标志
	if (dio->flags & DIO_LOCKING) {
		/* watch out for a 0 len io from a tricksy fs */
		if (rw == READ && end > offset) {
			struct address_space *mapping =
					iocb->ki_filp->f_mapping;

			/* will be released by direct_io_worker */
			mutex_lock(&inode->i_mutex);	// 加锁

			// 执行写操作贝宁等写完成
			retval = filemap_write_and_wait_range(mapping, offset,
							      end - 1);
			if (retval) {
				mutex_unlock(&inode->i_mutex);	// 解锁
				kfree(dio);
				goto out;
			}
		}

		/*
		 * Will be released at I/O completion, possibly in a
		 * different thread.
		 */
		// 在 I/O 完成时释放，可能在不同的线程中
		// 加读锁
		down_read_non_owner(&inode->i_alloc_sem);
	}

	/*
	 * For file extending writes updating i_size before data
	 * writeouts complete can expose uninitialized blocks. So
	 * even for AIO, we need to wait for i/o to complete before
	 * returning in this case.
	 */
	dio->is_async = !is_sync_kiocb(iocb) && !((rw & WRITE) &&
		(end > i_size_read(inode)));

	// 执行直接IO操作
	retval = direct_io_worker(rw, iocb, inode, iov, offset,
				nr_segs, blkbits, get_block, end_io, dio);

	/*
	 * In case of error extending write may have instantiated a few
	 * blocks outside i_size. Trim these off again for DIO_LOCKING.
	 *
	 * NOTE: filesystems with their own locking have to handle this
	 * on their own.
	 */
	if (flags & DIO_LOCKING) {
		if (unlikely((rw & WRITE) && retval < 0)) {
			loff_t isize = i_size_read(inode);
			if (end > isize)
			  // 调整文件大小
				vmtruncate(inode, isize);
		}
	}

out:
	return retval;	// 返回处理结果
}
EXPORT_SYMBOL(__blockdev_direct_IO);
