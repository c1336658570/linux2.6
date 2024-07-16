/*
 *  linux/fs/buffer.c
 *
 *  Copyright (C) 1991, 1992, 2002  Linus Torvalds
 */

/*
 * Start bdflush() with kernel_thread not syscall - Paul Gortmaker, 12/95
 *
 * Removed a lot of unnecessary code and simplified things now that
 * the buffer cache isn't our primary cache - Andrew Tridgell 12/96
 *
 * Speed up hash, lru, and free list operations.  Use gfp() for allocating
 * hash table, use SLAB cache for buffer heads. SMP threading.  -DaveM
 *
 * Added 32k buffer block sizes - these are required older ARM systems. - RMK
 *
 * async buffer flushing, 1999 Andrea Arcangeli <andrea@suse.de>
 */

#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/capability.h>
#include <linux/blkdev.h>
#include <linux/file.h>
#include <linux/quotaops.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/writeback.h>
#include <linux/hash.h>
#include <linux/suspend.h>
#include <linux/buffer_head.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/bio.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/bitops.h>
#include <linux/mpage.h>
#include <linux/bit_spinlock.h>

// 声明一个静态函数，该函数用于同步锁定的缓冲区列表。
static int fsync_buffers_list(spinlock_t *lock, struct list_head *list);

// 定义一个宏，它使用list_entry宏从给定的list_head结构中提取buffer_head结构。
// list_entry宏通常用于从链表元素获取包含它的结构体。
#define BH_ENTRY(list) list_entry((list), struct buffer_head, b_assoc_buffers)

// 定义一个内联函数，用于初始化buffer_head结构的某些字段。
inline void
init_buffer(struct buffer_head *bh, bh_end_io_t *handler, void *private)
{
	// 将handler函数指针赋值给b_end_io，此函数用于IO操作完成后的回调。
	bh->b_end_io = handler;
	// 将private指针赋值给b_private，此字段用于存放用户自定义的数据。
	bh->b_private = private;
}
EXPORT_SYMBOL(init_buffer);

/*
 * 同步缓冲区的辅助函数，被等待函数调用以促进缓冲区的解锁。
 */
static int sync_buffer(void *word)
{
	// 定义一个块设备的指针
	struct block_device *bd;
	// 从b_state成员的地址获取整个buffer_head结构的地址
	struct buffer_head *bh
		= container_of(word, struct buffer_head, b_state);

	// 内存屏障，确保之前的内存操作不会与后面的操作重叠
	smp_mb();
	// 获取buffer_head所属的块设备
	bd = bh->b_bdev;
	// 如果块设备存在，则运行该设备对应的地址空间
	if (bd)
		blk_run_address_space(bd->bd_inode->i_mapping);
	io_schedule();	// 调度其他I/O操作，让出处理器
	return 0;	// 返回0表示函数执行成功
}

void __lock_buffer(struct buffer_head *bh)
{
	wait_on_bit_lock(&bh->b_state, BH_Lock, sync_buffer,
							TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(__lock_buffer);

void unlock_buffer(struct buffer_head *bh)
{
	clear_bit_unlock(BH_Lock, &bh->b_state);
	smp_mb__after_clear_bit();
	wake_up_bit(&bh->b_state, BH_Lock);
}
EXPORT_SYMBOL(unlock_buffer);

/*
 * Block until a buffer comes unlocked.  This doesn't stop it
 * from becoming locked again - you have to lock it yourself
 * if you want to preserve its state.
 */
/*
 * 等待直到一个缓冲区解锁。这并不会阻止它再次被锁定——如果你想保持它的状态，
 * 你需要自己锁定它。
 */
void __wait_on_buffer(struct buffer_head * bh)
{
	// 等待特定的位（BH_Lock）在给定的位字段（bh->b_state）中被清除，使用的等待方式是不可中断的等待
	wait_on_bit(&bh->b_state, BH_Lock, sync_buffer, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(__wait_on_buffer);

static void
__clear_page_buffers(struct page *page)
{
	ClearPagePrivate(page);
	set_page_private(page, 0);
	page_cache_release(page);
}


static int quiet_error(struct buffer_head *bh)
{
	if (!test_bit(BH_Quiet, &bh->b_state) && printk_ratelimit())
		return 0;
	return 1;
}


static void buffer_io_error(struct buffer_head *bh)
{
	char b[BDEVNAME_SIZE];
	printk(KERN_ERR "Buffer I/O error on device %s, logical block %Lu\n",
			bdevname(bh->b_bdev, b),
			(unsigned long long)bh->b_blocknr);
}

/*
 * End-of-IO handler helper function which does not touch the bh after
 * unlocking it.
 * Note: unlock_buffer() sort-of does touch the bh after unlocking it, but
 * a race there is benign: unlock_buffer() only use the bh's address for
 * hashing after unlocking the buffer, so it doesn't actually touch the bh
 * itself.
 */
static void __end_buffer_read_notouch(struct buffer_head *bh, int uptodate)
{
	if (uptodate) {
		set_buffer_uptodate(bh);
	} else {
		/* This happens, due to failed READA attempts. */
		clear_buffer_uptodate(bh);
	}
	unlock_buffer(bh);
}

/*
 * Default synchronous end-of-IO handler..  Just mark it up-to-date and
 * unlock the buffer. This is what ll_rw_block uses too.
 */
void end_buffer_read_sync(struct buffer_head *bh, int uptodate)
{
	__end_buffer_read_notouch(bh, uptodate);
	put_bh(bh);
}
EXPORT_SYMBOL(end_buffer_read_sync);

void end_buffer_write_sync(struct buffer_head *bh, int uptodate)
{
	char b[BDEVNAME_SIZE];

	if (uptodate) {
		set_buffer_uptodate(bh);
	} else {
		if (!buffer_eopnotsupp(bh) && !quiet_error(bh)) {
			buffer_io_error(bh);
			printk(KERN_WARNING "lost page write due to "
					"I/O error on %s\n",
				       bdevname(bh->b_bdev, b));
		}
		set_buffer_write_io_error(bh);
		clear_buffer_uptodate(bh);
	}
	unlock_buffer(bh);
	put_bh(bh);
}
EXPORT_SYMBOL(end_buffer_write_sync);

/*
 * Various filesystems appear to want __find_get_block to be non-blocking.
 * But it's the page lock which protects the buffers.  To get around this,
 * we get exclusion from try_to_free_buffers with the blockdev mapping's
 * private_lock.
 *
 * Hack idea: for the blockdev mapping, i_bufferlist_lock contention
 * may be quite high.  This code could TryLock the page, and if that
 * succeeds, there is no need to take private_lock. (But if
 * private_lock is contended then so is mapping->tree_lock).
 */
static struct buffer_head *
__find_get_block_slow(struct block_device *bdev, sector_t block)
{
	struct inode *bd_inode = bdev->bd_inode;
	struct address_space *bd_mapping = bd_inode->i_mapping;
	struct buffer_head *ret = NULL;
	pgoff_t index;
	struct buffer_head *bh;
	struct buffer_head *head;
	struct page *page;
	int all_mapped = 1;

	index = block >> (PAGE_CACHE_SHIFT - bd_inode->i_blkbits);
	page = find_get_page(bd_mapping, index);
	if (!page)
		goto out;

	spin_lock(&bd_mapping->private_lock);
	if (!page_has_buffers(page))
		goto out_unlock;
	head = page_buffers(page);
	bh = head;
	do {
		if (!buffer_mapped(bh))
			all_mapped = 0;
		else if (bh->b_blocknr == block) {
			ret = bh;
			get_bh(bh);
			goto out_unlock;
		}
		bh = bh->b_this_page;
	} while (bh != head);

	/* we might be here because some of the buffers on this page are
	 * not mapped.  This is due to various races between
	 * file io on the block device and getblk.  It gets dealt with
	 * elsewhere, don't buffer_error if we had some unmapped buffers
	 */
	if (all_mapped) {
		printk("__find_get_block_slow() failed. "
			"block=%llu, b_blocknr=%llu\n",
			(unsigned long long)block,
			(unsigned long long)bh->b_blocknr);
		printk("b_state=0x%08lx, b_size=%zu\n",
			bh->b_state, bh->b_size);
		printk("device blocksize: %d\n", 1 << bd_inode->i_blkbits);
	}
out_unlock:
	spin_unlock(&bd_mapping->private_lock);
	page_cache_release(page);
out:
	return ret;
}

/* If invalidate_buffers() will trash dirty buffers, it means some kind
   of fs corruption is going on. Trashing dirty data always imply losing
   information that was supposed to be just stored on the physical layer
   by the user.

   Thus invalidate_buffers in general usage is not allwowed to trash
   dirty buffers. For example ioctl(FLSBLKBUF) expects dirty data to
   be preserved.  These buffers are simply skipped.
  
   We also skip buffers which are still in use.  For example this can
   happen if a userspace program is reading the block device.

   NOTE: In the case where the user removed a removable-media-disk even if
   there's still dirty data not synced on disk (due a bug in the device driver
   or due an error of the user), by not destroying the dirty buffers we could
   generate corruption also on the next media inserted, thus a parameter is
   necessary to handle this case in the most safe way possible (trying
   to not corrupt also the new disk inserted with the data belonging to
   the old now corrupted disk). Also for the ramdisk the natural thing
   to do in order to release the ramdisk memory is to destroy dirty buffers.

   These are two special cases. Normal usage imply the device driver
   to issue a sync on the device (without waiting I/O completion) and
   then an invalidate_buffers call that doesn't trash dirty buffers.

   For handling cache coherency with the blkdev pagecache the 'update' case
   is been introduced. It is needed to re-read from disk any pinned
   buffer. NOTE: re-reading from disk is destructive so we can do it only
   when we assume nobody is changing the buffercache under our I/O and when
   we think the disk contains more recent information than the buffercache.
   The update == 1 pass marks the buffers we need to update, the update == 2
   pass does the actual I/O. */
void invalidate_bdev(struct block_device *bdev)
{
	struct address_space *mapping = bdev->bd_inode->i_mapping;

	if (mapping->nrpages == 0)
		return;

	invalidate_bh_lrus();
	invalidate_mapping_pages(mapping, 0, -1);
}
EXPORT_SYMBOL(invalidate_bdev);

/*
 * Kick the writeback threads then try to free up some ZONE_NORMAL memory.
 */
static void free_more_memory(void)
{
	struct zone *zone;
	int nid;

	wakeup_flusher_threads(1024);
	yield();

	for_each_online_node(nid) {
		(void)first_zones_zonelist(node_zonelist(nid, GFP_NOFS),
						gfp_zone(GFP_NOFS), NULL,
						&zone);
		if (zone)
			try_to_free_pages(node_zonelist(nid, GFP_NOFS), 0,
						GFP_NOFS, NULL);
	}
}

/*
 * I/O completion handler for block_read_full_page() - pages
 * which come unlocked at the end of I/O.
 */
static void end_buffer_async_read(struct buffer_head *bh, int uptodate)
{
	unsigned long flags;
	struct buffer_head *first;
	struct buffer_head *tmp;
	struct page *page;
	int page_uptodate = 1;

	BUG_ON(!buffer_async_read(bh));

	page = bh->b_page;
	if (uptodate) {
		set_buffer_uptodate(bh);
	} else {
		clear_buffer_uptodate(bh);
		if (!quiet_error(bh))
			buffer_io_error(bh);
		SetPageError(page);
	}

	/*
	 * Be _very_ careful from here on. Bad things can happen if
	 * two buffer heads end IO at almost the same time and both
	 * decide that the page is now completely done.
	 */
	first = page_buffers(page);
	local_irq_save(flags);
	bit_spin_lock(BH_Uptodate_Lock, &first->b_state);
	clear_buffer_async_read(bh);
	unlock_buffer(bh);
	tmp = bh;
	do {
		if (!buffer_uptodate(tmp))
			page_uptodate = 0;
		if (buffer_async_read(tmp)) {
			BUG_ON(!buffer_locked(tmp));
			goto still_busy;
		}
		tmp = tmp->b_this_page;
	} while (tmp != bh);
	bit_spin_unlock(BH_Uptodate_Lock, &first->b_state);
	local_irq_restore(flags);

	/*
	 * If none of the buffers had errors and they are all
	 * uptodate then we can set the page uptodate.
	 */
	if (page_uptodate && !PageError(page))
		SetPageUptodate(page);
	unlock_page(page);
	return;

still_busy:
	bit_spin_unlock(BH_Uptodate_Lock, &first->b_state);
	local_irq_restore(flags);
	return;
}

/*
 * Completion handler for block_write_full_page() - pages which are unlocked
 * during I/O, and which have PageWriteback cleared upon I/O completion.
 */
void end_buffer_async_write(struct buffer_head *bh, int uptodate)
{
	char b[BDEVNAME_SIZE];
	unsigned long flags;
	struct buffer_head *first;
	struct buffer_head *tmp;
	struct page *page;

	BUG_ON(!buffer_async_write(bh));

	page = bh->b_page;
	if (uptodate) {
		set_buffer_uptodate(bh);
	} else {
		if (!quiet_error(bh)) {
			buffer_io_error(bh);
			printk(KERN_WARNING "lost page write due to "
					"I/O error on %s\n",
			       bdevname(bh->b_bdev, b));
		}
		set_bit(AS_EIO, &page->mapping->flags);
		set_buffer_write_io_error(bh);
		clear_buffer_uptodate(bh);
		SetPageError(page);
	}

	first = page_buffers(page);
	local_irq_save(flags);
	bit_spin_lock(BH_Uptodate_Lock, &first->b_state);

	clear_buffer_async_write(bh);
	unlock_buffer(bh);
	tmp = bh->b_this_page;
	while (tmp != bh) {
		if (buffer_async_write(tmp)) {
			BUG_ON(!buffer_locked(tmp));
			goto still_busy;
		}
		tmp = tmp->b_this_page;
	}
	bit_spin_unlock(BH_Uptodate_Lock, &first->b_state);
	local_irq_restore(flags);
	end_page_writeback(page);
	return;

still_busy:
	bit_spin_unlock(BH_Uptodate_Lock, &first->b_state);
	local_irq_restore(flags);
	return;
}
EXPORT_SYMBOL(end_buffer_async_write);

/*
 * If a page's buffers are under async readin (end_buffer_async_read
 * completion) then there is a possibility that another thread of
 * control could lock one of the buffers after it has completed
 * but while some of the other buffers have not completed.  This
 * locked buffer would confuse end_buffer_async_read() into not unlocking
 * the page.  So the absence of BH_Async_Read tells end_buffer_async_read()
 * that this buffer is not under async I/O.
 *
 * The page comes unlocked when it has no locked buffer_async buffers
 * left.
 *
 * PageLocked prevents anyone starting new async I/O reads any of
 * the buffers.
 *
 * PageWriteback is used to prevent simultaneous writeout of the same
 * page.
 *
 * PageLocked prevents anyone from starting writeback of a page which is
 * under read I/O (PageWriteback is only ever set against a locked page).
 */
static void mark_buffer_async_read(struct buffer_head *bh)
{
	bh->b_end_io = end_buffer_async_read;
	set_buffer_async_read(bh);
}

static void mark_buffer_async_write_endio(struct buffer_head *bh,
					  bh_end_io_t *handler)
{
	bh->b_end_io = handler;
	set_buffer_async_write(bh);
}

void mark_buffer_async_write(struct buffer_head *bh)
{
	mark_buffer_async_write_endio(bh, end_buffer_async_write);
}
EXPORT_SYMBOL(mark_buffer_async_write);


/*
 * fs/buffer.c contains helper functions for buffer-backed address space's
 * fsync functions.  A common requirement for buffer-based filesystems is
 * that certain data from the backing blockdev needs to be written out for
 * a successful fsync().  For example, ext2 indirect blocks need to be
 * written back and waited upon before fsync() returns.
 *
 * The functions mark_buffer_inode_dirty(), fsync_inode_buffers(),
 * inode_has_buffers() and invalidate_inode_buffers() are provided for the
 * management of a list of dependent buffers at ->i_mapping->private_list.
 *
 * Locking is a little subtle: try_to_free_buffers() will remove buffers
 * from their controlling inode's queue when they are being freed.  But
 * try_to_free_buffers() will be operating against the *blockdev* mapping
 * at the time, not against the S_ISREG file which depends on those buffers.
 * So the locking for private_list is via the private_lock in the address_space
 * which backs the buffers.  Which is different from the address_space 
 * against which the buffers are listed.  So for a particular address_space,
 * mapping->private_lock does *not* protect mapping->private_list!  In fact,
 * mapping->private_list will always be protected by the backing blockdev's
 * ->private_lock.
 *
 * Which introduces a requirement: all buffers on an address_space's
 * ->private_list must be from the same address_space: the blockdev's.
 *
 * address_spaces which do not place buffers at ->private_list via these
 * utility functions are free to use private_lock and private_list for
 * whatever they want.  The only requirement is that list_empty(private_list)
 * be true at clear_inode() time.
 *
 * FIXME: clear_inode should not call invalidate_inode_buffers().  The
 * filesystems should do that.  invalidate_inode_buffers() should just go
 * BUG_ON(!list_empty).
 *
 * FIXME: mark_buffer_dirty_inode() is a data-plane operation.  It should
 * take an address_space, not an inode.  And it should be called
 * mark_buffer_dirty_fsync() to clearly define why those buffers are being
 * queued up.
 *
 * FIXME: mark_buffer_dirty_inode() doesn't need to add the buffer to the
 * list if it is already on a list.  Because if the buffer is on a list,
 * it *must* already be on the right one.  If not, the filesystem is being
 * silly.  This will save a ton of locking.  But first we have to ensure
 * that buffers are taken *off* the old inode's list when they are freed
 * (presumably in truncate).  That requires careful auditing of all
 * filesystems (do it inside bforget()).  It could also be done by bringing
 * b_inode back.
 */

/*
 * The buffer's backing address_space's private_lock must be held
 */
static void __remove_assoc_queue(struct buffer_head *bh)
{
	list_del_init(&bh->b_assoc_buffers);
	WARN_ON(!bh->b_assoc_map);
	if (buffer_write_io_error(bh))
		set_bit(AS_EIO, &bh->b_assoc_map->flags);
	bh->b_assoc_map = NULL;
}

int inode_has_buffers(struct inode *inode)
{
	return !list_empty(&inode->i_data.private_list);
}

/*
 * osync is designed to support O_SYNC io.  It waits synchronously for
 * all already-submitted IO to complete, but does not queue any new
 * writes to the disk.
 *
 * To do O_SYNC writes, just queue the buffer writes with ll_rw_block as
 * you dirty the buffers, and then use osync_inode_buffers to wait for
 * completion.  Any other dirty buffers which are not yet queued for
 * write will not be flushed to disk by the osync.
 */
static int osync_buffers_list(spinlock_t *lock, struct list_head *list)
{
	struct buffer_head *bh;
	struct list_head *p;
	int err = 0;

	spin_lock(lock);
repeat:
	list_for_each_prev(p, list) {
		bh = BH_ENTRY(p);
		if (buffer_locked(bh)) {
			get_bh(bh);
			spin_unlock(lock);
			wait_on_buffer(bh);
			if (!buffer_uptodate(bh))
				err = -EIO;
			brelse(bh);
			spin_lock(lock);
			goto repeat;
		}
	}
	spin_unlock(lock);
	return err;
}

static void do_thaw_all(struct work_struct *work)
{
	struct super_block *sb;
	char b[BDEVNAME_SIZE];

	spin_lock(&sb_lock);
restart:
	list_for_each_entry(sb, &super_blocks, s_list) {
		sb->s_count++;
		spin_unlock(&sb_lock);
		down_read(&sb->s_umount);
		while (sb->s_bdev && !thaw_bdev(sb->s_bdev, sb))
			printk(KERN_WARNING "Emergency Thaw on %s\n",
			       bdevname(sb->s_bdev, b));
		up_read(&sb->s_umount);
		spin_lock(&sb_lock);
		if (__put_super_and_need_restart(sb))
			goto restart;
	}
	spin_unlock(&sb_lock);
	kfree(work);
	printk(KERN_WARNING "Emergency Thaw complete\n");
}

/**
 * emergency_thaw_all -- forcibly thaw every frozen filesystem
 *
 * Used for emergency unfreeze of all filesystems via SysRq
 */
void emergency_thaw_all(void)
{
	struct work_struct *work;

	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	if (work) {
		INIT_WORK(work, do_thaw_all);
		schedule_work(work);
	}
}

/**
 * sync_mapping_buffers - write out & wait upon a mapping's "associated" buffers
 * @mapping: the mapping which wants those buffers written
 *
 * Starts I/O against the buffers at mapping->private_list, and waits upon
 * that I/O.
 *
 * Basically, this is a convenience function for fsync().
 * @mapping is a file or directory which needs those buffers to be written for
 * a successful fsync().
 */
/**
 * sync_mapping_buffers - 写出并等待一个映射的“关联”缓冲区
 * @mapping: 需要写出这些缓冲区的映射
 *
 * 启动映射->private_list中的缓冲区的I/O，并等待该I/O。
 *
 * 基本上，这是一个用于 fsync() 的便利函数。
 * @mapping 是一个文件或目录，需要这些缓冲区被写出以便
 * 成功执行 fsync()。
 */
int sync_mapping_buffers(struct address_space *mapping)
{
	// 获取关联的映射
	struct address_space *buffer_mapping = mapping->assoc_mapping;

	// 如果关联的映射为空或者私有列表为空，则无需操作，直接返回0
	if (buffer_mapping == NULL || list_empty(&mapping->private_list))
		return 0;

	// 否则，执行真正的缓冲区同步操作
	return fsync_buffers_list(&buffer_mapping->private_lock,
					&mapping->private_list);	 // 使用关联映射的私有锁和当前映射的私有列表
}
EXPORT_SYMBOL(sync_mapping_buffers);

/*
 * Called when we've recently written block `bblock', and it is known that
 * `bblock' was for a buffer_boundary() buffer.  This means that the block at
 * `bblock + 1' is probably a dirty indirect block.  Hunt it down and, if it's
 * dirty, schedule it for IO.  So that indirects merge nicely with their data.
 */
void write_boundary_block(struct block_device *bdev,
			sector_t bblock, unsigned blocksize)
{
	struct buffer_head *bh = __find_get_block(bdev, bblock + 1, blocksize);
	if (bh) {
		if (buffer_dirty(bh))
			ll_rw_block(WRITE, 1, &bh);
		put_bh(bh);
	}
}

void mark_buffer_dirty_inode(struct buffer_head *bh, struct inode *inode)
{
	struct address_space *mapping = inode->i_mapping;
	struct address_space *buffer_mapping = bh->b_page->mapping;

	mark_buffer_dirty(bh);
	if (!mapping->assoc_mapping) {
		mapping->assoc_mapping = buffer_mapping;
	} else {
		BUG_ON(mapping->assoc_mapping != buffer_mapping);
	}
	if (!bh->b_assoc_map) {
		spin_lock(&buffer_mapping->private_lock);
		list_move_tail(&bh->b_assoc_buffers,
				&mapping->private_list);
		bh->b_assoc_map = mapping;
		spin_unlock(&buffer_mapping->private_lock);
	}
}
EXPORT_SYMBOL(mark_buffer_dirty_inode);

/*
 * Mark the page dirty, and set it dirty in the radix tree, and mark the inode
 * dirty.
 *
 * If warn is true, then emit a warning if the page is not uptodate and has
 * not been truncated.
 */
static void __set_page_dirty(struct page *page,
		struct address_space *mapping, int warn)
{
	spin_lock_irq(&mapping->tree_lock);
	if (page->mapping) {	/* Race with truncate? */
		WARN_ON_ONCE(warn && !PageUptodate(page));
		account_page_dirtied(page, mapping);
		radix_tree_tag_set(&mapping->page_tree,
				page_index(page), PAGECACHE_TAG_DIRTY);
	}
	spin_unlock_irq(&mapping->tree_lock);
	__mark_inode_dirty(mapping->host, I_DIRTY_PAGES);
}

/*
 * Add a page to the dirty page list.
 *
 * It is a sad fact of life that this function is called from several places
 * deeply under spinlocking.  It may not sleep.
 *
 * If the page has buffers, the uptodate buffers are set dirty, to preserve
 * dirty-state coherency between the page and the buffers.  It the page does
 * not have buffers then when they are later attached they will all be set
 * dirty.
 *
 * The buffers are dirtied before the page is dirtied.  There's a small race
 * window in which a writepage caller may see the page cleanness but not the
 * buffer dirtiness.  That's fine.  If this code were to set the page dirty
 * before the buffers, a concurrent writepage caller could clear the page dirty
 * bit, see a bunch of clean buffers and we'd end up with dirty buffers/clean
 * page on the dirty page list.
 *
 * We use private_lock to lock against try_to_free_buffers while using the
 * page's buffer list.  Also use this to protect against clean buffers being
 * added to the page after it was set dirty.
 *
 * FIXME: may need to call ->reservepage here as well.  That's rather up to the
 * address_space though.
 */
int __set_page_dirty_buffers(struct page *page)
{
	int newly_dirty;
	struct address_space *mapping = page_mapping(page);

	if (unlikely(!mapping))
		return !TestSetPageDirty(page);

	spin_lock(&mapping->private_lock);
	if (page_has_buffers(page)) {
		struct buffer_head *head = page_buffers(page);
		struct buffer_head *bh = head;

		do {
			set_buffer_dirty(bh);
			bh = bh->b_this_page;
		} while (bh != head);
	}
	newly_dirty = !TestSetPageDirty(page);
	spin_unlock(&mapping->private_lock);

	if (newly_dirty)
		__set_page_dirty(page, mapping, 1);
	return newly_dirty;
}
EXPORT_SYMBOL(__set_page_dirty_buffers);

/*
 * Write out and wait upon a list of buffers.
 *
 * We have conflicting pressures: we want to make sure that all
 * initially dirty buffers get waited on, but that any subsequently
 * dirtied buffers don't.  After all, we don't want fsync to last
 * forever if somebody is actively writing to the file.
 *
 * Do this in two main stages: first we copy dirty buffers to a
 * temporary inode list, queueing the writes as we go.  Then we clean
 * up, waiting for those writes to complete.
 * 
 * During this second stage, any subsequent updates to the file may end
 * up refiling the buffer on the original inode's dirty list again, so
 * there is a chance we will end up with a buffer queued for write but
 * not yet completed on that list.  So, as a final cleanup we go through
 * the osync code to catch these locked, dirty buffers without requeuing
 * any newly dirty buffers for write.
 */
/*
 * 写出并等待一系列缓冲区。
 *
 * 我们面临着冲突的压力：我们想确保所有最初脏的缓冲区都被等待，
 * 但那些随后变脏的缓冲区则不需要。毕竟，如果有人在活跃地写文件，
 * 我们不希望fsync持续不断。
 *
 * 这个过程分为两个主要阶段：首先，我们将脏缓冲区复制到一个临时的inode列表中，
 * 在这个过程中进行写操作。然后，我们进行清理，等待这些写操作完成。
 * 
 * 在这第二阶段中，任何对文件的后续更新可能会再次将缓冲区放回原始inode的脏列表中，
 * 所以可能会出现一个已经排队等待写入但还没有完成的缓冲区在该列表上。
 * 因此，作为最终清理，我们通过osync代码来捕获这些锁定的、脏的缓冲区，
 * 同时不会重新排队等待任何新变脏的缓冲区写入。
 */
/**
 * 一个用于同步（写出并等待完成）特定列表中所有缓冲区的函数。
 * 函数通过两个阶段处理：首先，将所有脏缓冲区移到一个临时列表
 * 并启动I/O操作；其次，等待这些I/O操作完成，并处理在此期间
 * 可能再次变脏的缓冲区。这个设计防止了在文件持续被写入时 
 * fsync 操作无休止执行的问题。
 */
static int fsync_buffers_list(spinlock_t *lock, struct list_head *list)
{
	struct buffer_head *bh;	// 缓冲区头部
	struct list_head tmp;		// 临时列表
	struct address_space *mapping, *prev_mapping = NULL;
	int err = 0, err2;	// 错误码

	// 初始化一个空的临时链表
	INIT_LIST_HEAD(&tmp);
	
	// 锁住给定的锁
	spin_lock(lock);
	// 循环遍历列表直到为空
	while (!list_empty(list)) {
		// 从列表中获取下一个缓冲区头
		bh = BH_ENTRY(list->next);
		// 获取该缓冲区的地址空间
		mapping = bh->b_assoc_map;
		// 从关联队列中移除此缓冲区头
		__remove_assoc_queue(bh);
		/* Avoid race with mark_buffer_dirty_inode() which does
		 * a lockless check and we rely on seeing the dirty bit */
		/* 避免与mark_buffer_dirty_inode()的竞争，后者在无锁情况下检查，我们依赖看到脏位 */
		smp_mb();
		// 如果缓冲区是脏的或者锁定的
		if (buffer_dirty(bh) || buffer_locked(bh)) {
			// 添加到临时列表
			list_add(&bh->b_assoc_buffers, &tmp);
			bh->b_assoc_map = mapping;
			if (buffer_dirty(bh)) {
				// 增加引用计数
				get_bh(bh);
				spin_unlock(lock);
				/*
				 * Ensure any pending I/O completes so that
				 * ll_rw_block() actually writes the current
				 * contents - it is a noop if I/O is still in
				 * flight on potentially older contents.
				 */
				/*
				 * 确保任何悬挂的I/O完成，这样ll_rw_block()实际上写入当前内容 - 
				 * 如果I/O还在更老的内容上进行，那么这是一个空操作。
				 */
				ll_rw_block(SWRITE_SYNC_PLUG, 1, &bh);

				/*
				 * Kick off IO for the previous mapping. Note
				 * that we will not run the very last mapping,
				 * wait_on_buffer() will do that for us
				 * through sync_buffer().
				 */
				/*
				 * 启动前一个映射的IO。注意，我们不会运行最后一个映射，
				 * wait_on_buffer()会通过sync_buffer()为我们进行。
				 */
				if (prev_mapping && prev_mapping != mapping)
					blk_run_address_space(prev_mapping);
				prev_mapping = mapping;

				brelse(bh);	// 减少引用计数
				spin_lock(lock);	// 重新上锁
			}
		}
	}

	while (!list_empty(&tmp)) {
		// 从临时列表中获取前一个缓冲区头
		bh = BH_ENTRY(tmp.prev);
		get_bh(bh);
		mapping = bh->b_assoc_map;
		__remove_assoc_queue(bh);
		/* Avoid race with mark_buffer_dirty_inode() which does
		 * a lockless check and we rely on seeing the dirty bit */
		smp_mb();
		if (buffer_dirty(bh)) {
			// 将脏缓冲区添加到私有列表
			list_add(&bh->b_assoc_buffers,
				 &mapping->private_list);
			bh->b_assoc_map = mapping;
		}
		spin_unlock(lock);
		// 等待缓冲区的I/O完成
		wait_on_buffer(bh);
		// 如果缓冲区不是最新的，记录输入输出错误
		if (!buffer_uptodate(bh))
			err = -EIO;
		brelse(bh);
		spin_lock(lock);
	}
	
	spin_unlock(lock);	// 解锁
	// 处理可能遗留的锁定或脏缓冲区
	err2 = osync_buffers_list(lock, list);
	if (err)
		return err;	// 返回第一个错误
	else
		return err2;	// 否则返回第二个错误
}

/*
 * Invalidate any and all dirty buffers on a given inode.  We are
 * probably unmounting the fs, but that doesn't mean we have already
 * done a sync().  Just drop the buffers from the inode list.
 *
 * NOTE: we take the inode's blockdev's mapping's private_lock.  Which
 * assumes that all the buffers are against the blockdev.  Not true
 * for reiserfs.
 */
void invalidate_inode_buffers(struct inode *inode)
{
	if (inode_has_buffers(inode)) {
		struct address_space *mapping = &inode->i_data;
		struct list_head *list = &mapping->private_list;
		struct address_space *buffer_mapping = mapping->assoc_mapping;

		spin_lock(&buffer_mapping->private_lock);
		while (!list_empty(list))
			__remove_assoc_queue(BH_ENTRY(list->next));
		spin_unlock(&buffer_mapping->private_lock);
	}
}
EXPORT_SYMBOL(invalidate_inode_buffers);

/*
 * Remove any clean buffers from the inode's buffer list.  This is called
 * when we're trying to free the inode itself.  Those buffers can pin it.
 *
 * Returns true if all buffers were removed.
 */
int remove_inode_buffers(struct inode *inode)
{
	int ret = 1;

	if (inode_has_buffers(inode)) {
		struct address_space *mapping = &inode->i_data;
		struct list_head *list = &mapping->private_list;
		struct address_space *buffer_mapping = mapping->assoc_mapping;

		spin_lock(&buffer_mapping->private_lock);
		while (!list_empty(list)) {
			struct buffer_head *bh = BH_ENTRY(list->next);
			if (buffer_dirty(bh)) {
				ret = 0;
				break;
			}
			__remove_assoc_queue(bh);
		}
		spin_unlock(&buffer_mapping->private_lock);
	}
	return ret;
}

/*
 * Create the appropriate buffers when given a page for data area and
 * the size of each buffer.. Use the bh->b_this_page linked list to
 * follow the buffers created.  Return NULL if unable to create more
 * buffers.
 *
 * The retry flag is used to differentiate async IO (paging, swapping)
 * which may not fail from ordinary buffer allocations.
 */
/*
 * 在给定一个数据区的页面和每个缓冲区的大小时，创建适当的缓冲区。
 * 使用 bh->b_this_page 链表来跟踪创建的缓冲区。如果无法创建更多
 * 缓冲区，则返回 NULL。
 *
 * retry 标志用于区分异步 IO（分页、交换）与普通缓冲区分配，
 * 前者可能不会失败。
 */
struct buffer_head *alloc_page_buffers(struct page *page, unsigned long size,
		int retry)
{
	struct buffer_head *bh, *head;
	long offset;

try_again:	// 重试标签，用于缓冲区创建失败时重新尝试
	head = NULL;	// 初始化头部指针
	offset = PAGE_SIZE;	// 设置偏移量为页面大小
	while ((offset -= size) >= 0) {	// 循环减小偏移量，创建缓冲区
		// 分配一个新的缓冲区头
		bh = alloc_buffer_head(GFP_NOFS);
		if (!bh)
			goto no_grow;	// 如果分配失败，跳转到清理代码

		bh->b_bdev = NULL;  // 设置缓冲区的块设备为 NULL
		bh->b_this_page = head;  // 链接当前缓冲区到链表头
		bh->b_blocknr = -1;  // 设置块编号为 -1
		head = bh;  // 更新链表头

		bh->b_state = 0;  // 初始化状态为 0
		atomic_set(&bh->b_count, 0);  // 设置引用计数为 0
		bh->b_private = NULL;  // 私有数据设置为 NULL
		bh->b_size = size;  // 设置缓冲区大小

		/* Link the buffer to its page */
		/* 将缓冲区链接到它的页面 */
		set_bh_page(bh, page, offset);	// 设置缓冲区的页面和偏移量

		init_buffer(bh, NULL, NULL);		// 初始化缓冲区
	}
	return head;	// 返回创建的缓冲区链表头
/*
 * In case anything failed, we just free everything we got.
 */
/*
 * 如果出现任何失败，我们就释放我们得到的所有东西。
 */
no_grow:
	if (head) {
		do {
			bh = head;	// 获取链表头
			head = head->b_this_page;	// 移动到下一个缓冲区
			free_buffer_head(bh);	// 释放缓冲区头
		} while (head);	// 循环直到链表为空
	}

	/*
	 * Return failure for non-async IO requests.  Async IO requests
	 * are not allowed to fail, so we have to wait until buffer heads
	 * become available.  But we don't want tasks sleeping with 
	 * partially complete buffers, so all were released above.
	 */
	/*
	 * 对于非异步 IO 请求，返回失败。异步 IO 请求不允许失败，
	 * 所以我们必须等到缓冲区头变得可用。但我们不希望任务在
	 * 缓冲区部分完成时进入睡眠状态，所以上面释放了所有缓冲区。
	 */
	if (!retry)
		return NULL;	// 如果不允许重试，则返回 NULL

	/* We're _really_ low on memory. Now we just
	 * wait for old buffer heads to become free due to
	 * finishing IO.  Since this is an async request and
	 * the reserve list is empty, we're sure there are 
	 * async buffer heads in use.
	 */
	/* 我们的内存真的很紧张。现在我们只是等待旧的缓冲区头
	 * 因完成 IO 而释放。由于这是一个异步请求，并且保留列表为空，
	 * 我们确定有正在使用的异步缓冲区头。
	 */
	free_more_memory();	// 尝试释放更多内存
	goto try_again;			// 重试创建缓冲区
}
EXPORT_SYMBOL_GPL(alloc_page_buffers);

static inline void
link_dev_buffers(struct page *page, struct buffer_head *head)
{
	struct buffer_head *bh, *tail;

	bh = head;
	do {
		tail = bh;
		bh = bh->b_this_page;
	} while (bh);
	tail->b_this_page = head;
	attach_page_buffers(page, head);
}

/*
 * Initialise the state of a blockdev page's buffers.
 */ 
static void
init_page_buffers(struct page *page, struct block_device *bdev,
			sector_t block, int size)
{
	struct buffer_head *head = page_buffers(page);
	struct buffer_head *bh = head;
	int uptodate = PageUptodate(page);

	do {
		if (!buffer_mapped(bh)) {
			init_buffer(bh, NULL, NULL);
			bh->b_bdev = bdev;
			bh->b_blocknr = block;
			if (uptodate)
				set_buffer_uptodate(bh);
			set_buffer_mapped(bh);
		}
		block++;
		bh = bh->b_this_page;
	} while (bh != head);
}

/*
 * Create the page-cache page that contains the requested block.
 *
 * This is user purely for blockdev mappings.
 */
static struct page *
grow_dev_page(struct block_device *bdev, sector_t block,
		pgoff_t index, int size)
{
	struct inode *inode = bdev->bd_inode;
	struct page *page;
	struct buffer_head *bh;

	page = find_or_create_page(inode->i_mapping, index,
		(mapping_gfp_mask(inode->i_mapping) & ~__GFP_FS)|__GFP_MOVABLE);
	if (!page)
		return NULL;

	BUG_ON(!PageLocked(page));

	if (page_has_buffers(page)) {
		bh = page_buffers(page);
		if (bh->b_size == size) {
			init_page_buffers(page, bdev, block, size);
			return page;
		}
		if (!try_to_free_buffers(page))
			goto failed;
	}

	/*
	 * Allocate some buffers for this page
	 */
	bh = alloc_page_buffers(page, size, 0);
	if (!bh)
		goto failed;

	/*
	 * Link the page to the buffers and initialise them.  Take the
	 * lock to be atomic wrt __find_get_block(), which does not
	 * run under the page lock.
	 */
	spin_lock(&inode->i_mapping->private_lock);
	link_dev_buffers(page, bh);
	init_page_buffers(page, bdev, block, size);
	spin_unlock(&inode->i_mapping->private_lock);
	return page;

failed:
	BUG();
	unlock_page(page);
	page_cache_release(page);
	return NULL;
}

/*
 * Create buffers for the specified block device block's page.  If
 * that page was dirty, the buffers are set dirty also.
 */
static int
grow_buffers(struct block_device *bdev, sector_t block, int size)
{
	struct page *page;
	pgoff_t index;
	int sizebits;

	sizebits = -1;
	do {
		sizebits++;
	} while ((size << sizebits) < PAGE_SIZE);

	index = block >> sizebits;

	/*
	 * Check for a block which wants to lie outside our maximum possible
	 * pagecache index.  (this comparison is done using sector_t types).
	 */
	if (unlikely(index != block >> sizebits)) {
		char b[BDEVNAME_SIZE];

		printk(KERN_ERR "%s: requested out-of-range block %llu for "
			"device %s\n",
			__func__, (unsigned long long)block,
			bdevname(bdev, b));
		return -EIO;
	}
	block = index << sizebits;
	/* Create a page with the proper size buffers.. */
	page = grow_dev_page(bdev, block, index, size);
	if (!page)
		return 0;
	unlock_page(page);
	page_cache_release(page);
	return 1;
}

// 这是 __getblk 的一个辅助函数，用于在没有快速找到对应的 buffer_head 时，通过更慢的路径尝试获取或创建一个 buffer_head。
static struct buffer_head *
__getblk_slow(struct block_device *bdev, sector_t block, int size)
{
	/* Size must be multiple of hard sectorsize */
	/* 大小必须是硬盘扇区大小的整数倍 */
	// 检查所请求的块大小是否合法：必须是逻辑块大小的整数倍，且大小范围合理
	if (unlikely(size & (bdev_logical_block_size(bdev)-1) ||
			(size < 512 || size > PAGE_SIZE))) {
		// 打印错误消息，请求的块大小不合法
		printk(KERN_ERR "getblk(): invalid block size %d requested\n",
					size);
		// 打印逻辑块大小
		printk(KERN_ERR "logical block size: %d\n",
					bdev_logical_block_size(bdev));
		// 打印堆栈信息以帮助定位问题
		dump_stack();
		// 因为块大小不合法，返回NULL
		return NULL;
	}

	for (;;) {	// 无限循环尝试获取buffer_head
		struct buffer_head * bh;
		int ret;

		// 尝试找到一个现有的buffer_head
		bh = __find_get_block(bdev, block, size);
		if (bh)
			return bh;	// 如果找到了，直接返回

		// 尝试增加缓冲区来满足需求
		ret = grow_buffers(bdev, block, size);
		if (ret < 0)
			return NULL;	// 如果增加缓冲区失败，返回NULL
		if (ret == 0)
			// 如果没有足够的空间增加缓冲区，尝试释放更多内存
			free_more_memory();
	}
}

/*
 * The relationship between dirty buffers and dirty pages:
 *
 * Whenever a page has any dirty buffers, the page's dirty bit is set, and
 * the page is tagged dirty in its radix tree.
 *
 * At all times, the dirtiness of the buffers represents the dirtiness of
 * subsections of the page.  If the page has buffers, the page dirty bit is
 * merely a hint about the true dirty state.
 *
 * When a page is set dirty in its entirety, all its buffers are marked dirty
 * (if the page has buffers).
 *
 * When a buffer is marked dirty, its page is dirtied, but the page's other
 * buffers are not.
 *
 * Also.  When blockdev buffers are explicitly read with bread(), they
 * individually become uptodate.  But their backing page remains not
 * uptodate - even if all of its buffers are uptodate.  A subsequent
 * block_read_full_page() against that page will discover all the uptodate
 * buffers, will set the page uptodate and will perform no I/O.
 */

/**
 * mark_buffer_dirty - mark a buffer_head as needing writeout
 * @bh: the buffer_head to mark dirty
 *
 * mark_buffer_dirty() will set the dirty bit against the buffer, then set its
 * backing page dirty, then tag the page as dirty in its address_space's radix
 * tree and then attach the address_space's inode to its superblock's dirty
 * inode list.
 *
 * mark_buffer_dirty() is atomic.  It takes bh->b_page->mapping->private_lock,
 * mapping->tree_lock and the global inode_lock.
 */
void mark_buffer_dirty(struct buffer_head *bh)
{
	WARN_ON_ONCE(!buffer_uptodate(bh));

	/*
	 * Very *carefully* optimize the it-is-already-dirty case.
	 *
	 * Don't let the final "is it dirty" escape to before we
	 * perhaps modified the buffer.
	 */
	if (buffer_dirty(bh)) {
		smp_mb();
		if (buffer_dirty(bh))
			return;
	}

	if (!test_set_buffer_dirty(bh)) {
		struct page *page = bh->b_page;
		if (!TestSetPageDirty(page)) {
			struct address_space *mapping = page_mapping(page);
			if (mapping)
				__set_page_dirty(page, mapping, 0);
		}
	}
}
EXPORT_SYMBOL(mark_buffer_dirty);

/*
 * Decrement a buffer_head's reference count.  If all buffers against a page
 * have zero reference count, are clean and unlocked, and if the page is clean
 * and unlocked then try_to_free_buffers() may strip the buffers from the page
 * in preparation for freeing it (sometimes, rarely, buffers are removed from
 * a page but it ends up not being freed, and buffers may later be reattached).
 */
/*
 * 递减缓冲区头（buffer_head）的引用计数。如果页面上的所有缓冲区头的引用计数都为零，
 * 并且都是干净且未锁定的，而且如果页面本身也是干净且未锁定的，
 * 则 try_to_free_buffers() 可能会在准备释放页面前从页面中移除缓冲区头
 * （有时，虽然很少见，缓冲区头可能会从页面中移除，但页面最终没有被释放，
 * 并且缓冲区头可能稍后会重新附加）。
 */
void __brelse(struct buffer_head * buf)
{
	// 如果缓冲区头的引用计数非零
	if (atomic_read(&buf->b_count)) {
		put_bh(buf);	// 减少缓冲区头的引用计数
		return;
	}
	// 如果尝试释放一个已经是自由的缓冲区头，输出警告信息
	WARN(1, KERN_ERR "VFS: brelse: Trying to free free buffer\n");
}
EXPORT_SYMBOL(__brelse);

/*
 * bforget() is like brelse(), except it discards any
 * potentially dirty data.
 */
/*
 * bforget() 类似于 brelise()，但它会丢弃任何可能是脏的数据。
 */
void __bforget(struct buffer_head *bh)
{
	// 清除缓冲区头的脏标记
	clear_buffer_dirty(bh);
	// 如果缓冲区头关联了映射
	if (bh->b_assoc_map) {
		struct address_space *buffer_mapping = bh->b_page->mapping;

		// 获取锁
		spin_lock(&buffer_mapping->private_lock);
		// 从关联列表中移除缓冲区头
		list_del_init(&bh->b_assoc_buffers);
		// 清除关联映射
		bh->b_assoc_map = NULL;
		// 释放锁
		spin_unlock(&buffer_mapping->private_lock);
	}
	__brelse(bh);	// 最后调用__brelse释放缓冲区头
}
EXPORT_SYMBOL(__bforget);

static struct buffer_head *__bread_slow(struct buffer_head *bh)
{
	// 锁定缓冲区，防止同时访问
	lock_buffer(bh);
	// 如果缓冲区已经是最新的，则解锁并返回
	if (buffer_uptodate(bh)) {
		unlock_buffer(bh);
		return bh;
	} else {
		// 增加缓冲区的引用计数
		get_bh(bh);
		// 设置缓冲区的结束I/O函数，用于在读取完成后的处理
		bh->b_end_io = end_buffer_read_sync;
		// 提交缓冲区以进行读取
		submit_bh(READ, bh);
		// 等待缓冲区的读取完成
		wait_on_buffer(bh);
		// 如果读取完成后缓冲区数据是最新的，则返回缓冲区
		if (buffer_uptodate(bh))
			return bh;
	}
	// 释放缓冲区并返回NULL，表示读取失败
	brelse(bh);
	return NULL;
}

/*
 * Per-cpu buffer LRU implementation.  To reduce the cost of __find_get_block().
 * The bhs[] array is sorted - newest buffer is at bhs[0].  Buffers have their
 * refcount elevated by one when they're in an LRU.  A buffer can only appear
 * once in a particular CPU's LRU.  A single buffer can be present in multiple
 * CPU's LRUs at the same time.
 *
 * This is a transparent caching front-end to sb_bread(), sb_getblk() and
 * sb_find_get_block().
 *
 * The LRUs themselves only need locking against invalidate_bh_lrus.  We use
 * a local interrupt disable for that.
 */

#define BH_LRU_SIZE	8

struct bh_lru {
	struct buffer_head *bhs[BH_LRU_SIZE];
};

static DEFINE_PER_CPU(struct bh_lru, bh_lrus) = {{ NULL }};

#ifdef CONFIG_SMP
#define bh_lru_lock()	local_irq_disable()
#define bh_lru_unlock()	local_irq_enable()
#else
#define bh_lru_lock()	preempt_disable()
#define bh_lru_unlock()	preempt_enable()
#endif

static inline void check_irqs_on(void)
{
#ifdef irqs_disabled
	BUG_ON(irqs_disabled());
#endif
}

/*
 * The LRU management algorithm is dopey-but-simple.  Sorry.
 */
static void bh_lru_install(struct buffer_head *bh)
{
	struct buffer_head *evictee = NULL;
	struct bh_lru *lru;

	check_irqs_on();
	bh_lru_lock();
	lru = &__get_cpu_var(bh_lrus);
	if (lru->bhs[0] != bh) {
		struct buffer_head *bhs[BH_LRU_SIZE];
		int in;
		int out = 0;

		get_bh(bh);
		bhs[out++] = bh;
		for (in = 0; in < BH_LRU_SIZE; in++) {
			struct buffer_head *bh2 = lru->bhs[in];

			if (bh2 == bh) {
				__brelse(bh2);
			} else {
				if (out >= BH_LRU_SIZE) {
					BUG_ON(evictee != NULL);
					evictee = bh2;
				} else {
					bhs[out++] = bh2;
				}
			}
		}
		while (out < BH_LRU_SIZE)
			bhs[out++] = NULL;
		memcpy(lru->bhs, bhs, sizeof(bhs));
	}
	bh_lru_unlock();

	if (evictee)
		__brelse(evictee);
}

/*
 * Look up the bh in this cpu's LRU.  If it's there, move it to the head.
 */
static struct buffer_head *
lookup_bh_lru(struct block_device *bdev, sector_t block, unsigned size)
{
	struct buffer_head *ret = NULL;
	struct bh_lru *lru;
	unsigned int i;

	check_irqs_on();
	bh_lru_lock();
	lru = &__get_cpu_var(bh_lrus);
	for (i = 0; i < BH_LRU_SIZE; i++) {
		struct buffer_head *bh = lru->bhs[i];

		if (bh && bh->b_bdev == bdev &&
				bh->b_blocknr == block && bh->b_size == size) {
			if (i) {
				while (i) {
					lru->bhs[i] = lru->bhs[i - 1];
					i--;
				}
				lru->bhs[0] = bh;
			}
			get_bh(bh);
			ret = bh;
			break;
		}
	}
	bh_lru_unlock();
	return ret;
}

/*
 * Perform a pagecache lookup for the matching buffer.  If it's there, refresh
 * it in the LRU and mark it as accessed.  If it is not present then return
 * NULL
 */
// 该函数用于在页面缓存中查找一个匹配的缓冲区。如果找到了，会在LRU列表中刷新它并标记为已访问。如果没有找到，则返回 NULL。
struct buffer_head *
__find_get_block(struct block_device *bdev, sector_t block, unsigned size)
{
	// 尝试通过LRU机制快速查找buffer_head
	struct buffer_head *bh = lookup_bh_lru(bdev, block, size);

	// 如果LRU查找未成功，尝试慢速路径查找
	if (bh == NULL) {
		// 如果慢速查找成功，将该buffer_head安装到LRU中
		bh = __find_get_block_slow(bdev, block);
		if (bh)
			bh_lru_install(bh);
	}
	// 如果找到了buffer_head，更新它的访问时间
	if (bh)
		// 返回找到的buffer_head，如果都没找到则返回NULL
		touch_buffer(bh);
	return bh;
}
EXPORT_SYMBOL(__find_get_block);

/*
 * __getblk will locate (and, if necessary, create) the buffer_head
 * which corresponds to the passed block_device, block and size. The
 * returned buffer has its reference count incremented.
 *
 * __getblk() cannot fail - it just keeps trying.  If you pass it an
 * illegal block number, __getblk() will happily return a buffer_head
 * which represents the non-existent block.  Very weird.
 *
 * __getblk() will lock up the machine if grow_dev_page's try_to_free_buffers()
 * attempt is failing.  FIXME, perhaps?
 */
/*
 * __getblk 会定位（必要时创建）对应于传入的块设备、块号和大小的 buffer_head。
 * 返回的缓冲区其引用计数被增加。
 *
 * __getblk() 不会失败 - 它会不断尝试。如果你传递一个非法的块号，__getblk()
 * 会愉快地返回一个代表不存在的块的 buffer_head。这非常奇怪。
 *
 * 如果 grow_dev_page 的 try_to_free_buffers() 尝试失败，__getblk() 会锁定机器。
 * 或许这是个需要修复的问题（FIXME）？
 */
// 接收块设备、块号和大小作为参数
struct buffer_head *
__getblk(struct block_device *bdev, sector_t block, unsigned size)
{
	// 尝试查找已存在的缓冲区头
	struct buffer_head *bh = __find_get_block(bdev, block, size);

	// 可能会休眠，因此确保不在原子上下文中调用
	might_sleep();
	// 如果没有找到缓冲区头
	if (bh == NULL)
		// 使用慢速方式获取缓冲区头
		bh = __getblk_slow(bdev, block, size);
	return bh;	// 返回找到或创建的缓冲区头
}
EXPORT_SYMBOL(__getblk);

/*
 * Do async read-ahead on a buffer..
 */
void __breadahead(struct block_device *bdev, sector_t block, unsigned size)
{
	struct buffer_head *bh = __getblk(bdev, block, size);
	if (likely(bh)) {
		ll_rw_block(READA, 1, &bh);
		brelse(bh);
	}
}
EXPORT_SYMBOL(__breadahead);

/**
 *  __bread() - reads a specified block and returns the bh
 *  @bdev: the block_device to read from
 *  @block: number of block
 *  @size: size (in bytes) to read
 * 
 *  Reads a specified block, and returns buffer head that contains it.
 *  It returns NULL if the block was unreadable.
 */
/**
 *  __bread() - 从指定的块设备读取一个指定的块，并返回包含该块的缓冲区头
 *  @bdev: 要读取的块设备
 *  @block: 块号
 *  @size: 要读取的大小（字节）
 * 
 *  该函数读取指定的块，并返回一个包含该块的缓冲区头。
 *  如果该块无法被读取，返回NULL。
 */
struct buffer_head *
__bread(struct block_device *bdev, sector_t block, unsigned size)
{
	// 从块设备bdev获取编号为block、大小为size的块
	struct buffer_head *bh = __getblk(bdev, block, size);

	// 如果成功获取到缓冲区头，且缓冲区头的数据不是最新的
	if (likely(bh) && !buffer_uptodate(bh))
		 // 进行慢速读取，确保数据是最新的
		bh = __bread_slow(bh);
	return bh;	// 返回包含数据的缓冲区头
}
EXPORT_SYMBOL(__bread);

/*
 * invalidate_bh_lrus() is called rarely - but not only at unmount.
 * This doesn't race because it runs in each cpu either in irq
 * or with preempt disabled.
 */
static void invalidate_bh_lru(void *arg)
{
	struct bh_lru *b = &get_cpu_var(bh_lrus);
	int i;

	for (i = 0; i < BH_LRU_SIZE; i++) {
		brelse(b->bhs[i]);
		b->bhs[i] = NULL;
	}
	put_cpu_var(bh_lrus);
}
	
void invalidate_bh_lrus(void)
{
	on_each_cpu(invalidate_bh_lru, NULL, 1);
}
EXPORT_SYMBOL_GPL(invalidate_bh_lrus);

void set_bh_page(struct buffer_head *bh,
		struct page *page, unsigned long offset)
{
	bh->b_page = page;
	BUG_ON(offset >= PAGE_SIZE);
	if (PageHighMem(page))
		/*
		 * This catches illegal uses and preserves the offset:
		 */
		bh->b_data = (char *)(0 + offset);
	else
		bh->b_data = page_address(page) + offset;
}
EXPORT_SYMBOL(set_bh_page);

/*
 * Called when truncating a buffer on a page completely.
 */
static void discard_buffer(struct buffer_head * bh)
{
	lock_buffer(bh);
	clear_buffer_dirty(bh);
	bh->b_bdev = NULL;
	clear_buffer_mapped(bh);
	clear_buffer_req(bh);
	clear_buffer_new(bh);
	clear_buffer_delay(bh);
	clear_buffer_unwritten(bh);
	unlock_buffer(bh);
}

/**
 * block_invalidatepage - invalidate part of all of a buffer-backed page
 *
 * @page: the page which is affected
 * @offset: the index of the truncation point
 *
 * block_invalidatepage() is called when all or part of the page has become
 * invalidatedby a truncate operation.
 *
 * block_invalidatepage() does not have to release all buffers, but it must
 * ensure that no dirty buffer is left outside @offset and that no I/O
 * is underway against any of the blocks which are outside the truncation
 * point.  Because the caller is about to free (and possibly reuse) those
 * blocks on-disk.
 */
void block_invalidatepage(struct page *page, unsigned long offset)
{
	struct buffer_head *head, *bh, *next;
	unsigned int curr_off = 0;

	BUG_ON(!PageLocked(page));
	if (!page_has_buffers(page))
		goto out;

	head = page_buffers(page);
	bh = head;
	do {
		unsigned int next_off = curr_off + bh->b_size;
		next = bh->b_this_page;

		/*
		 * is this block fully invalidated?
		 */
		if (offset <= curr_off)
			discard_buffer(bh);
		curr_off = next_off;
		bh = next;
	} while (bh != head);

	/*
	 * We release buffers only if the entire page is being invalidated.
	 * The get_block cached value has been unconditionally invalidated,
	 * so real IO is not possible anymore.
	 */
	if (offset == 0)
		try_to_release_page(page, 0);
out:
	return;
}
EXPORT_SYMBOL(block_invalidatepage);

/*
 * We attach and possibly dirty the buffers atomically wrt
 * __set_page_dirty_buffers() via private_lock.  try_to_free_buffers
 * is already excluded via the page lock.
 */
/*
 * 我们原子地关联并可能使缓冲区变脏，相对于 __set_page_dirty_buffers() 通过 private_lock。
 * try_to_free_buffers 已通过页面锁被排除。
 */
void create_empty_buffers(struct page *page,
			unsigned long blocksize, unsigned long b_state)
{
	struct buffer_head *bh, *head, *tail;

	// 为给定的页分配一个新的缓冲区链
	head = alloc_page_buffers(page, blocksize, 1);
	bh = head;
	do {
		// 将给定的状态标志位设置到缓冲区状态中
		bh->b_state |= b_state;
		tail = bh;
		// 移动到该页的下一个缓冲区
		bh = bh->b_this_page;
	} while (bh);	// 循环直到遍历完所有缓冲区
	// 将缓冲区链形成一个环形结构，tail的下一个指向head
	tail->b_this_page = head;

	// 锁定页映射的私有锁
	spin_lock(&page->mapping->private_lock);
	// 如果页面已更新或已脏，则设置缓冲区的相应状态
	if (PageUptodate(page) || PageDirty(page)) {
		bh = head;
		do {
			if (PageDirty(page))
				set_buffer_dirty(bh);	// 标记缓冲区为脏
			if (PageUptodate(page))
				set_buffer_uptodate(bh);	// 标记缓冲区已更新
			// 移动到下一个缓冲区
			bh = bh->b_this_page;
		} while (bh != head);	// 循环直到回到起点
	}
	// 将缓冲区链附加到页面
	attach_page_buffers(page, head);
	// 解锁页映射的私有锁
	spin_unlock(&page->mapping->private_lock);
}
EXPORT_SYMBOL(create_empty_buffers);

/*
 * We are taking a block for data and we don't want any output from any
 * buffer-cache aliases starting from return from that function and
 * until the moment when something will explicitly mark the buffer
 * dirty (hopefully that will not happen until we will free that block ;-)
 * We don't even need to mark it not-uptodate - nobody can expect
 * anything from a newly allocated buffer anyway. We used to used
 * unmap_buffer() for such invalidation, but that was wrong. We definitely
 * don't want to mark the alias unmapped, for example - it would confuse
 * anyone who might pick it with bread() afterwards...
 *
 * Also..  Note that bforget() doesn't lock the buffer.  So there can
 * be writeout I/O going on against recently-freed buffers.  We don't
 * wait on that I/O in bforget() - it's more efficient to wait on the I/O
 * only if we really need to.  That happens here.
 */
void unmap_underlying_metadata(struct block_device *bdev, sector_t block)
{
	struct buffer_head *old_bh;

	might_sleep();

	old_bh = __find_get_block_slow(bdev, block);
	if (old_bh) {
		clear_buffer_dirty(old_bh);
		wait_on_buffer(old_bh);
		clear_buffer_req(old_bh);
		__brelse(old_bh);
	}
}
EXPORT_SYMBOL(unmap_underlying_metadata);

/*
 * NOTE! All mapped/uptodate combinations are valid:
 *
 *	Mapped	Uptodate	Meaning
 *
 *	No	No		"unknown" - must do get_block()
 *	No	Yes		"hole" - zero-filled
 *	Yes	No		"allocated" - allocated on disk, not read in
 *	Yes	Yes		"valid" - allocated and up-to-date in memory.
 *
 * "Dirty" is valid only with the last case (mapped+uptodate).
 */

/*
 * While block_write_full_page is writing back the dirty buffers under
 * the page lock, whoever dirtied the buffers may decide to clean them
 * again at any time.  We handle that by only looking at the buffer
 * state inside lock_buffer().
 *
 * If block_write_full_page() is called for regular writeback
 * (wbc->sync_mode == WB_SYNC_NONE) then it will redirty a page which has a
 * locked buffer.   This only can happen if someone has written the buffer
 * directly, with submit_bh().  At the address_space level PageWriteback
 * prevents this contention from occurring.
 *
 * If block_write_full_page() is called with wbc->sync_mode ==
 * WB_SYNC_ALL, the writes are posted using WRITE_SYNC_PLUG; this
 * causes the writes to be flagged as synchronous writes, but the
 * block device queue will NOT be unplugged, since usually many pages
 * will be pushed to the out before the higher-level caller actually
 * waits for the writes to be completed.  The various wait functions,
 * such as wait_on_writeback_range() will ultimately call sync_page()
 * which will ultimately call blk_run_backing_dev(), which will end up
 * unplugging the device queue.
 */
/*
 * 注意！所有映射/更新的组合都是有效的：
 *
 *	映射	更新	含义
 *
 *	否	否	"未知" - 必须执行get_block()
 *	否	是	"空洞" - 已填充零
 *	是	否	"已分配" - 已在磁盘上分配，未读入
 *	是	是	"有效" - 已分配并在内存中更新。
 *
 * "脏"仅在最后一种情况（映射+更新）下有效。
 */

/*
 * 当block_write_full_page正在写回页面下的脏缓冲区时，
 * 弄脏缓冲区的任何人都可以随时决定再次清理它们。
 * 我们通过仅在lock_buffer()内部查看缓冲区状态来处理这一点。
 *
 * 如果为常规写回调用block_write_full_page()
 * (wbc->sync_mode == WB_SYNC_NONE)，那么它将重新脏化具有锁定缓冲区的页面。
 * 这只能发生在有人直接通过submit_bh()写入缓冲区的情况。
 * 在地址空间级别，PageWriteback阻止了这种争用的发生。
 *
 * 如果使用wbc->sync_mode == WB_SYNC_ALL调用block_write_full_page()，
 * 写入使用WRITE_SYNC_PLUG标记; 这会导致写入被标记为同步写入，
 * 但块设备队列将不会被拔出，因为通常许多页面将在更高级别的调用者实际等待
 * 写入完成之前被推送出去。各种等待函数，如wait_on_writeback_range()，
 * 最终将调用sync_page()，最终将调用blk_run_backing_dev()，从而最终拔出设备队列。
 */
static int __block_write_full_page(struct inode *inode, struct page *page,
			get_block_t *get_block, struct writeback_control *wbc,
			bh_end_io_t *handler)
{
	int err;	// 用于存储错误码
	sector_t block;	// 当前处理的块的索引
	sector_t last_block;	// 最后一个块的索引
	 // 用于遍历页面内所有缓冲头的指针
	struct buffer_head *bh, *head;
	// 块大小
	const unsigned blocksize = 1 << inode->i_blkbits;
	// 正在处理的缓冲区数量
	int nr_underway = 0;
	// 写操作类型，决定是否使用插件式写入
	int write_op = (wbc->sync_mode == WB_SYNC_ALL ?
			WRITE_SYNC_PLUG : WRITE);

	// 确保页面已锁定
	BUG_ON(!PageLocked(page));

	// 计算最后一个块的索引
	last_block = (i_size_read(inode) - 1) >> inode->i_blkbits;

	// 检查页面是否有缓冲区，没有则创建空缓冲区
	if (!page_has_buffers(page)) {
		create_empty_buffers(page, blocksize,
					(1 << BH_Dirty)|(1 << BH_Uptodate));
	}

	/*
	 * Be very careful.  We have no exclusion from __set_page_dirty_buffers
	 * here, and the (potentially unmapped) buffers may become dirty at
	 * any time.  If a buffer becomes dirty here after we've inspected it
	 * then we just miss that fact, and the page stays dirty.
	 *
	 * Buffers outside i_size may be dirtied by __set_page_dirty_buffers;
	 * handle that here by just cleaning them.
	 */
	/*
	 * 非常小心。我们这里没有从__set_page_dirty_buffers中排除，
	 * 而且（可能未映射的）缓冲区可能随时变脏。如果在我们检查后缓冲区变脏，
	 * 那么我们就会错过这一事实，页面将保持脏状态。
	 *
	 * i_size之外的缓冲区可能会通过__set_page_dirty_buffers变脏；
	 * 在这里通过清理它们来处理。
	 */

	// 初始化块索引
	block = (sector_t)page->index << (PAGE_CACHE_SHIFT - inode->i_blkbits);
	head = page_buffers(page);
	bh = head;

	/*
	 * Get all the dirty buffers mapped to disk addresses and
	 * handle any aliases from the underlying blockdev's mapping.
	 */
	/*
	 * 获取所有映射到磁盘地址的脏缓冲区，并处理来自底层块设备映射的任何别名。
	 */
	// 遍历所有缓冲区，处理每个缓冲区
	do {
		if (block > last_block) {	// 如果块索引超出文件大小
			/*
			 * mapped buffers outside i_size will occur, because
			 * this page can be outside i_size when there is a
			 * truncate in progress.
			 */
			/*
			 * The buffer was zeroed by block_write_full_page()
			 */
			/*
			 * 如果页面在i_size之外，可能会发生映射缓冲区，
			 * 因为当进行截断时，此页面可能在i_size之外。
			 */
			/*
			 * 该缓冲区由block_write_full_page()清零
			 */
			clear_buffer_dirty(bh);	// 清除脏标记
			set_buffer_uptodate(bh);	// 设置为更新状态
		} else if ((!buffer_mapped(bh) || buffer_delay(bh)) &&
			   buffer_dirty(bh)) {	// 如果缓冲区未映射或延迟，并且是脏的
			WARN_ON(bh->b_size != blocksize);	// 块大小不匹配警告
			// 映射块
			err = get_block(inode, block, bh, 1);
			if (err)	// 错误处理
				goto recover;
			clear_buffer_delay(bh);	// 清除延迟标记
			if (buffer_new(bh)) {		// 如果是新块
				/* blockdev mappings never come here */
				/* 块设备映射从不来这里 */
				clear_buffer_new(bh);	// 清除新块标记
				// 取消映射底层元数据
				unmap_underlying_metadata(bh->b_bdev,
							bh->b_blocknr);
			}
		}
		bh = bh->b_this_page;
		block++;
	} while (bh != head);

	do {
		// 如果缓冲区未映射，则跳过
		if (!buffer_mapped(bh))
			continue;
		/*
		 * If it's a fully non-blocking write attempt and we cannot
		 * lock the buffer then redirty the page.  Note that this can
		 * potentially cause a busy-wait loop from writeback threads
		 * and kswapd activity, but those code paths have their own
		 * higher-level throttling.
		 */
		/*
		 * 如果这是一个完全非阻塞的写入尝试，并且我们不能锁定缓冲区，
		 * 则重新使页面变脏。注意，这可能导致来自写回线程和kswapd活动的
		 * 忙等循环，但这些代码路径有自己的高级别节流。
		 */
		// 如果需要同步或非非阻塞模式
		if (wbc->sync_mode != WB_SYNC_NONE || !wbc->nonblocking) {
			lock_buffer(bh);	// 锁定缓冲区
		} else if (!trylock_buffer(bh)) {	// 尝试锁定缓冲区，失败则重新脏化页面
			redirty_page_for_writepage(wbc, page);
			continue;
		}
		// 测试并清除脏标记
		if (test_clear_buffer_dirty(bh)) {
			// 标记为异步写并设置结束IO处理函数
			mark_buffer_async_write_endio(bh, handler);
		} else {
			// 解锁缓冲区
			unlock_buffer(bh);
		}
	} while ((bh = bh->b_this_page) != head);

	/*
	 * The page and its buffers are protected by PageWriteback(), so we can
	 * drop the bh refcounts early.
	 */
	/*
	 * 页面及其缓冲区受PageWriteback()保护，因此我们可以提前删除bh引用计数。
	 */
	// 设置页面为写回状态
	BUG_ON(PageWriteback(page));
	set_page_writeback(page);

	do {
		struct buffer_head *next = bh->b_this_page;
		if (buffer_async_write(bh)) {	// 如果缓冲区是异步写
			submit_bh(write_op, bh);	// 提交缓冲头进行IO操作
			nr_underway++;	// 正在处理的缓冲区数量增加
		}
		bh = next;
	} while (bh != head);
	unlock_page(page);	// 解锁页面

	err = 0;	// 设置错误码为0
done:
	if (nr_underway == 0) {	// 如果没有正在进行的缓冲区
		/*
		 * The page was marked dirty, but the buffers were
		 * clean.  Someone wrote them back by hand with
		 * ll_rw_block/submit_bh.  A rare case.
		 */
		/*
		 * 页面被标记为脏，但缓冲区是干净的。有人用ll_rw_block/submit_bh手动
		 * 写回了它们。这是一个罕见的情况。
		 */
		end_page_writeback(page);	// 结束页面的写回

		/*
		 * The page and buffer_heads can be released at any time from
		 * here on.
		 */
		/*
		 * 从这里开始，页面和缓冲区可以随时释放。
		 */
	}
	return err;

recover:
	/*
	 * ENOSPC, or some other error.  We may already have added some
	 * blocks to the file, so we need to write these out to avoid
	 * exposing stale data.
	 * The page is currently locked and not marked for writeback
	 */
	/*
	 * ENOSPC或其他错误。我们可能已经向文件添加了一些块，所以我们需要将这些块
	 * 写出以避免暴露陈旧数据。当前页面已锁定且未标记为写回。
	 */
	bh = head;
	/* Recovery: lock and submit the mapped buffers */
	/* 恢复：锁定并提交映射的缓冲区 */
	do {
		// 如果缓冲区已映射且脏
		if (buffer_mapped(bh) && buffer_dirty(bh) &&
		    !buffer_delay(bh)) {
			lock_buffer(bh);	// 锁定缓冲区
			// 标记为异步写并设置结束IO处理函数
			mark_buffer_async_write_endio(bh, handler);
		} else {
			/*
			 * The buffer may have been set dirty during
			 * attachment to a dirty page.
			 */
			/*
			 * 缓冲区可能在附加到脏页面时被设置为脏。
			 */
			clear_buffer_dirty(bh);	// 清除脏标记
		}
	} while ((bh = bh->b_this_page) != head);
	SetPageError(page);	// 设置页面错误
	BUG_ON(PageWriteback(page));	// 断言检查页面不应处于写回状态
	// 设置页面映射的错误
	mapping_set_error(page->mapping, err);
	// 设置页面为写回状态
	set_page_writeback(page);
	do {
		struct buffer_head *next = bh->b_this_page;
		// 如果缓冲区是异步写
		if (buffer_async_write(bh)) {
			clear_buffer_dirty(bh);	// 清除脏标记
			submit_bh(write_op, bh);	// 提交缓冲头进行IO操作
			nr_underway++;	// 正在处理的缓冲区数量增加
		}
		bh = next;
	} while (bh != head);
	unlock_page(page);	// 解锁页面
	goto done;	// 跳转到完成处理部分
}

/*
 * If a page has any new buffers, zero them out here, and mark them uptodate
 * and dirty so they'll be written out (in order to prevent uninitialised
 * block data from leaking). And clear the new bit.
 */
void page_zero_new_buffers(struct page *page, unsigned from, unsigned to)
{
	unsigned int block_start, block_end;
	struct buffer_head *head, *bh;

	BUG_ON(!PageLocked(page));
	if (!page_has_buffers(page))
		return;

	bh = head = page_buffers(page);
	block_start = 0;
	do {
		block_end = block_start + bh->b_size;

		if (buffer_new(bh)) {
			if (block_end > from && block_start < to) {
				if (!PageUptodate(page)) {
					unsigned start, size;

					start = max(from, block_start);
					size = min(to, block_end) - start;

					zero_user(page, start, size);
					set_buffer_uptodate(bh);
				}

				clear_buffer_new(bh);
				mark_buffer_dirty(bh);
			}
		}

		block_start = block_end;
		bh = bh->b_this_page;
	} while (bh != head);
}
EXPORT_SYMBOL(page_zero_new_buffers);

/**
 * 1. **检查条件**：确保页面已锁定，起始和结束位置在页缓存大小范围内，并且起始位置不大于结束位置。
 * 2. **页面缓冲区初始化**：如果页面没有缓冲区，为其创建空缓冲区。
 * 3. **遍历所有缓冲区**：对于页面中的每个缓冲区，根据需要更新或映射。
 * - 如果缓冲区的位置在要写的范围之外，只更新状态。
 * - 对于需要写入的部分，如果缓冲区未映射，则尝试映射它。
 * - 对于新映射的缓冲区，如果所在页面已更新，直接标记缓冲区已更新；如果是新分配的块，可能需要对其进行初始化。
 * 4. **处理直接I/O请求**：如果发出了读取请求（对未更新的缓冲区进行读取以填充数据），则等待这些请求完成，并检查结果。
 * 5. **异常处理**：如果在任何步骤中遇到错误（例如映射失败或读取失败），则清除页面中受影响部分的内容，并返回错误码。
 * 
 * 此函数主要在文件系统中实现写入操作前的准备工作，包括分配和映射块、处理部分写入和同步页面内容。
 */
static int __block_prepare_write(struct inode *inode, struct page *page,
		unsigned from, unsigned to, get_block_t *get_block)
{
	// 块起始和结束位置
	unsigned block_start, block_end;
	sector_t block;	// 块号
	int err = 0;		// 错误码初始化为0
	// 块大小和块位数
	unsigned blocksize, bbits;
	// 缓冲头结构体
	struct buffer_head *bh, *head, *wait[2], **wait_bh=wait;

	BUG_ON(!PageLocked(page));	// 确保页面被锁定
	BUG_ON(from > PAGE_CACHE_SIZE);	// 起始位置不能超过页缓存大小
	BUG_ON(to > PAGE_CACHE_SIZE);		// 结束位置不能超过页缓存大小
	BUG_ON(from > to);	// 起始位置不能大于结束位置

	// 计算块大小
	blocksize = 1 << inode->i_blkbits;
	// 如果页面没有缓冲区，则创建
	if (!page_has_buffers(page))
		create_empty_buffers(page, blocksize, 0);
	// 获取页面的第一个缓冲区头
	head = page_buffers(page);

	bbits = inode->i_blkbits;	// 获取块位数
	// 计算块号
	block = (sector_t)page->index << (PAGE_CACHE_SHIFT - bbits);

	for(bh = head, block_start = 0; bh != head || !block_start;
	    block++, block_start=block_end, bh = bh->b_this_page) {
		// 计算块结束位置
		block_end = block_start + blocksize;
		if (block_end <= from || block_start >= to) {
			if (PageUptodate(page)) {	// 如果页面是最新的
				if (!buffer_uptodate(bh))
					// 设置缓冲区为最新
					set_buffer_uptodate(bh);
			}
			continue;
		}
		// 如果是新缓冲区，清除新缓冲区标记
		if (buffer_new(bh))
			clear_buffer_new(bh);
		// 如果缓冲区没有映射
		if (!buffer_mapped(bh)) {
			// 警告块大小不一致
			WARN_ON(bh->b_size != blocksize);
			// 获取块映射
			err = get_block(inode, block, bh, 1);
			if (err)
				break;
			if (buffer_new(bh)) {
				// 取消元数据映射
				unmap_underlying_metadata(bh->b_bdev,
							bh->b_blocknr);
				if (PageUptodate(page)) {
					clear_buffer_new(bh);
					set_buffer_uptodate(bh);
					// 标记缓冲区为脏
					mark_buffer_dirty(bh);
					continue;
				}
				if (block_end > to || block_start < from)
					// 清零用户段
					zero_user_segments(page,
						to, block_end,
						block_start, from);
				continue;
			}
		}
		if (PageUptodate(page)) {
			if (!buffer_uptodate(bh))
				// 设置缓冲区为最新
				set_buffer_uptodate(bh);
			continue; 
		}
		if (!buffer_uptodate(bh) && !buffer_delay(bh) &&
		    !buffer_unwritten(bh) &&
		     (block_start < from || block_end > to)) {
			ll_rw_block(READ, 1, &bh);	// 读取块
			*wait_bh++=bh;		// 添加到等待列表
		}
	}
	/*
	 * If we issued read requests - let them complete.
	 */
	/*
	 * 如果发出了读请求 - 等待它们完成。
	 */
	while(wait_bh > wait) {
		 // 等待缓冲区的I/O操作完成
		wait_on_buffer(*--wait_bh);
		 // 如果缓冲区未更新成功，则返回I/O错误
		if (!buffer_uptodate(*wait_bh))
			err = -EIO;
	}
	// 如果出错，则将新缓冲区区间清零
	if (unlikely(err))
		page_zero_new_buffers(page, from, to);
	return err;	// 返回处理结果
}

// 在写操作完成后提交对页面的更改。该函数处理页面内的每个缓冲头，更新它们的状态，
// 并确保如果页面完全更新，则将其标记为最新。这有助于优化后续的文件访问操作，避免不必要的磁盘读取。
static int __block_commit_write(struct inode *inode, struct page *page,
		unsigned from, unsigned to)
{
	unsigned block_start, block_end;
	int partial = 0;	// 部分写入的标记
	unsigned blocksize;	// 块大小
	// 缓冲头指针
	struct buffer_head *bh, *head;

	// 计算块大小
	blocksize = 1 << inode->i_blkbits;

	// 遍历页面中的所有缓冲头
	for(bh = head = page_buffers(page), block_start = 0;
	    bh != head || !block_start;
	    block_start=block_end, bh = bh->b_this_page) {
		block_end = block_start + blocksize;
		// 如果当前块不在写入区域内
		if (block_end <= from || block_start >= to) {
			// 如果缓冲块不是最新的，标记为部分写入
			if (!buffer_uptodate(bh))
				partial = 1;
		} else {	// 如果当前块在写入区域内
			set_buffer_uptodate(bh);	// 标记缓冲块为最新的
			mark_buffer_dirty(bh);		// 标记缓冲块为脏的
		}
		clear_buffer_new(bh);				// 清除缓冲块的“新”状态
	}

	/*
	 * If this is a partial write which happened to make all buffers
	 * uptodate then we can optimize away a bogus readpage() for
	 * the next read(). Here we 'discover' whether the page went
	 * uptodate as a result of this (potentially partial) write.
	 */
	/*
	 * 如果这是一个部分写入并且导致所有缓冲块都为最新的，那么我们可以优化掉
	 * 下一次读取时的无效readpage()调用。这里我们“发现”页面是否因为这次
	 * （可能是部分的）写入而变为最新的。
	 */w
	if (!partial)
		SetPageUptodate(page);	// 如果没有部分写入，标记页面为最新的
	return 0;	// 返回成功
}

/*
 * block_write_begin takes care of the basic task of block allocation and
 * bringing partial write blocks uptodate first.
 *
 * If *pagep is not NULL, then block_write_begin uses the locked page
 * at *pagep rather than allocating its own. In this case, the page will
 * not be unlocked or deallocated on failure.
 */
/*
 * block_write_begin 负责块分配的基本任务，并首先将部分写入块更新到最新状态。
 *
 * 如果 *pagep 不是 NULL，则 block_write_begin 使用 *pagep 中的锁定页面，
 * 而不是分配自己的页面。在这种情况下，失败时页面不会被解锁或释放。
 */
int block_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata,
			get_block_t *get_block)
{
	// 获取映射关联的inode结构
	struct inode *inode = mapping->host;
	int status = 0;	// 初始化状态变量
	struct page *page;	// 用于操作的页面指针
	pgoff_t index;			// 页面索引
	unsigned start, end;	// 写入操作的开始和结束位置
	int ownpage = 0;			// 标记是否拥有页面

	// 计算页内偏移
	index = pos >> PAGE_CACHE_SHIFT;
	// 计算开始位置在页面中的偏移
	start = pos & (PAGE_CACHE_SIZE - 1);
	// 计算结束位置
	end = start + len;

	page = *pagep;	// 获取传入的页面指针
	if (page == NULL) {	// 如果页面不存在，需要新获取一个页面
		ownpage = 1;	// 标记自己拥有页面
		// 尝试获取缓存页面
		page = grab_cache_page_write_begin(mapping, index, flags);
		if (!page) {
			// 页面获取失败，返回内存不足错误
			status = -ENOMEM;
			goto out;
		}
		*pagep = page;
	} else
	 	// 如果页面已存在，确保该页面已被锁定
		BUG_ON(!PageLocked(page));

	 // 准备写入，设置必要的缓冲区头部等
	status = __block_prepare_write(inode, page, start, end, get_block);
	// 如果准备写入失败，清除页面的更新状态
	if (unlikely(status)) {
		ClearPageUptodate(page);

		// 如果是函数内部分配的页面
		if (ownpage) {
			// 解锁页面
			unlock_page(page);
			// 释放页面引用
			page_cache_release(page);
			// 清空页面指针
			*pagep = NULL;

			/*
			 * prepare_write() may have instantiated a few blocks
			 * outside i_size.  Trim these off again. Don't need
			 * i_size_read because we hold i_mutex.
			 */
			/*
		 * prepare_write() 可能已经实例化了一些超出 i_size 的块。
		 * 如果发生这种情况，需要再次修剪它们。不需要 i_size_read，
		 * 因为我们持有 i_mutex。
		 */
			if (pos + len > inode->i_size)
				// 调整文件大小以匹配实际写入的数据
				vmtruncate(inode, inode->i_size);
		}
	}

out:
	return status;
}
EXPORT_SYMBOL(block_write_begin);

// 主要负责文件的写操作结束部分。block_write_end 函数处理实际的写入结束逻辑，包括处理短写和页不最新的情况。
int block_write_end(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct page *page, void *fsdata)
{
	// 获取inode对象，该inode与写入的文件关联
	struct inode *inode = mapping->host;
	unsigned start;

	// 计算页内偏移
	start = pos & (PAGE_CACHE_SIZE - 1);

	if (unlikely(copied < len)) {
		/*
		 * The buffers that were written will now be uptodate, so we
		 * don't have to worry about a readpage reading them and
		 * overwriting a partial write. However if we have encountered
		 * a short write and only partially written into a buffer, it
		 * will not be marked uptodate, so a readpage might come in and
		 * destroy our partial write.
		 *
		 * Do the simplest thing, and just treat any short write to a
		 * non uptodate page as a zero-length write, and force the
		 * caller to redo the whole thing.
		 */
		/*
		 * 已写入的缓冲区现在将是最新的，因此我们不必担心readpage读取它们并
		 * 覆盖部分写入。但是，如果我们遇到短写并且只部分写入缓冲区，则它将不会
		 * 标记为最新，因此readpage可能进入并破坏我们的部分写入。
		 *
		 * 做最简单的事情，对未更新页面的任何短写都视为零长度写入，并强制调用者
		 * 重新执行整个操作。
		 */
		// 如果页面不是最新的，将复制长度设置为0
		if (!PageUptodate(page))
			copied = 0;

		page_zero_new_buffers(page, start+copied, start+len);
	}
	// 清零页面中新缓冲区的剩余部分
	flush_dcache_page(page);

	/* This could be a short (even 0-length) commit */
	/* 这可能是一个短的（甚至是0长度的）提交 */
	// 提交写入操作
	__block_commit_write(inode, page, start, start+copied);

	return copied;	// 返回复制的字节数
}
EXPORT_SYMBOL(block_write_end);

// 通用的写结束处理函数，它首先调用 block_write_end 函数，然后处理文件大小的更新和释放资源等后续操作。
int generic_write_end(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct page *page, void *fsdata)
{
	// 获取inode对象
	struct inode *inode = mapping->host;
	int i_size_changed = 0;	// 文件大小改变标志

	// 调用block_write_end进行写入操作
	copied = block_write_end(file, mapping, pos, len, copied, page, fsdata);

	/*
	 * No need to use i_size_read() here, the i_size
	 * cannot change under us because we hold i_mutex.
	 *
	 * But it's important to update i_size while still holding page lock:
	 * page writeout could otherwise come in and zero beyond i_size.
	 */
	/*
	 * 这里不需要使用i_size_read()，因为i_size在我们持有i_mutex的情况下不能更改。
	 *
	 * 但在仍持有页锁的情况下更新i_size很重要：否则页面写出可能会进入并在i_size之后归零。
	 */
	if (pos+copied > inode->i_size) {
		i_size_write(inode, pos+copied);	// 更新inode的i_size
		i_size_changed = 1;	// 标记文件大小已更改
	}

	unlock_page(page);	// 解锁页面
	page_cache_release(page);	// 释放页面缓存

	/*
	 * Don't mark the inode dirty under page lock. First, it unnecessarily
	 * makes the holding time of page lock longer. Second, it forces lock
	 * ordering of page lock and transaction start for journaling
	 * filesystems.
	 */
	/*
	 * 不要在持有页锁的情况下标记inode为脏。首先，它不必要地延长了持有页锁的时间。
	 * 其次，对于日志文件系统，它强制了页锁和事务开始的锁定顺序。
	 */
	// 如果文件大小已更改，将inode标记为脏
	if (i_size_changed)
		mark_inode_dirty(inode);

	return copied;	// 返回复制的字节数
}
EXPORT_SYMBOL(generic_write_end);

/*
 * block_is_partially_uptodate checks whether buffers within a page are
 * uptodate or not.
 *
 * Returns true if all buffers which correspond to a file portion
 * we want to read are uptodate.
 */
int block_is_partially_uptodate(struct page *page, read_descriptor_t *desc,
					unsigned long from)
{
	struct inode *inode = page->mapping->host;
	unsigned block_start, block_end, blocksize;
	unsigned to;
	struct buffer_head *bh, *head;
	int ret = 1;

	if (!page_has_buffers(page))
		return 0;

	blocksize = 1 << inode->i_blkbits;
	to = min_t(unsigned, PAGE_CACHE_SIZE - from, desc->count);
	to = from + to;
	if (from < blocksize && to > PAGE_CACHE_SIZE - blocksize)
		return 0;

	head = page_buffers(page);
	bh = head;
	block_start = 0;
	do {
		block_end = block_start + blocksize;
		if (block_end > from && block_start < to) {
			if (!buffer_uptodate(bh)) {
				ret = 0;
				break;
			}
			if (block_end >= to)
				break;
		}
		block_start = block_end;
		bh = bh->b_this_page;
	} while (bh != head);

	return ret;
}
EXPORT_SYMBOL(block_is_partially_uptodate);

/*
 * Generic "read page" function for block devices that have the normal
 * get_block functionality. This is most of the block device filesystems.
 * Reads the page asynchronously --- the unlock_buffer() and
 * set/clear_buffer_uptodate() functions propagate buffer state into the
 * page struct once IO has completed.
 */
int block_read_full_page(struct page *page, get_block_t *get_block)
{
	struct inode *inode = page->mapping->host;
	sector_t iblock, lblock;
	struct buffer_head *bh, *head, *arr[MAX_BUF_PER_PAGE];
	unsigned int blocksize;
	int nr, i;
	int fully_mapped = 1;

	BUG_ON(!PageLocked(page));
	blocksize = 1 << inode->i_blkbits;
	if (!page_has_buffers(page))
		create_empty_buffers(page, blocksize, 0);
	head = page_buffers(page);

	iblock = (sector_t)page->index << (PAGE_CACHE_SHIFT - inode->i_blkbits);
	lblock = (i_size_read(inode)+blocksize-1) >> inode->i_blkbits;
	bh = head;
	nr = 0;
	i = 0;

	do {
		if (buffer_uptodate(bh))
			continue;

		if (!buffer_mapped(bh)) {
			int err = 0;

			fully_mapped = 0;
			if (iblock < lblock) {
				WARN_ON(bh->b_size != blocksize);
				err = get_block(inode, iblock, bh, 0);
				if (err)
					SetPageError(page);
			}
			if (!buffer_mapped(bh)) {
				zero_user(page, i * blocksize, blocksize);
				if (!err)
					set_buffer_uptodate(bh);
				continue;
			}
			/*
			 * get_block() might have updated the buffer
			 * synchronously
			 */
			if (buffer_uptodate(bh))
				continue;
		}
		arr[nr++] = bh;
	} while (i++, iblock++, (bh = bh->b_this_page) != head);

	if (fully_mapped)
		SetPageMappedToDisk(page);

	if (!nr) {
		/*
		 * All buffers are uptodate - we can set the page uptodate
		 * as well. But not if get_block() returned an error.
		 */
		if (!PageError(page))
			SetPageUptodate(page);
		unlock_page(page);
		return 0;
	}

	/* Stage two: lock the buffers */
	for (i = 0; i < nr; i++) {
		bh = arr[i];
		lock_buffer(bh);
		mark_buffer_async_read(bh);
	}

	/*
	 * Stage 3: start the IO.  Check for uptodateness
	 * inside the buffer lock in case another process reading
	 * the underlying blockdev brought it uptodate (the sct fix).
	 */
	for (i = 0; i < nr; i++) {
		bh = arr[i];
		if (buffer_uptodate(bh))
			end_buffer_async_read(bh, 1);
		else
			submit_bh(READ, bh);
	}
	return 0;
}
EXPORT_SYMBOL(block_read_full_page);

/* utility function for filesystems that need to do work on expanding
 * truncates.  Uses filesystem pagecache writes to allow the filesystem to
 * deal with the hole.  
 */
int generic_cont_expand_simple(struct inode *inode, loff_t size)
{
	struct address_space *mapping = inode->i_mapping;
	struct page *page;
	void *fsdata;
	int err;

	err = inode_newsize_ok(inode, size);
	if (err)
		goto out;

	err = pagecache_write_begin(NULL, mapping, size, 0,
				AOP_FLAG_UNINTERRUPTIBLE|AOP_FLAG_CONT_EXPAND,
				&page, &fsdata);
	if (err)
		goto out;

	err = pagecache_write_end(NULL, mapping, size, 0, 0, page, fsdata);
	BUG_ON(err > 0);

out:
	return err;
}
EXPORT_SYMBOL(generic_cont_expand_simple);

static int cont_expand_zero(struct file *file, struct address_space *mapping,
			    loff_t pos, loff_t *bytes)
{
	struct inode *inode = mapping->host;
	unsigned blocksize = 1 << inode->i_blkbits;
	struct page *page;
	void *fsdata;
	pgoff_t index, curidx;
	loff_t curpos;
	unsigned zerofrom, offset, len;
	int err = 0;

	index = pos >> PAGE_CACHE_SHIFT;
	offset = pos & ~PAGE_CACHE_MASK;

	while (index > (curidx = (curpos = *bytes)>>PAGE_CACHE_SHIFT)) {
		zerofrom = curpos & ~PAGE_CACHE_MASK;
		if (zerofrom & (blocksize-1)) {
			*bytes |= (blocksize-1);
			(*bytes)++;
		}
		len = PAGE_CACHE_SIZE - zerofrom;

		err = pagecache_write_begin(file, mapping, curpos, len,
						AOP_FLAG_UNINTERRUPTIBLE,
						&page, &fsdata);
		if (err)
			goto out;
		zero_user(page, zerofrom, len);
		err = pagecache_write_end(file, mapping, curpos, len, len,
						page, fsdata);
		if (err < 0)
			goto out;
		BUG_ON(err != len);
		err = 0;

		balance_dirty_pages_ratelimited(mapping);
	}

	/* page covers the boundary, find the boundary offset */
	if (index == curidx) {
		zerofrom = curpos & ~PAGE_CACHE_MASK;
		/* if we will expand the thing last block will be filled */
		if (offset <= zerofrom) {
			goto out;
		}
		if (zerofrom & (blocksize-1)) {
			*bytes |= (blocksize-1);
			(*bytes)++;
		}
		len = offset - zerofrom;

		err = pagecache_write_begin(file, mapping, curpos, len,
						AOP_FLAG_UNINTERRUPTIBLE,
						&page, &fsdata);
		if (err)
			goto out;
		zero_user(page, zerofrom, len);
		err = pagecache_write_end(file, mapping, curpos, len, len,
						page, fsdata);
		if (err < 0)
			goto out;
		BUG_ON(err != len);
		err = 0;
	}
out:
	return err;
}

/*
 * For moronic filesystems that do not allow holes in file.
 * We may have to extend the file.
 */
int cont_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata,
			get_block_t *get_block, loff_t *bytes)
{
	struct inode *inode = mapping->host;
	unsigned blocksize = 1 << inode->i_blkbits;
	unsigned zerofrom;
	int err;

	err = cont_expand_zero(file, mapping, pos, bytes);
	if (err)
		goto out;

	zerofrom = *bytes & ~PAGE_CACHE_MASK;
	if (pos+len > *bytes && zerofrom & (blocksize-1)) {
		*bytes |= (blocksize-1);
		(*bytes)++;
	}

	*pagep = NULL;
	err = block_write_begin(file, mapping, pos, len,
				flags, pagep, fsdata, get_block);
out:
	return err;
}
EXPORT_SYMBOL(cont_write_begin);

int block_prepare_write(struct page *page, unsigned from, unsigned to,
			get_block_t *get_block)
{
	struct inode *inode = page->mapping->host;
	int err = __block_prepare_write(inode, page, from, to, get_block);
	if (err)
		ClearPageUptodate(page);
	return err;
}
EXPORT_SYMBOL(block_prepare_write);

int block_commit_write(struct page *page, unsigned from, unsigned to)
{
	struct inode *inode = page->mapping->host;
	__block_commit_write(inode,page,from,to);
	return 0;
}
EXPORT_SYMBOL(block_commit_write);

/*
 * block_page_mkwrite() is not allowed to change the file size as it gets
 * called from a page fault handler when a page is first dirtied. Hence we must
 * be careful to check for EOF conditions here. We set the page up correctly
 * for a written page which means we get ENOSPC checking when writing into
 * holes and correct delalloc and unwritten extent mapping on filesystems that
 * support these features.
 *
 * We are not allowed to take the i_mutex here so we have to play games to
 * protect against truncate races as the page could now be beyond EOF.  Because
 * vmtruncate() writes the inode size before removing pages, once we have the
 * page lock we can determine safely if the page is beyond EOF. If it is not
 * beyond EOF, then the page is guaranteed safe against truncation until we
 * unlock the page.
 */
int
block_page_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf,
		   get_block_t get_block)
{
	struct page *page = vmf->page;
	struct inode *inode = vma->vm_file->f_path.dentry->d_inode;
	unsigned long end;
	loff_t size;
	int ret = VM_FAULT_NOPAGE; /* make the VM retry the fault */

	lock_page(page);
	size = i_size_read(inode);
	if ((page->mapping != inode->i_mapping) ||
	    (page_offset(page) > size)) {
		/* page got truncated out from underneath us */
		unlock_page(page);
		goto out;
	}

	/* page is wholly or partially inside EOF */
	if (((page->index + 1) << PAGE_CACHE_SHIFT) > size)
		end = size & ~PAGE_CACHE_MASK;
	else
		end = PAGE_CACHE_SIZE;

	ret = block_prepare_write(page, 0, end, get_block);
	if (!ret)
		ret = block_commit_write(page, 0, end);

	if (unlikely(ret)) {
		unlock_page(page);
		if (ret == -ENOMEM)
			ret = VM_FAULT_OOM;
		else /* -ENOSPC, -EIO, etc */
			ret = VM_FAULT_SIGBUS;
	} else
		ret = VM_FAULT_LOCKED;

out:
	return ret;
}
EXPORT_SYMBOL(block_page_mkwrite);

/*
 * nobh_write_begin()'s prereads are special: the buffer_heads are freed
 * immediately, while under the page lock.  So it needs a special end_io
 * handler which does not touch the bh after unlocking it.
 */
static void end_buffer_read_nobh(struct buffer_head *bh, int uptodate)
{
	__end_buffer_read_notouch(bh, uptodate);
}

/*
 * Attach the singly-linked list of buffers created by nobh_write_begin, to
 * the page (converting it to circular linked list and taking care of page
 * dirty races).
 */
static void attach_nobh_buffers(struct page *page, struct buffer_head *head)
{
	struct buffer_head *bh;

	BUG_ON(!PageLocked(page));

	spin_lock(&page->mapping->private_lock);
	bh = head;
	do {
		if (PageDirty(page))
			set_buffer_dirty(bh);
		if (!bh->b_this_page)
			bh->b_this_page = head;
		bh = bh->b_this_page;
	} while (bh != head);
	attach_page_buffers(page, head);
	spin_unlock(&page->mapping->private_lock);
}

/*
 * On entry, the page is fully not uptodate.
 * On exit the page is fully uptodate in the areas outside (from,to)
 */
int nobh_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata,
			get_block_t *get_block)
{
	struct inode *inode = mapping->host;
	const unsigned blkbits = inode->i_blkbits;
	const unsigned blocksize = 1 << blkbits;
	struct buffer_head *head, *bh;
	struct page *page;
	pgoff_t index;
	unsigned from, to;
	unsigned block_in_page;
	unsigned block_start, block_end;
	sector_t block_in_file;
	int nr_reads = 0;
	int ret = 0;
	int is_mapped_to_disk = 1;

	index = pos >> PAGE_CACHE_SHIFT;
	from = pos & (PAGE_CACHE_SIZE - 1);
	to = from + len;

	page = grab_cache_page_write_begin(mapping, index, flags);
	if (!page)
		return -ENOMEM;
	*pagep = page;
	*fsdata = NULL;

	if (page_has_buffers(page)) {
		unlock_page(page);
		page_cache_release(page);
		*pagep = NULL;
		return block_write_begin(file, mapping, pos, len, flags, pagep,
					fsdata, get_block);
	}

	if (PageMappedToDisk(page))
		return 0;

	/*
	 * Allocate buffers so that we can keep track of state, and potentially
	 * attach them to the page if an error occurs. In the common case of
	 * no error, they will just be freed again without ever being attached
	 * to the page (which is all OK, because we're under the page lock).
	 *
	 * Be careful: the buffer linked list is a NULL terminated one, rather
	 * than the circular one we're used to.
	 */
	head = alloc_page_buffers(page, blocksize, 0);
	if (!head) {
		ret = -ENOMEM;
		goto out_release;
	}

	block_in_file = (sector_t)page->index << (PAGE_CACHE_SHIFT - blkbits);

	/*
	 * We loop across all blocks in the page, whether or not they are
	 * part of the affected region.  This is so we can discover if the
	 * page is fully mapped-to-disk.
	 */
	for (block_start = 0, block_in_page = 0, bh = head;
		  block_start < PAGE_CACHE_SIZE;
		  block_in_page++, block_start += blocksize, bh = bh->b_this_page) {
		int create;

		block_end = block_start + blocksize;
		bh->b_state = 0;
		create = 1;
		if (block_start >= to)
			create = 0;
		ret = get_block(inode, block_in_file + block_in_page,
					bh, create);
		if (ret)
			goto failed;
		if (!buffer_mapped(bh))
			is_mapped_to_disk = 0;
		if (buffer_new(bh))
			unmap_underlying_metadata(bh->b_bdev, bh->b_blocknr);
		if (PageUptodate(page)) {
			set_buffer_uptodate(bh);
			continue;
		}
		if (buffer_new(bh) || !buffer_mapped(bh)) {
			zero_user_segments(page, block_start, from,
							to, block_end);
			continue;
		}
		if (buffer_uptodate(bh))
			continue;	/* reiserfs does this */
		if (block_start < from || block_end > to) {
			lock_buffer(bh);
			bh->b_end_io = end_buffer_read_nobh;
			submit_bh(READ, bh);
			nr_reads++;
		}
	}

	if (nr_reads) {
		/*
		 * The page is locked, so these buffers are protected from
		 * any VM or truncate activity.  Hence we don't need to care
		 * for the buffer_head refcounts.
		 */
		for (bh = head; bh; bh = bh->b_this_page) {
			wait_on_buffer(bh);
			if (!buffer_uptodate(bh))
				ret = -EIO;
		}
		if (ret)
			goto failed;
	}

	if (is_mapped_to_disk)
		SetPageMappedToDisk(page);

	*fsdata = head; /* to be released by nobh_write_end */

	return 0;

failed:
	BUG_ON(!ret);
	/*
	 * Error recovery is a bit difficult. We need to zero out blocks that
	 * were newly allocated, and dirty them to ensure they get written out.
	 * Buffers need to be attached to the page at this point, otherwise
	 * the handling of potential IO errors during writeout would be hard
	 * (could try doing synchronous writeout, but what if that fails too?)
	 */
	attach_nobh_buffers(page, head);
	page_zero_new_buffers(page, from, to);

out_release:
	unlock_page(page);
	page_cache_release(page);
	*pagep = NULL;

	if (pos + len > inode->i_size)
		vmtruncate(inode, inode->i_size);

	return ret;
}
EXPORT_SYMBOL(nobh_write_begin);

int nobh_write_end(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct page *page, void *fsdata)
{
	struct inode *inode = page->mapping->host;
	struct buffer_head *head = fsdata;
	struct buffer_head *bh;
	BUG_ON(fsdata != NULL && page_has_buffers(page));

	if (unlikely(copied < len) && head)
		attach_nobh_buffers(page, head);
	if (page_has_buffers(page))
		return generic_write_end(file, mapping, pos, len,
					copied, page, fsdata);

	SetPageUptodate(page);
	set_page_dirty(page);
	if (pos+copied > inode->i_size) {
		i_size_write(inode, pos+copied);
		mark_inode_dirty(inode);
	}

	unlock_page(page);
	page_cache_release(page);

	while (head) {
		bh = head;
		head = head->b_this_page;
		free_buffer_head(bh);
	}

	return copied;
}
EXPORT_SYMBOL(nobh_write_end);

/*
 * nobh_writepage() - based on block_full_write_page() except
 * that it tries to operate without attaching bufferheads to
 * the page.
 */
int nobh_writepage(struct page *page, get_block_t *get_block,
			struct writeback_control *wbc)
{
	struct inode * const inode = page->mapping->host;
	loff_t i_size = i_size_read(inode);
	const pgoff_t end_index = i_size >> PAGE_CACHE_SHIFT;
	unsigned offset;
	int ret;

	/* Is the page fully inside i_size? */
	if (page->index < end_index)
		goto out;

	/* Is the page fully outside i_size? (truncate in progress) */
	offset = i_size & (PAGE_CACHE_SIZE-1);
	if (page->index >= end_index+1 || !offset) {
		/*
		 * The page may have dirty, unmapped buffers.  For example,
		 * they may have been added in ext3_writepage().  Make them
		 * freeable here, so the page does not leak.
		 */
#if 0
		/* Not really sure about this  - do we need this ? */
		if (page->mapping->a_ops->invalidatepage)
			page->mapping->a_ops->invalidatepage(page, offset);
#endif
		unlock_page(page);
		return 0; /* don't care */
	}

	/*
	 * The page straddles i_size.  It must be zeroed out on each and every
	 * writepage invocation because it may be mmapped.  "A file is mapped
	 * in multiples of the page size.  For a file that is not a multiple of
	 * the  page size, the remaining memory is zeroed when mapped, and
	 * writes to that region are not written out to the file."
	 */
	zero_user_segment(page, offset, PAGE_CACHE_SIZE);
out:
	ret = mpage_writepage(page, get_block, wbc);
	if (ret == -EAGAIN)
		ret = __block_write_full_page(inode, page, get_block, wbc,
					      end_buffer_async_write);
	return ret;
}
EXPORT_SYMBOL(nobh_writepage);

int nobh_truncate_page(struct address_space *mapping,
			loff_t from, get_block_t *get_block)
{
	pgoff_t index = from >> PAGE_CACHE_SHIFT;
	unsigned offset = from & (PAGE_CACHE_SIZE-1);
	unsigned blocksize;
	sector_t iblock;
	unsigned length, pos;
	struct inode *inode = mapping->host;
	struct page *page;
	struct buffer_head map_bh;
	int err;

	blocksize = 1 << inode->i_blkbits;
	length = offset & (blocksize - 1);

	/* Block boundary? Nothing to do */
	if (!length)
		return 0;

	length = blocksize - length;
	iblock = (sector_t)index << (PAGE_CACHE_SHIFT - inode->i_blkbits);

	page = grab_cache_page(mapping, index);
	err = -ENOMEM;
	if (!page)
		goto out;

	if (page_has_buffers(page)) {
has_buffers:
		unlock_page(page);
		page_cache_release(page);
		return block_truncate_page(mapping, from, get_block);
	}

	/* Find the buffer that contains "offset" */
	pos = blocksize;
	while (offset >= pos) {
		iblock++;
		pos += blocksize;
	}

	map_bh.b_size = blocksize;
	map_bh.b_state = 0;
	err = get_block(inode, iblock, &map_bh, 0);
	if (err)
		goto unlock;
	/* unmapped? It's a hole - nothing to do */
	if (!buffer_mapped(&map_bh))
		goto unlock;

	/* Ok, it's mapped. Make sure it's up-to-date */
	if (!PageUptodate(page)) {
		err = mapping->a_ops->readpage(NULL, page);
		if (err) {
			page_cache_release(page);
			goto out;
		}
		lock_page(page);
		if (!PageUptodate(page)) {
			err = -EIO;
			goto unlock;
		}
		if (page_has_buffers(page))
			goto has_buffers;
	}
	zero_user(page, offset, length);
	set_page_dirty(page);
	err = 0;

unlock:
	unlock_page(page);
	page_cache_release(page);
out:
	return err;
}
EXPORT_SYMBOL(nobh_truncate_page);

int block_truncate_page(struct address_space *mapping,
			loff_t from, get_block_t *get_block)
{
	pgoff_t index = from >> PAGE_CACHE_SHIFT;
	unsigned offset = from & (PAGE_CACHE_SIZE-1);
	unsigned blocksize;
	sector_t iblock;
	unsigned length, pos;
	struct inode *inode = mapping->host;
	struct page *page;
	struct buffer_head *bh;
	int err;

	blocksize = 1 << inode->i_blkbits;
	length = offset & (blocksize - 1);

	/* Block boundary? Nothing to do */
	if (!length)
		return 0;

	length = blocksize - length;
	iblock = (sector_t)index << (PAGE_CACHE_SHIFT - inode->i_blkbits);
	
	page = grab_cache_page(mapping, index);
	err = -ENOMEM;
	if (!page)
		goto out;

	if (!page_has_buffers(page))
		create_empty_buffers(page, blocksize, 0);

	/* Find the buffer that contains "offset" */
	bh = page_buffers(page);
	pos = blocksize;
	while (offset >= pos) {
		bh = bh->b_this_page;
		iblock++;
		pos += blocksize;
	}

	err = 0;
	if (!buffer_mapped(bh)) {
		WARN_ON(bh->b_size != blocksize);
		err = get_block(inode, iblock, bh, 0);
		if (err)
			goto unlock;
		/* unmapped? It's a hole - nothing to do */
		if (!buffer_mapped(bh))
			goto unlock;
	}

	/* Ok, it's mapped. Make sure it's up-to-date */
	if (PageUptodate(page))
		set_buffer_uptodate(bh);

	if (!buffer_uptodate(bh) && !buffer_delay(bh) && !buffer_unwritten(bh)) {
		err = -EIO;
		ll_rw_block(READ, 1, &bh);
		wait_on_buffer(bh);
		/* Uhhuh. Read error. Complain and punt. */
		if (!buffer_uptodate(bh))
			goto unlock;
	}

	zero_user(page, offset, length);
	mark_buffer_dirty(bh);
	err = 0;

unlock:
	unlock_page(page);
	page_cache_release(page);
out:
	return err;
}
EXPORT_SYMBOL(block_truncate_page);

/*
 * The generic ->writepage function for buffer-backed address_spaces
 * this form passes in the end_io handler used to finish the IO.
 */
/*
 * 通用的 ->writepage 函数用于支持缓冲区的地址空间，
 * 本形式传入了用于完成 IO 的 end_io 处理函数。
 */
int block_write_full_page_endio(struct page *page, get_block_t *get_block,
			struct writeback_control *wbc, bh_end_io_t *handler)
{
	// 从页的映射获取关联的inode结构
	struct inode * const inode = page->mapping->host;
	// 读取inode的大小
	loff_t i_size = i_size_read(inode);
	// 计算索引页的终点
	const pgoff_t end_index = i_size >> PAGE_CACHE_SHIFT;
	unsigned offset;

	/* Is the page fully inside i_size? */
	/* 检查页面是否完全位于 i_size 内 */
	if (page->index < end_index)
		// 如果是，则调用底层写入函数
		return __block_write_full_page(inode, page, get_block, wbc,
					       handler);

	/* Is the page fully outside i_size? (truncate in progress) */
	/* 页面是否完全在 i_size 外部？（正在进行截断） */
	offset = i_size & (PAGE_CACHE_SIZE-1);	// 获取i_size的偏移量
	if (page->index >= end_index+1 || !offset) {
		/*
		 * The page may have dirty, unmapped buffers.  For example,
		 * they may have been added in ext3_writepage().  Make them
		 * freeable here, so the page does not leak.
		 */
		/*
		 * 页面可能有脏的、未映射的缓冲区。例如，
		 * 它们可能已经在 ext3_writepage() 中被添加。
		 * 在这里使它们可释放，以便页面不会泄漏。
		 */
		do_invalidatepage(page, 0);	// 使页面无效
		unlock_page(page);	// 解锁页面
		// 不关心结果
		return 0; /* don't care */
	}

	/*
	 * The page straddles i_size.  It must be zeroed out on each and every
	 * writepage invocation because it may be mmapped.  "A file is mapped
	 * in multiples of the page size.  For a file that is not a multiple of
	 * the  page size, the remaining memory is zeroed when mapped, and
	 * writes to that region are not written out to the file."
	 */
	/*
	 * 页面跨越了 i_size。每次调用 writepage 时必须将其清零，
	 * 因为它可能被内存映射了。"文件被映射为页面大小的整数倍。
	 * 对于不是页面大小倍数的文件，映射时剩余的内存将被清零，
	 * 并且对该区域的写入不会写出到文件。"
	 */
	// 清零跨越部分
	zero_user_segment(page, offset, PAGE_CACHE_SIZE);
	// 再次尝试写入页面
	return __block_write_full_page(inode, page, get_block, wbc, handler);
}
EXPORT_SYMBOL(block_write_full_page_endio);

/*
 * The generic ->writepage function for buffer-backed address_spaces
 */
/*
 * 通用的 ->writepage 函数用于支持缓冲区的地址空间
 */
int block_write_full_page(struct page *page, get_block_t *get_block,
			struct writeback_control *wbc)
{
	// 调用带 end_io 处理函数的版本
	return block_write_full_page_endio(page, get_block, wbc,
					   end_buffer_async_write);
}
EXPORT_SYMBOL(block_write_full_page);

/*
 * 计算文件的逻辑块映射到磁盘上的哪个物理块
 */
sector_t generic_block_bmap(struct address_space *mapping, sector_t block,
			    get_block_t *get_block)
{
	struct buffer_head tmp;
	// 从地址空间映射中获取 inode
	struct inode *inode = mapping->host;
	tmp.b_state = 0;		// 初始化缓冲头的状态
	tmp.b_blocknr = 0;	// 初始化块号
	// 设置缓冲头的大小
	tmp.b_size = 1 << inode->i_blkbits;
	// 获取块的映射
	get_block(inode, block, &tmp, 0);
	// 返回映射后的块号
	return tmp.b_blocknr;
}
EXPORT_SYMBOL(generic_block_bmap);

// 在I/O操作完成后被调用，根据操作结果设置缓冲头状态，并释放bio。
static void end_bio_bh_io_sync(struct bio *bio, int err)
{
	struct buffer_head *bh = bio->bi_private;
	
	// 如果发生错误"EOPNOTSUPP"，设置相关的错误状态标记
	if (err == -EOPNOTSUPP) {
		set_bit(BIO_EOPNOTSUPP, &bio->bi_flags);
		set_bit(BH_Eopnotsupp, &bh->b_state);
	}

	// 如果设置了BIO_QUIET标志，设置BH_Quiet状态
	if (unlikely (test_bit(BIO_QUIET,&bio->bi_flags)))
		set_bit(BH_Quiet, &bh->b_state);

	// 调用b_end_io回调函数来结束处理，并根据BIO_UPTODATE位来判断是否成功
	bh->b_end_io(bh, test_bit(BIO_UPTODATE, &bio->bi_flags));
	// 释放bio结构
	bio_put(bio);
}

// submit_bh初始化和提交bio结构以进行I/O操作。它处理缓冲头状态，确定是否添加写屏障，并在完成后释放bio。
int submit_bh(int rw, struct buffer_head * bh)
{
	struct bio *bio;
	int ret = 0;

	// 检查缓冲头的一致性
	BUG_ON(!buffer_locked(bh));
	BUG_ON(!buffer_mapped(bh));
	BUG_ON(!bh->b_end_io);
	BUG_ON(buffer_delay(bh));
	BUG_ON(buffer_unwritten(bh));

	/*
	 * Mask in barrier bit for a write (could be either a WRITE or a
	 * WRITE_SYNC
	 */
	// 对写入操作添加写屏障
	if (buffer_ordered(bh) && (rw & WRITE))
		rw |= WRITE_BARRIER;

	/*
	 * Only clear out a write error when rewriting
	 */
	// 清除写错误标记以重新写入
	if (test_set_buffer_req(bh) && (rw & WRITE))
		clear_buffer_write_io_error(bh);

	/*
	 * from here on down, it's all bio -- do the initial mapping,
	 * submit_bio -> generic_make_request may further map this bio around
	 */
	// 分配bio结构，设置其字段，为数据传输做准备
	bio = bio_alloc(GFP_NOIO, 1);

	// 设置扇区号和设备
	bio->bi_sector = bh->b_blocknr * (bh->b_size >> 9);
	bio->bi_bdev = bh->b_bdev;
	bio->bi_io_vec[0].bv_page = bh->b_page;
	bio->bi_io_vec[0].bv_len = bh->b_size;
	bio->bi_io_vec[0].bv_offset = bh_offset(bh);

	bio->bi_vcnt = 1;
	bio->bi_idx = 0;
	bio->bi_size = bh->b_size;

	bio->bi_end_io = end_bio_bh_io_sync;
	bio->bi_private = bh;

	// 提交bio进行I/O操作
	bio_get(bio);
	submit_bio(rw, bio);

	// 检查是否支持该操作
	if (bio_flagged(bio, BIO_EOPNOTSUPP))
		ret = -EOPNOTSUPP;

	// 释放bio结构
	bio_put(bio);
	return ret;
}
EXPORT_SYMBOL(submit_bh);

/**
 * ll_rw_block: low-level access to block devices (DEPRECATED)
 * @rw: whether to %READ or %WRITE or %SWRITE or maybe %READA (readahead)
 * @nr: number of &struct buffer_heads in the array
 * @bhs: array of pointers to &struct buffer_head
 *
 * ll_rw_block() takes an array of pointers to &struct buffer_heads, and
 * requests an I/O operation on them, either a %READ or a %WRITE.  The third
 * %SWRITE is like %WRITE only we make sure that the *current* data in buffers
 * are sent to disk. The fourth %READA option is described in the documentation
 * for generic_make_request() which ll_rw_block() calls.
 *
 * This function drops any buffer that it cannot get a lock on (with the
 * BH_Lock state bit) unless SWRITE is required, any buffer that appears to be
 * clean when doing a write request, and any buffer that appears to be
 * up-to-date when doing read request.  Further it marks as clean buffers that
 * are processed for writing (the buffer cache won't assume that they are
 * actually clean until the buffer gets unlocked).
 *
 * ll_rw_block sets b_end_io to simple completion handler that marks
 * the buffer up-to-date (if approriate), unlocks the buffer and wakes
 * any waiters. 
 *
 * All of the buffers must be for the same device, and must also be a
 * multiple of the current approved size for the device.
 */
void ll_rw_block(int rw, int nr, struct buffer_head *bhs[])
{
	int i;

	for (i = 0; i < nr; i++) {
		struct buffer_head *bh = bhs[i];

		if (rw == SWRITE || rw == SWRITE_SYNC || rw == SWRITE_SYNC_PLUG)
			lock_buffer(bh);
		else if (!trylock_buffer(bh))
			continue;

		if (rw == WRITE || rw == SWRITE || rw == SWRITE_SYNC ||
		    rw == SWRITE_SYNC_PLUG) {
			if (test_clear_buffer_dirty(bh)) {
				bh->b_end_io = end_buffer_write_sync;
				get_bh(bh);
				if (rw == SWRITE_SYNC)
					submit_bh(WRITE_SYNC, bh);
				else
					submit_bh(WRITE, bh);
				continue;
			}
		} else {
			if (!buffer_uptodate(bh)) {
				bh->b_end_io = end_buffer_read_sync;
				get_bh(bh);
				submit_bh(rw, bh);
				continue;
			}
		}
		unlock_buffer(bh);
	}
}
EXPORT_SYMBOL(ll_rw_block);

/*
 * For a data-integrity writeout, we need to wait upon any in-progress I/O
 * and then start new I/O and then wait upon it.  The caller must have a ref on
 * the buffer_head.
 */
int sync_dirty_buffer(struct buffer_head *bh)
{
	int ret = 0;

	WARN_ON(atomic_read(&bh->b_count) < 1);
	lock_buffer(bh);
	if (test_clear_buffer_dirty(bh)) {
		get_bh(bh);
		bh->b_end_io = end_buffer_write_sync;
		ret = submit_bh(WRITE_SYNC, bh);
		wait_on_buffer(bh);
		if (buffer_eopnotsupp(bh)) {
			clear_buffer_eopnotsupp(bh);
			ret = -EOPNOTSUPP;
		}
		if (!ret && !buffer_uptodate(bh))
			ret = -EIO;
	} else {
		unlock_buffer(bh);
	}
	return ret;
}
EXPORT_SYMBOL(sync_dirty_buffer);

/*
 * try_to_free_buffers() checks if all the buffers on this particular page
 * are unused, and releases them if so.
 *
 * Exclusion against try_to_free_buffers may be obtained by either
 * locking the page or by holding its mapping's private_lock.
 *
 * If the page is dirty but all the buffers are clean then we need to
 * be sure to mark the page clean as well.  This is because the page
 * may be against a block device, and a later reattachment of buffers
 * to a dirty page will set *all* buffers dirty.  Which would corrupt
 * filesystem data on the same device.
 *
 * The same applies to regular filesystem pages: if all the buffers are
 * clean then we set the page clean and proceed.  To do that, we require
 * total exclusion from __set_page_dirty_buffers().  That is obtained with
 * private_lock.
 *
 * try_to_free_buffers() is non-blocking.
 */
static inline int buffer_busy(struct buffer_head *bh)
{
	return atomic_read(&bh->b_count) |
		(bh->b_state & ((1 << BH_Dirty) | (1 << BH_Lock)));
}

static int
drop_buffers(struct page *page, struct buffer_head **buffers_to_free)
{
	struct buffer_head *head = page_buffers(page);
	struct buffer_head *bh;

	bh = head;
	do {
		if (buffer_write_io_error(bh) && page->mapping)
			set_bit(AS_EIO, &page->mapping->flags);
		if (buffer_busy(bh))
			goto failed;
		bh = bh->b_this_page;
	} while (bh != head);

	do {
		struct buffer_head *next = bh->b_this_page;

		if (bh->b_assoc_map)
			__remove_assoc_queue(bh);
		bh = next;
	} while (bh != head);
	*buffers_to_free = head;
	__clear_page_buffers(page);
	return 1;
failed:
	return 0;
}

int try_to_free_buffers(struct page *page)
{
	struct address_space * const mapping = page->mapping;
	struct buffer_head *buffers_to_free = NULL;
	int ret = 0;

	BUG_ON(!PageLocked(page));
	if (PageWriteback(page))
		return 0;

	if (mapping == NULL) {		/* can this still happen? */
		ret = drop_buffers(page, &buffers_to_free);
		goto out;
	}

	spin_lock(&mapping->private_lock);
	ret = drop_buffers(page, &buffers_to_free);

	/*
	 * If the filesystem writes its buffers by hand (eg ext3)
	 * then we can have clean buffers against a dirty page.  We
	 * clean the page here; otherwise the VM will never notice
	 * that the filesystem did any IO at all.
	 *
	 * Also, during truncate, discard_buffer will have marked all
	 * the page's buffers clean.  We discover that here and clean
	 * the page also.
	 *
	 * private_lock must be held over this entire operation in order
	 * to synchronise against __set_page_dirty_buffers and prevent the
	 * dirty bit from being lost.
	 */
	if (ret)
		cancel_dirty_page(page, PAGE_CACHE_SIZE);
	spin_unlock(&mapping->private_lock);
out:
	if (buffers_to_free) {
		struct buffer_head *bh = buffers_to_free;

		do {
			struct buffer_head *next = bh->b_this_page;
			free_buffer_head(bh);
			bh = next;
		} while (bh != buffers_to_free);
	}
	return ret;
}
EXPORT_SYMBOL(try_to_free_buffers);

void block_sync_page(struct page *page)
{
	struct address_space *mapping;

	smp_mb();
	mapping = page_mapping(page);
	if (mapping)
		blk_run_backing_dev(mapping->backing_dev_info, page);
}
EXPORT_SYMBOL(block_sync_page);

/*
 * There are no bdflush tunables left.  But distributions are
 * still running obsolete flush daemons, so we terminate them here.
 *
 * Use of bdflush() is deprecated and will be removed in a future kernel.
 * The `flush-X' kernel threads fully replace bdflush daemons and this call.
 */
SYSCALL_DEFINE2(bdflush, int, func, long, data)
{
	static int msg_count;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (msg_count < 5) {
		msg_count++;
		printk(KERN_INFO
			"warning: process `%s' used the obsolete bdflush"
			" system call\n", current->comm);
		printk(KERN_INFO "Fix your initscripts?\n");
	}

	if (func == 1)
		do_exit(0);
	return 0;
}

/*
 * Buffer-head allocation
 */
static struct kmem_cache *bh_cachep;

/*
 * Once the number of bh's in the machine exceeds this level, we start
 * stripping them in writeback.
 */
/*
 * 当机器中的缓冲头数量超过此水平时，我们开始在回写中剥离它们。
 */
static int max_buffer_heads;	// 定义一个静态变量存储最大缓冲头数量

int buffer_heads_over_limit;	 // 定义一个整型变量，用于表示是否超过最大缓冲头数量的限制

struct bh_accounting {
	/* 存活的缓冲头数量 */
	int nr;			/* Number of live bh's */
	/* 限制高速缓存行抖动 */
	int ratelimit;		/* Limit cacheline bouncing */
};

// 定义一个每个CPU的bh_accounting结构，初始值为 {0, 0}
static DEFINE_PER_CPU(struct bh_accounting, bh_accounting) = {0, 0};

// 重新计算缓冲头的状态
static void recalc_bh_state(void)
{
	int i;
	int tot = 0;	// 用于统计所有CPU的缓冲头数量

	// 如果当前CPU的缓冲头限制计数尚未达到4096，则返回
	if (__get_cpu_var(bh_accounting).ratelimit++ < 4096)
		return;
	// 重置当前CPU的缓冲头限制计数
	__get_cpu_var(bh_accounting).ratelimit = 0;
	// 遍历每一个在线的CPU
	for_each_online_cpu(i)
		// 累加每个CPU的缓冲头数量
		tot += per_cpu(bh_accounting, i).nr;
	// 更新是否超过最大缓冲头数量的标志
	buffer_heads_over_limit = (tot > max_buffer_heads);
}
	
// 分配一个缓冲头的函数
struct buffer_head *alloc_buffer_head(gfp_t gfp_flags)
{
	// 使用指定的内存分配标志从缓冲头缓存中分配一个缓冲头
	struct buffer_head *ret = kmem_cache_zalloc(bh_cachep, gfp_flags);
	if (ret) {
		// 初始化关联的缓冲列表头
		INIT_LIST_HEAD(&ret->b_assoc_buffers);
		// 获取当前CPU的bh_accounting变量并增加存活的缓冲头数量
		get_cpu_var(bh_accounting).nr++;
		// 重新计算缓冲头状态
		recalc_bh_state();
		// 释放当前CPU的bh_accounting变量
		put_cpu_var(bh_accounting);
	}
	// 返回新分配的缓冲头
	return ret;
}
EXPORT_SYMBOL(alloc_buffer_head);

void free_buffer_head(struct buffer_head *bh)
{
	BUG_ON(!list_empty(&bh->b_assoc_buffers));
	kmem_cache_free(bh_cachep, bh);
	get_cpu_var(bh_accounting).nr--;
	recalc_bh_state();
	put_cpu_var(bh_accounting);
}
EXPORT_SYMBOL(free_buffer_head);

// 当CPU退出时释放其所有的缓冲区头并清理相关的计数器
static void buffer_exit_cpu(int cpu)
{
	int i;
	// 获取当前CPU
	struct bh_lru *b = &per_cpu(bh_lrus, cpu);

	// 获取当前CPU
	for (i = 0; i < BH_LRU_SIZE; i++) {
		// 释放缓冲区头，减少其引用计数
		brelse(b->bhs[i]);
		 // 将指针设置为NULL，避免悬挂引用
		b->bhs[i] = NULL;
	}
	// 将当前CPU的缓冲区头计数合并到全局计数中
	get_cpu_var(bh_accounting).nr += per_cpu(bh_accounting, cpu).nr;
	// 清零当前CPU的缓冲区头计数
	per_cpu(bh_accounting, cpu).nr = 0;
	put_cpu_var(bh_accounting);	// 释放对变量的引用
}

// CPU状态改变时的通知处理函数，用于处理CPU的退出
static int buffer_cpu_notify(struct notifier_block *self,
			      unsigned long action, void *hcpu)
{
	// 如果CPU已经停止或冻结
	if (action == CPU_DEAD || action == CPU_DEAD_FROZEN)
	 	// 调用清理函数释放该CPU的资源
		buffer_exit_cpu((unsigned long)hcpu);
	return NOTIFY_OK;	// 返回通知已处理的状态码
}

/**
 * bh_uptodate_or_lock - Test whether the buffer is uptodate
 * @bh: struct buffer_head
 *
 * Return true if the buffer is up-to-date and false,
 * with the buffer locked, if not.
 */
int bh_uptodate_or_lock(struct buffer_head *bh)
{
	if (!buffer_uptodate(bh)) {
		lock_buffer(bh);
		if (!buffer_uptodate(bh))
			return 0;
		unlock_buffer(bh);
	}
	return 1;
}
EXPORT_SYMBOL(bh_uptodate_or_lock);

/**
 * bh_submit_read - Submit a locked buffer for reading
 * @bh: struct buffer_head
 *
 * Returns zero on success and -EIO on error.
 */
/**
 * bh_submit_read - 提交一个锁定的缓冲区进行读取
 * @bh: struct buffer_head
 *
 * 如果成功，返回0；如果出现错误，返回 -EIO。
 */
int bh_submit_read(struct buffer_head *bh)
{
	// 确保缓冲区已锁定，如果未锁定则触发BUG
	BUG_ON(!buffer_locked(bh));

	// 如果缓冲区已更新
	if (buffer_uptodate(bh)) {
		unlock_buffer(bh);	// 解锁缓冲区
		return 0;	// 返回成功
	}

	get_bh(bh);	// 增加缓冲区的引用计数
	// 设置读取完成后的回调函数
	bh->b_end_io = end_buffer_read_sync;
	// 提交缓冲区进行读取
	submit_bh(READ, bh);
	// 等待缓冲区的IO操作完成
	wait_on_buffer(bh);
	// 再次检查缓冲区是否已更新
	if (buffer_uptodate(bh))
		return 0;	// 返回成功
	return -EIO;	// 如果缓冲区未成功更新，返回I/O错误
}
EXPORT_SYMBOL(bh_submit_read);

/**
 * buffer_init - 初始化缓冲区系统
 */
void __init buffer_init(void)
{
	int nrpages;

	// 创建缓冲区头的内存缓
	bh_cachep = kmem_cache_create("buffer_head",
			sizeof(struct buffer_head), 0,
				(SLAB_RECLAIM_ACCOUNT|SLAB_PANIC|
				SLAB_MEM_SPREAD),
				NULL);

	/*
	 * Limit the bh occupancy to 10% of ZONE_NORMAL
	 */
	/*
 	 * 将缓冲区头的最大数限制在ZONE_NORMAL的10%
 	 */
	// 计算可用页数的10%
	nrpages = (nr_free_buffer_pages() * 10) / 100;
	// 根据每页可以容纳的缓冲区头数量计算最大缓冲区头数
	max_buffer_heads = nrpages * (PAGE_SIZE / sizeof(struct buffer_head));
	// 注册CPU热插拔通知函数，以便在CPU状态改变时作出响应
	hotcpu_notifier(buffer_cpu_notify, 0);
}
