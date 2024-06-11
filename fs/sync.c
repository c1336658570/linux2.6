/*
 * High-level sync()-related operations
 */

#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/writeback.h>
#include <linux/syscalls.h>
#include <linux/linkage.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/buffer_head.h>
#include <linux/backing-dev.h>
#include "internal.h"

#define VALID_FLAGS (SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE| \
			SYNC_FILE_RANGE_WAIT_AFTER)

/*
 * Do the filesystem syncing work. For simple filesystems
 * writeback_inodes_sb(sb) just dirties buffers with inodes so we have to
 * submit IO for these buffers via __sync_blockdev(). This also speeds up the
 * wait == 1 case since in that case write_inode() functions do
 * sync_dirty_buffer() and thus effectively write one block at a time.
 */
static int __sync_filesystem(struct super_block *sb, int wait)
{
	/*
	 * This should be safe, as we require bdi backing to actually
	 * write out data in the first place
	 */
	if (!sb->s_bdi || sb->s_bdi == &noop_backing_dev_info)
		return 0;

	if (sb->s_qcop && sb->s_qcop->quota_sync)
		sb->s_qcop->quota_sync(sb, -1, wait);

	if (wait)
		sync_inodes_sb(sb);
	else
		writeback_inodes_sb(sb);

	if (sb->s_op->sync_fs)
		sb->s_op->sync_fs(sb, wait);
	return __sync_blockdev(sb->s_bdev, wait);
}

/*
 * Write out and wait upon all dirty data associated with this
 * superblock.  Filesystem data as well as the underlying block
 * device.  Takes the superblock lock.
 */
int sync_filesystem(struct super_block *sb)
{
	int ret;

	/*
	 * We need to be protected against the filesystem going from
	 * r/o to r/w or vice versa.
	 */
	WARN_ON(!rwsem_is_locked(&sb->s_umount));

	/*
	 * No point in syncing out anything if the filesystem is read-only.
	 */
	if (sb->s_flags & MS_RDONLY)
		return 0;

	ret = __sync_filesystem(sb, 0);
	if (ret < 0)
		return ret;
	return __sync_filesystem(sb, 1);
}
EXPORT_SYMBOL_GPL(sync_filesystem);

/*
 * Sync all the data for all the filesystems (called by sys_sync() and
 * emergency sync)
 *
 * This operation is careful to avoid the livelock which could easily happen
 * if two or more filesystems are being continuously dirtied.  s_need_sync
 * is used only here.  We set it against all filesystems and then clear it as
 * we sync them.  So redirtied filesystems are skipped.
 *
 * But if process A is currently running sync_filesystems and then process B
 * calls sync_filesystems as well, process B will set all the s_need_sync
 * flags again, which will cause process A to resync everything.  Fix that with
 * a local mutex.
 */
static void sync_filesystems(int wait)
{
	struct super_block *sb;
	static DEFINE_MUTEX(mutex);

	mutex_lock(&mutex);		/* Could be down_interruptible */
	spin_lock(&sb_lock);
	list_for_each_entry(sb, &super_blocks, s_list)
		sb->s_need_sync = 1;

restart:
	list_for_each_entry(sb, &super_blocks, s_list) {
		if (!sb->s_need_sync)
			continue;
		sb->s_need_sync = 0;
		sb->s_count++;
		spin_unlock(&sb_lock);

		down_read(&sb->s_umount);
		if (!(sb->s_flags & MS_RDONLY) && sb->s_root && sb->s_bdi)
			__sync_filesystem(sb, wait);
		up_read(&sb->s_umount);

		/* restart only when sb is no longer on the list */
		spin_lock(&sb_lock);
		if (__put_super_and_need_restart(sb))
			goto restart;
	}
	spin_unlock(&sb_lock);
	mutex_unlock(&mutex);
}

/*
 * sync everything.  Start out by waking pdflush, because that writes back
 * all queues in parallel.
 */
SYSCALL_DEFINE0(sync)
{
	wakeup_flusher_threads(0);
	sync_filesystems(0);
	sync_filesystems(1);
	if (unlikely(laptop_mode))
		laptop_sync_completion();
	return 0;
}

static void do_sync_work(struct work_struct *work)
{
	/*
	 * Sync twice to reduce the possibility we skipped some inodes / pages
	 * because they were temporarily locked
	 */
	sync_filesystems(0);
	sync_filesystems(0);
	printk("Emergency Sync complete\n");
	kfree(work);
}

void emergency_sync(void)
{
	struct work_struct *work;

	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	if (work) {
		INIT_WORK(work, do_sync_work);
		schedule_work(work);
	}
}

/*
 * Generic function to fsync a file.
 *
 * filp may be NULL if called via the msync of a vma.
 */
int file_fsync(struct file *filp, struct dentry *dentry, int datasync)
{
	struct inode * inode = dentry->d_inode;
	struct super_block * sb;
	int ret, err;

	/* sync the inode to buffers */
	ret = write_inode_now(inode, 0);

	/* sync the superblock to buffers */
	sb = inode->i_sb;
	if (sb->s_dirt && sb->s_op->write_super)
		sb->s_op->write_super(sb);

	/* .. finally sync the buffers to disk */
	err = sync_blockdev(sb->s_bdev);
	if (!ret)
		ret = err;
	return ret;
}
EXPORT_SYMBOL(file_fsync);

/**
 * vfs_fsync_range - helper to sync a range of data & metadata to disk
 * @file:		file to sync
 * @dentry:		dentry of @file
 * @start:		offset in bytes of the beginning of data range to sync
 * @end:		offset in bytes of the end of data range (inclusive)
 * @datasync:		perform only datasync
 *
 * Write back data in range @start..@end and metadata for @file to disk.  If
 * @datasync is set only metadata needed to access modified file data is
 * written.
 *
 * In case this function is called from nfsd @file may be %NULL and
 * only @dentry is set.  This can only happen when the filesystem
 * implements the export_operations API.
 */
/**
 * vfs_fsync_range - 辅助函数，用于将数据和元数据的一个范围同步到磁盘
 * @file: 需要同步的文件
 * @dentry: file的目录项
 * @start: 要同步的数据范围开始的字节偏移
 * @end: 要同步的数据范围结束的字节偏移（包含此偏移）
 * @datasync: 只执行datasync操作
 *
 * 将 @start 到 @end 范围内的数据和 @file 的元数据写回磁盘。如果
 * 设置了 @datasync，则只写入访问修改后的文件数据所需的元数据。
 *
 * 如果此函数从 nfsd 调用，@file 可能为 %NULL，
 * 只设置了 @dentry。这只在文件系统实现了 export_operations API 时发生。
 */
int vfs_fsync_range(struct file *file, struct dentry *dentry, loff_t start,
		    loff_t end, int datasync)
{
	// 文件操作结构指针
	const struct file_operations *fop;
	// 地址空间结构指针
	struct address_space *mapping;
	int err, ret;		// 错误码和返回值

	/*
	 * Get mapping and operations from the file in case we have
	 * as file, or get the default values for them in case we
	 * don't have a struct file available.  Damn nfsd..
	 */
	/*
     * 从文件获取映射和操作，如果我们有文件；
     * 如果我们没有文件结构可用，则获取它们的默认值。
     * 这对于 nfsd 来说是一个问题。
     */
	if (file) {
		// 从 file 获取地址空间映射
		mapping = file->f_mapping;
		fop = file->f_op;	// 从 file 获取文件操作
	} else {
		// 从 dentry 的 inode 获取地址空间映射
		mapping = dentry->d_inode->i_mapping;
		// 从 dentry 的 inode 获取文件操作
		fop = dentry->d_inode->i_fop;
	}

	// 检查文件操作和同步函数是否存在
	if (!fop || !fop->fsync) {
		ret = -EINVAL;	// 如果不存在，返回错误
		goto out;	// 跳到函数末尾处理返回
	}

	// 写入并等待指定范围内的数据被写入
	ret = filemap_write_and_wait_range(mapping, start, end);

	/*
	 * We need to protect against concurrent writers, which could cause
	 * livelocks in fsync_buffers_list().
	 */
	/*
   * 我们需要防止并发写入者，它们可能导致在fsync_buffers_list()中死锁。
   */
	mutex_lock(&mapping->host->i_mutex);	// 锁定互斥量以防并发写入
	err = fop->fsync(file, dentry, datasync);	// 执行文件的同步操作
	if (!ret)
		ret = err;	// 如果之前没有错误，现在赋予新的错误或成功码
	// 解锁互斥量
	mutex_unlock(&mapping->host->i_mutex);

out:
	return ret;	// 返回结果
}
EXPORT_SYMBOL(vfs_fsync_range);

/**
 * vfs_fsync - perform a fsync or fdatasync on a file
 * @file:		file to sync
 * @dentry:		dentry of @file
 * @datasync:		only perform a fdatasync operation
 *
 * Write back data and metadata for @file to disk.  If @datasync is
 * set only metadata needed to access modified file data is written.
 *
 * In case this function is called from nfsd @file may be %NULL and
 * only @dentry is set.  This can only happen when the filesystem
 * implements the export_operations API.
 */
/**
 * vfs_fsync - 对一个文件执行 fsync 或 fdatasync 操作
 * @file: 需要同步的文件
 * @dentry: file的目录项
 * @datasync: 只执行 fdatasync 操作
 *
 * 将 @file 的数据和元数据写回到磁盘。如果设置了 @datasync，则只写入
 * 访问修改后的文件数据所需的元数据。
 *
 * 如果此函数从 nfsd 调用，@file 可能为 %NULL，
 * 只设置了 @dentry。这只在文件系统实现了 export_operations API 时发生。
 */
int vfs_fsync(struct file *file, struct dentry *dentry, int datasync)
{
	// 对整个文件进行同步，范围是从文件开始到最大长整型值
	return vfs_fsync_range(file, dentry, 0, LLONG_MAX, datasync);
}
EXPORT_SYMBOL(vfs_fsync);

// do_fsync - 执行文件的同步操作
static int do_fsync(unsigned int fd, int datasync)
{
	struct file *file;	// 定义文件结构指针
	int ret = -EBADF;		// 默认返回值设为 -EBADF，表示无效的文件描述符错误

	// 通过文件描述符获取文件结构
	file = fget(fd);
	if (file) {	// 如果文件结构有效
		// 对文件执行同步操作
		ret = vfs_fsync(file, file->f_path.dentry, datasync);
		fput(file);	// 释放文件结构
	}
	return ret;	// 返回操作结果
}

// 系统调用，对文件执行 fsync
SYSCALL_DEFINE1(fsync, unsigned int, fd)
{
	// 调用 do_fsync，传入 datasync 参数为 0（执行全同步）
	return do_fsync(fd, 0);
}

// 系统调用，对文件执行 fdatasync，不会回写被修改的元数据，
// 除非对于一些对于数据完整性检索有关的场景。例如，若仅是文件的
// 最后一次访问时间（st_atime）或最后一次修改时间（st_mtime）
// 发生变化是不需要同步元数据的，因为它不会影响文件数据块的检索，
// 若是文件的大小改变了（st_isize）则显然是需要同步元数据的，
// 若不同步则可能导致系统崩溃后无法检索修改的数据。
SYSCALL_DEFINE1(fdatasync, unsigned int, fd)
{
	// 调用 do_fsync，传入 datasync 参数为 1
	return do_fsync(fd, 1);
}

/**
 * generic_write_sync - perform syncing after a write if file / inode is sync
 * @file:	file to which the write happened
 * @pos:	offset where the write started
 * @count:	length of the write
 *
 * This is just a simple wrapper about our general syncing function.
 */
/**
 * generic_write_sync - 如果文件/索引节点需要同步，则在写操作后执行同步操作
 * @file: 发生写操作的文件
 * @pos: 写操作开始的偏移量
 * @count: 写操作的长度
 *
 * 这只是一个关于我们通用同步函数的简单封装。
 */
int generic_write_sync(struct file *file, loff_t pos, loff_t count)
{
	// 检查文件的标志位中是否没有设置 O_DSYNC 且 文件的索引节点不是同步模式
	if (!(file->f_flags & O_DSYNC) && !IS_SYNC(file->f_mapping->host))
		return 0;	// 如果都不需要同步，则直接返回0，不进行同步操作
	// 否则，执行范围同步，范围是从 pos 到 pos + count - 1
	return vfs_fsync_range(file, file->f_path.dentry, pos,
			       pos + count - 1,
			       (file->f_flags & __O_SYNC) ? 0 : 1);
	// 如果文件的标志位中设置了 __O_SYNC，则第五个参数为0，否则为1
}
EXPORT_SYMBOL(generic_write_sync);

/*
 * sys_sync_file_range() permits finely controlled syncing over a segment of
 * a file in the range offset .. (offset+nbytes-1) inclusive.  If nbytes is
 * zero then sys_sync_file_range() will operate from offset out to EOF.
 *
 * The flag bits are:
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE: wait upon writeout of all pages in the range
 * before performing the write.
 *
 * SYNC_FILE_RANGE_WRITE: initiate writeout of all those dirty pages in the
 * range which are not presently under writeback. Note that this may block for
 * significant periods due to exhaustion of disk request structures.
 *
 * SYNC_FILE_RANGE_WAIT_AFTER: wait upon writeout of all pages in the range
 * after performing the write.
 *
 * Useful combinations of the flag bits are:
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE: ensures that all pages
 * in the range which were dirty on entry to sys_sync_file_range() are placed
 * under writeout.  This is a start-write-for-data-integrity operation.
 *
 * SYNC_FILE_RANGE_WRITE: start writeout of all dirty pages in the range which
 * are not presently under writeout.  This is an asynchronous flush-to-disk
 * operation.  Not suitable for data integrity operations.
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE (or SYNC_FILE_RANGE_WAIT_AFTER): wait for
 * completion of writeout of all pages in the range.  This will be used after an
 * earlier SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE operation to wait
 * for that operation to complete and to return the result.
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE|SYNC_FILE_RANGE_WAIT_AFTER:
 * a traditional sync() operation.  This is a write-for-data-integrity operation
 * which will ensure that all pages in the range which were dirty on entry to
 * sys_sync_file_range() are committed to disk.
 *
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE and SYNC_FILE_RANGE_WAIT_AFTER will detect any
 * I/O errors or ENOSPC conditions and will return those to the caller, after
 * clearing the EIO and ENOSPC flags in the address_space.
 *
 * It should be noted that none of these operations write out the file's
 * metadata.  So unless the application is strictly performing overwrites of
 * already-instantiated disk blocks, there are no guarantees here that the data
 * will be available after a crash.
 */
SYSCALL_DEFINE(sync_file_range)(int fd, loff_t offset, loff_t nbytes,
				unsigned int flags)
{
	int ret;
	struct file *file;
	struct address_space *mapping;
	loff_t endbyte;			/* inclusive */
	int fput_needed;
	umode_t i_mode;

	ret = -EINVAL;
	if (flags & ~VALID_FLAGS)
		goto out;

	endbyte = offset + nbytes;

	if ((s64)offset < 0)
		goto out;
	if ((s64)endbyte < 0)
		goto out;
	if (endbyte < offset)
		goto out;

	if (sizeof(pgoff_t) == 4) {
		if (offset >= (0x100000000ULL << PAGE_CACHE_SHIFT)) {
			/*
			 * The range starts outside a 32 bit machine's
			 * pagecache addressing capabilities.  Let it "succeed"
			 */
			ret = 0;
			goto out;
		}
		if (endbyte >= (0x100000000ULL << PAGE_CACHE_SHIFT)) {
			/*
			 * Out to EOF
			 */
			nbytes = 0;
		}
	}

	if (nbytes == 0)
		endbyte = LLONG_MAX;
	else
		endbyte--;		/* inclusive */

	ret = -EBADF;
	file = fget_light(fd, &fput_needed);
	if (!file)
		goto out;

	i_mode = file->f_path.dentry->d_inode->i_mode;
	ret = -ESPIPE;
	if (!S_ISREG(i_mode) && !S_ISBLK(i_mode) && !S_ISDIR(i_mode) &&
			!S_ISLNK(i_mode))
		goto out_put;

	mapping = file->f_mapping;
	if (!mapping) {
		ret = -EINVAL;
		goto out_put;
	}

	ret = 0;
	if (flags & SYNC_FILE_RANGE_WAIT_BEFORE) {
		ret = filemap_fdatawait_range(mapping, offset, endbyte);
		if (ret < 0)
			goto out_put;
	}

	if (flags & SYNC_FILE_RANGE_WRITE) {
		ret = filemap_fdatawrite_range(mapping, offset, endbyte);
		if (ret < 0)
			goto out_put;
	}

	if (flags & SYNC_FILE_RANGE_WAIT_AFTER)
		ret = filemap_fdatawait_range(mapping, offset, endbyte);

out_put:
	fput_light(file, fput_needed);
out:
	return ret;
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_sync_file_range(long fd, loff_t offset, loff_t nbytes,
				    long flags)
{
	return SYSC_sync_file_range((int) fd, offset, nbytes,
				    (unsigned int) flags);
}
SYSCALL_ALIAS(sys_sync_file_range, SyS_sync_file_range);
#endif

/* It would be nice if people remember that not all the world's an i386
   when they introduce new system calls */
SYSCALL_DEFINE(sync_file_range2)(int fd, unsigned int flags,
				 loff_t offset, loff_t nbytes)
{
	return sys_sync_file_range(fd, offset, nbytes, flags);
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_sync_file_range2(long fd, long flags,
				     loff_t offset, loff_t nbytes)
{
	return SYSC_sync_file_range2((int) fd, (unsigned int) flags,
				     offset, nbytes);
}
SYSCALL_ALIAS(sys_sync_file_range2, SyS_sync_file_range2);
#endif
