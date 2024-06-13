#ifndef _LINUX_FS_H
#define _LINUX_FS_H

/*
 * This file has definitions for some important file table
 * structures etc.
 */

#include <linux/limits.h>
#include <linux/ioctl.h>

/*
 * It's silly to have NR_OPEN bigger than NR_FILE, but you can change
 * the file limit at runtime and only root can increase the per-process
 * nr_file rlimit, so it's safe to set up a ridiculously high absolute
 * upper limit on files-per-process.
 *
 * Some programs (notably those using select()) may have to be 
 * recompiled to take full advantage of the new limits..  
 */

/* Fixed constants first: */
#undef NR_OPEN
#define INR_OPEN 1024		/* Initial setting for nfile rlimits */

#define BLOCK_SIZE_BITS 10
#define BLOCK_SIZE (1<<BLOCK_SIZE_BITS)

#define SEEK_SET	0	/* seek relative to beginning of file */
#define SEEK_CUR	1	/* seek relative to current file position */
#define SEEK_END	2	/* seek relative to end of file */
#define SEEK_MAX	SEEK_END

/* And dynamically-tunable limits and defaults: */
struct files_stat_struct {
	int nr_files;		/* read only */
	int nr_free_files;	/* read only */
	int max_files;		/* tunable */
};

struct inodes_stat_t {
	int nr_inodes;
	int nr_unused;
	int dummy[5];		/* padding for sysctl ABI compatibility */
};


#define NR_FILE  8192	/* this can well be larger on a larger system */

#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4
#define MAY_APPEND 8
#define MAY_ACCESS 16
#define MAY_OPEN 32

/*
 * flags in file.f_mode.  Note that FMODE_READ and FMODE_WRITE must correspond
 * to O_WRONLY and O_RDWR via the strange trick in __dentry_open()
 */

/* file is open for reading */
#define FMODE_READ		((__force fmode_t)0x1)
/* file is open for writing */
#define FMODE_WRITE		((__force fmode_t)0x2)
/* file is seekable */
#define FMODE_LSEEK		((__force fmode_t)0x4)
/* file can be accessed using pread */
#define FMODE_PREAD		((__force fmode_t)0x8)
/* file can be accessed using pwrite */
#define FMODE_PWRITE		((__force fmode_t)0x10)
/* File is opened for execution with sys_execve / sys_uselib */
#define FMODE_EXEC		((__force fmode_t)0x20)
/* File is opened with O_NDELAY (only set for block devices) */
#define FMODE_NDELAY		((__force fmode_t)0x40)
/* File is opened with O_EXCL (only set for block devices) */
#define FMODE_EXCL		((__force fmode_t)0x80)
/* File is opened using open(.., 3, ..) and is writeable only for ioctls
   (specialy hack for floppy.c) */
#define FMODE_WRITE_IOCTL	((__force fmode_t)0x100)

/*
 * Don't update ctime and mtime.
 *
 * Currently a special hack for the XFS open_by_handle ioctl, but we'll
 * hopefully graduate it to a proper O_CMTIME flag supported by open(2) soon.
 */
#define FMODE_NOCMTIME		((__force fmode_t)0x800)

/* Expect random access pattern */
#define FMODE_RANDOM		((__force fmode_t)0x1000)

/*
 * The below are the various read and write types that we support. Some of
 * them include behavioral modifiers that send information down to the
 * block layer and IO scheduler. Terminology:
 *
 *	The block layer uses device plugging to defer IO a little bit, in
 *	the hope that we will see more IO very shortly. This increases
 *	coalescing of adjacent IO and thus reduces the number of IOs we
 *	have to send to the device. It also allows for better queuing,
 *	if the IO isn't mergeable. If the caller is going to be waiting
 *	for the IO, then he must ensure that the device is unplugged so
 *	that the IO is dispatched to the driver.
 *
 *	All IO is handled async in Linux. This is fine for background
 *	writes, but for reads or writes that someone waits for completion
 *	on, we want to notify the block layer and IO scheduler so that they
 *	know about it. That allows them to make better scheduling
 *	decisions. So when the below references 'sync' and 'async', it
 *	is referencing this priority hint.
 *
 * With that in mind, the available types are:
 *
 * READ			A normal read operation. Device will be plugged.
 * READ_SYNC		A synchronous read. Device is not plugged, caller can
 *			immediately wait on this read without caring about
 *			unplugging.
 * READA		Used for read-ahead operations. Lower priority, and the
 *			 block layer could (in theory) choose to ignore this
 *			request if it runs into resource problems.
 * WRITE		A normal async write. Device will be plugged.
 * SWRITE		Like WRITE, but a special case for ll_rw_block() that
 *			tells it to lock the buffer first. Normally a buffer
 *			must be locked before doing IO.
 * WRITE_SYNC_PLUG	Synchronous write. Identical to WRITE, but passes down
 *			the hint that someone will be waiting on this IO
 *			shortly. The device must still be unplugged explicitly,
 *			WRITE_SYNC_PLUG does not do this as we could be
 *			submitting more writes before we actually wait on any
 *			of them.
 * WRITE_SYNC		Like WRITE_SYNC_PLUG, but also unplugs the device
 *			immediately after submission. The write equivalent
 *			of READ_SYNC.
 * WRITE_ODIRECT_PLUG	Special case write for O_DIRECT only.
 * SWRITE_SYNC
 * SWRITE_SYNC_PLUG	Like WRITE_SYNC/WRITE_SYNC_PLUG, but locks the buffer.
 *			See SWRITE.
 * WRITE_BARRIER	Like WRITE, but tells the block layer that all
 *			previously submitted writes must be safely on storage
 *			before this one is started. Also guarantees that when
 *			this write is complete, it itself is also safely on
 *			storage. Prevents reordering of writes on both sides
 *			of this IO.
 *
 */
#define RW_MASK		1
#define RWA_MASK	2
#define READ 0
#define WRITE 1
#define READA 2		/* read-ahead  - don't block if no resources */
#define SWRITE 3	/* for ll_rw_block() - wait for buffer lock */
#define READ_SYNC	(READ | (1 << BIO_RW_SYNCIO) | (1 << BIO_RW_UNPLUG))
#define READ_META	(READ | (1 << BIO_RW_META))
#define WRITE_SYNC_PLUG	(WRITE | (1 << BIO_RW_SYNCIO) | (1 << BIO_RW_NOIDLE))
#define WRITE_SYNC	(WRITE_SYNC_PLUG | (1 << BIO_RW_UNPLUG))
#define WRITE_ODIRECT_PLUG	(WRITE | (1 << BIO_RW_SYNCIO))
#define WRITE_META	(WRITE | (1 << BIO_RW_META))
#define SWRITE_SYNC_PLUG	\
			(SWRITE | (1 << BIO_RW_SYNCIO) | (1 << BIO_RW_NOIDLE))
#define SWRITE_SYNC	(SWRITE_SYNC_PLUG | (1 << BIO_RW_UNPLUG))
#define WRITE_BARRIER	(WRITE | (1 << BIO_RW_BARRIER))

/*
 * These aren't really reads or writes, they pass down information about
 * parts of device that are now unused by the file system.
 */
#define DISCARD_NOBARRIER (WRITE | (1 << BIO_RW_DISCARD))
#define DISCARD_BARRIER (DISCARD_NOBARRIER | (1 << BIO_RW_BARRIER))

#define SEL_IN		1
#define SEL_OUT		2
#define SEL_EX		4

/* public flags for file_system_type */
#define FS_REQUIRES_DEV 1 
#define FS_BINARY_MOUNTDATA 2
#define FS_HAS_SUBTYPE 4
#define FS_REVAL_DOT	16384	/* Check the paths ".", ".." for staleness */
#define FS_RENAME_DOES_D_MOVE	32768	/* FS will handle d_move()
					 * during rename() internally.
					 */

/*
 * These are the fs-independent mount-flags: up to 32 flags are supported
 */
#define MS_RDONLY	 1	/* Mount read-only */
#define MS_NOSUID	 2	/* Ignore suid and sgid bits */
#define MS_NODEV	 4	/* Disallow access to device special files */
#define MS_NOEXEC	 8	/* Disallow program execution */
#define MS_SYNCHRONOUS	16	/* Writes are synced at once */
#define MS_REMOUNT	32	/* Alter flags of a mounted FS */
#define MS_MANDLOCK	64	/* Allow mandatory locks on an FS */
#define MS_DIRSYNC	128	/* Directory modifications are synchronous */
#define MS_NOATIME	1024	/* Do not update access times. */
#define MS_NODIRATIME	2048	/* Do not update directory access times */
#define MS_BIND		4096
#define MS_MOVE		8192
#define MS_REC		16384
#define MS_VERBOSE	32768	/* War is peace. Verbosity is silence.
				   MS_VERBOSE is deprecated. */
#define MS_SILENT	32768
#define MS_POSIXACL	(1<<16)	/* VFS does not apply the umask */
#define MS_UNBINDABLE	(1<<17)	/* change to unbindable */
#define MS_PRIVATE	(1<<18)	/* change to private */
#define MS_SLAVE	(1<<19)	/* change to slave */
#define MS_SHARED	(1<<20)	/* change to shared */
#define MS_RELATIME	(1<<21)	/* Update atime relative to mtime/ctime. */
#define MS_KERNMOUNT	(1<<22) /* this is a kern_mount call */
#define MS_I_VERSION	(1<<23) /* Update inode I_version field */
#define MS_STRICTATIME	(1<<24) /* Always perform atime updates */
#define MS_ACTIVE	(1<<30)
#define MS_NOUSER	(1<<31)

/*
 * Superblock flags that can be altered by MS_REMOUNT
 */
#define MS_RMT_MASK	(MS_RDONLY|MS_SYNCHRONOUS|MS_MANDLOCK|MS_I_VERSION)

/*
 * Old magic mount flag and mask
 */
#define MS_MGC_VAL 0xC0ED0000
#define MS_MGC_MSK 0xffff0000

/* Inode flags - they have nothing to superblock flags now */

#define S_SYNC		1	/* Writes are synced at once */
#define S_NOATIME	2	/* Do not update access times */
#define S_APPEND	4	/* Append-only file */
#define S_IMMUTABLE	8	/* Immutable file */
#define S_DEAD		16	/* removed, but still open directory */
#define S_NOQUOTA	32	/* Inode is not counted to quota */
#define S_DIRSYNC	64	/* Directory modifications are synchronous */
#define S_NOCMTIME	128	/* Do not update file c/mtime */
#define S_SWAPFILE	256	/* Do not truncate: swapon got its bmaps */
#define S_PRIVATE	512	/* Inode is fs-internal */

/*
 * Note that nosuid etc flags are inode-specific: setting some file-system
 * flags just means all the inodes inherit those flags by default. It might be
 * possible to override it selectively if you really wanted to with some
 * ioctl() that is not currently implemented.
 *
 * Exception: MS_RDONLY is always applied to the entire file system.
 *
 * Unfortunately, it is possible to change a filesystems flags with it mounted
 * with files in use.  This means that all of the inodes will not have their
 * i_flags updated.  Hence, i_flags no longer inherit the superblock mount
 * flags, so these have to be checked separately. -- rmk@arm.uk.linux.org
 */
#define __IS_FLG(inode,flg) ((inode)->i_sb->s_flags & (flg))

#define IS_RDONLY(inode) ((inode)->i_sb->s_flags & MS_RDONLY)
#define IS_SYNC(inode)		(__IS_FLG(inode, MS_SYNCHRONOUS) || \
					((inode)->i_flags & S_SYNC))
#define IS_DIRSYNC(inode)	(__IS_FLG(inode, MS_SYNCHRONOUS|MS_DIRSYNC) || \
					((inode)->i_flags & (S_SYNC|S_DIRSYNC)))
#define IS_MANDLOCK(inode)	__IS_FLG(inode, MS_MANDLOCK)
#define IS_NOATIME(inode)   __IS_FLG(inode, MS_RDONLY|MS_NOATIME)
#define IS_I_VERSION(inode)   __IS_FLG(inode, MS_I_VERSION)

#define IS_NOQUOTA(inode)	((inode)->i_flags & S_NOQUOTA)
#define IS_APPEND(inode)	((inode)->i_flags & S_APPEND)
#define IS_IMMUTABLE(inode)	((inode)->i_flags & S_IMMUTABLE)
#define IS_POSIXACL(inode)	__IS_FLG(inode, MS_POSIXACL)

#define IS_DEADDIR(inode)	((inode)->i_flags & S_DEAD)
#define IS_NOCMTIME(inode)	((inode)->i_flags & S_NOCMTIME)
#define IS_SWAPFILE(inode)	((inode)->i_flags & S_SWAPFILE)
#define IS_PRIVATE(inode)	((inode)->i_flags & S_PRIVATE)

/* the read-only stuff doesn't really belong here, but any other place is
   probably as bad and I don't want to create yet another include file. */

#define BLKROSET   _IO(0x12,93)	/* set device read-only (0 = read-write) */
#define BLKROGET   _IO(0x12,94)	/* get read-only status (0 = read_write) */
#define BLKRRPART  _IO(0x12,95)	/* re-read partition table */
#define BLKGETSIZE _IO(0x12,96)	/* return device size /512 (long *arg) */
#define BLKFLSBUF  _IO(0x12,97)	/* flush buffer cache */
#define BLKRASET   _IO(0x12,98)	/* set read ahead for block device */
#define BLKRAGET   _IO(0x12,99)	/* get current read ahead setting */
#define BLKFRASET  _IO(0x12,100)/* set filesystem (mm/filemap.c) read-ahead */
#define BLKFRAGET  _IO(0x12,101)/* get filesystem (mm/filemap.c) read-ahead */
#define BLKSECTSET _IO(0x12,102)/* set max sectors per request (ll_rw_blk.c) */
#define BLKSECTGET _IO(0x12,103)/* get max sectors per request (ll_rw_blk.c) */
#define BLKSSZGET  _IO(0x12,104)/* get block device sector size */
#if 0
#define BLKPG      _IO(0x12,105)/* See blkpg.h */

/* Some people are morons.  Do not use sizeof! */

#define BLKELVGET  _IOR(0x12,106,size_t)/* elevator get */
#define BLKELVSET  _IOW(0x12,107,size_t)/* elevator set */
/* This was here just to show that the number is taken -
   probably all these _IO(0x12,*) ioctls should be moved to blkpg.h. */
#endif
/* A jump here: 108-111 have been used for various private purposes. */
#define BLKBSZGET  _IOR(0x12,112,size_t)
#define BLKBSZSET  _IOW(0x12,113,size_t)
#define BLKGETSIZE64 _IOR(0x12,114,size_t)	/* return device size in bytes (u64 *arg) */
#define BLKTRACESETUP _IOWR(0x12,115,struct blk_user_trace_setup)
#define BLKTRACESTART _IO(0x12,116)
#define BLKTRACESTOP _IO(0x12,117)
#define BLKTRACETEARDOWN _IO(0x12,118)
#define BLKDISCARD _IO(0x12,119)
#define BLKIOMIN _IO(0x12,120)
#define BLKIOOPT _IO(0x12,121)
#define BLKALIGNOFF _IO(0x12,122)
#define BLKPBSZGET _IO(0x12,123)
#define BLKDISCARDZEROES _IO(0x12,124)

#define BMAP_IOCTL 1		/* obsolete - kept for compatibility */
#define FIBMAP	   _IO(0x00,1)	/* bmap access */
#define FIGETBSZ   _IO(0x00,2)	/* get the block size used for bmap */
#define FIFREEZE	_IOWR('X', 119, int)	/* Freeze */
#define FITHAW		_IOWR('X', 120, int)	/* Thaw */

#define	FS_IOC_GETFLAGS			_IOR('f', 1, long)
#define	FS_IOC_SETFLAGS			_IOW('f', 2, long)
#define	FS_IOC_GETVERSION		_IOR('v', 1, long)
#define	FS_IOC_SETVERSION		_IOW('v', 2, long)
#define FS_IOC_FIEMAP			_IOWR('f', 11, struct fiemap)
#define FS_IOC32_GETFLAGS		_IOR('f', 1, int)
#define FS_IOC32_SETFLAGS		_IOW('f', 2, int)
#define FS_IOC32_GETVERSION		_IOR('v', 1, int)
#define FS_IOC32_SETVERSION		_IOW('v', 2, int)

/*
 * Inode flags (FS_IOC_GETFLAGS / FS_IOC_SETFLAGS)
 */
#define	FS_SECRM_FL			0x00000001 /* Secure deletion */
#define	FS_UNRM_FL			0x00000002 /* Undelete */
#define	FS_COMPR_FL			0x00000004 /* Compress file */
#define FS_SYNC_FL			0x00000008 /* Synchronous updates */
#define FS_IMMUTABLE_FL			0x00000010 /* Immutable file */
#define FS_APPEND_FL			0x00000020 /* writes to file may only append */
#define FS_NODUMP_FL			0x00000040 /* do not dump file */
#define FS_NOATIME_FL			0x00000080 /* do not update atime */
/* Reserved for compression usage... */
#define FS_DIRTY_FL			0x00000100
#define FS_COMPRBLK_FL			0x00000200 /* One or more compressed clusters */
#define FS_NOCOMP_FL			0x00000400 /* Don't compress */
#define FS_ECOMPR_FL			0x00000800 /* Compression error */
/* End compression flags --- maybe not all used */
#define FS_BTREE_FL			0x00001000 /* btree format dir */
#define FS_INDEX_FL			0x00001000 /* hash-indexed directory */
#define FS_IMAGIC_FL			0x00002000 /* AFS directory */
#define FS_JOURNAL_DATA_FL		0x00004000 /* Reserved for ext3 */
#define FS_NOTAIL_FL			0x00008000 /* file tail should not be merged */
#define FS_DIRSYNC_FL			0x00010000 /* dirsync behaviour (directories only) */
#define FS_TOPDIR_FL			0x00020000 /* Top of directory hierarchies*/
#define FS_EXTENT_FL			0x00080000 /* Extents */
#define FS_DIRECTIO_FL			0x00100000 /* Use direct i/o */
#define FS_RESERVED_FL			0x80000000 /* reserved for ext2 lib */

#define FS_FL_USER_VISIBLE		0x0003DFFF /* User visible flags */
#define FS_FL_USER_MODIFIABLE		0x000380FF /* User modifiable flags */


#define SYNC_FILE_RANGE_WAIT_BEFORE	1
#define SYNC_FILE_RANGE_WRITE		2
#define SYNC_FILE_RANGE_WAIT_AFTER	4

#ifdef __KERNEL__

#include <linux/linkage.h>
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/stat.h>
#include <linux/cache.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/radix-tree.h>
#include <linux/prio_tree.h>
#include <linux/init.h>
#include <linux/pid.h>
#include <linux/mutex.h>
#include <linux/capability.h>
#include <linux/semaphore.h>
#include <linux/fiemap.h>

#include <asm/atomic.h>
#include <asm/byteorder.h>

struct export_operations;
struct hd_geometry;
struct iovec;
struct nameidata;
struct kiocb;
struct pipe_inode_info;
struct poll_table_struct;
struct kstatfs;
struct vm_area_struct;
struct vfsmount;
struct cred;

extern void __init inode_init(void);
extern void __init inode_init_early(void);
extern void __init files_init(unsigned long);

extern struct files_stat_struct files_stat;
extern int get_max_files(void);
extern int sysctl_nr_open;
extern struct inodes_stat_t inodes_stat;
extern int leases_enable, lease_break_time;
#ifdef CONFIG_DNOTIFY
extern int dir_notify_enable;
#endif

struct buffer_head;
typedef int (get_block_t)(struct inode *inode, sector_t iblock,
			struct buffer_head *bh_result, int create);
typedef void (dio_iodone_t)(struct kiocb *iocb, loff_t offset,
			ssize_t bytes, void *private);

/*
 * Attribute flags.  These should be or-ed together to figure out what
 * has been changed!
 */
#define ATTR_MODE	(1 << 0)
#define ATTR_UID	(1 << 1)
#define ATTR_GID	(1 << 2)
#define ATTR_SIZE	(1 << 3)
#define ATTR_ATIME	(1 << 4)
#define ATTR_MTIME	(1 << 5)
#define ATTR_CTIME	(1 << 6)
#define ATTR_ATIME_SET	(1 << 7)
#define ATTR_MTIME_SET	(1 << 8)
#define ATTR_FORCE	(1 << 9) /* Not a change, but a change it */
#define ATTR_ATTR_FLAG	(1 << 10)
#define ATTR_KILL_SUID	(1 << 11)
#define ATTR_KILL_SGID	(1 << 12)
#define ATTR_FILE	(1 << 13)
#define ATTR_KILL_PRIV	(1 << 14)
#define ATTR_OPEN	(1 << 15) /* Truncating from open(O_TRUNC) */
#define ATTR_TIMES_SET	(1 << 16)

/*
 * This is the Inode Attributes structure, used for notify_change().  It
 * uses the above definitions as flags, to know which values have changed.
 * Also, in this manner, a Filesystem can look at only the values it cares
 * about.  Basically, these are the attributes that the VFS layer can
 * request to change from the FS layer.
 *
 * Derek Atkins <warlord@MIT.EDU> 94-10-20
 */
struct iattr {
	unsigned int	ia_valid;
	umode_t		ia_mode;
	uid_t		ia_uid;
	gid_t		ia_gid;
	loff_t		ia_size;
	struct timespec	ia_atime;
	struct timespec	ia_mtime;
	struct timespec	ia_ctime;

	/*
	 * Not an attribute, but an auxilary info for filesystems wanting to
	 * implement an ftruncate() like method.  NOTE: filesystem should
	 * check for (ia_valid & ATTR_FILE), and not for (ia_file != NULL).
	 */
	struct file	*ia_file;
};

/*
 * Includes for diskquotas.
 */
#include <linux/quota.h>

/** 
 * enum positive_aop_returns - aop return codes with specific semantics
 *
 * @AOP_WRITEPAGE_ACTIVATE: Informs the caller that page writeback has
 * 			    completed, that the page is still locked, and
 * 			    should be considered active.  The VM uses this hint
 * 			    to return the page to the active list -- it won't
 * 			    be a candidate for writeback again in the near
 * 			    future.  Other callers must be careful to unlock
 * 			    the page if they get this return.  Returned by
 * 			    writepage(); 
 *
 * @AOP_TRUNCATED_PAGE: The AOP method that was handed a locked page has
 *  			unlocked it and the page might have been truncated.
 *  			The caller should back up to acquiring a new page and
 *  			trying again.  The aop will be taking reasonable
 *  			precautions not to livelock.  If the caller held a page
 *  			reference, it should drop it before retrying.  Returned
 *  			by readpage().
 *
 * address_space_operation functions return these large constants to indicate
 * special semantics to the caller.  These are much larger than the bytes in a
 * page to allow for functions that return the number of bytes operated on in a
 * given page.
 */
/**
 * enum positive_aop_returns - 有特定语义的aop返回码
 *
 * @AOP_WRITEPAGE_ACTIVATE: 告知调用者页面写回已完成，页面仍然锁定，并且应被视为活跃的。
 *                          VM使用这一提示将页面返回到活跃列表——它在不久的将来不会再成为写回的候选。
 *                          其他调用者在收到此返回时必须小心解锁页面。由writepage()返回；
 *
 * @AOP_TRUNCATED_PAGE: 被传递了一个锁定页面的AOP方法已经解锁了该页面，并且页面可能已被截断。
 *                      调用者应该返回到获取新页面并重试。aop将采取合理的预防措施以避免活锁。
 *                      如果调用者持有页面引用，在重试前应放弃它。由readpage()返回。
 *
 * address_space_operation函数返回这些大的常数来向调用者表明特殊的语义。这些常数远大于页面中的字节数，
 * 以允许函数返回在给定页面中操作的字节数。
 */

enum positive_aop_returns {
	AOP_WRITEPAGE_ACTIVATE	= 0x80000,	 	// 写页面激活
	AOP_TRUNCATED_PAGE	= 0x80001,				// 页面被截断
};

/* 将不执行短写 */
#define AOP_FLAG_UNINTERRUPTIBLE	0x0001 /* will not do a short write */
/* 从cont_expand调用 */
#define AOP_FLAG_CONT_EXPAND		0x0002 /* called from cont_expand */
/* 用于文件系统指示
 * 辅助代码（例如缓冲层）
 * 从分配中清除GFP_FS
 */
#define AOP_FLAG_NOFS			0x0004 /* used by filesystem to direct
						* helper code (eg buffer layer)
						* to clear GFP_FS from alloc */

/*
 * oh the beauties of C type declarations.
 */
struct page;
struct address_space;
struct writeback_control;

struct iov_iter {
	// 指向iovec结构数组的指针
	const struct iovec *iov;
	// iovec结构的数量
	unsigned long nr_segs;
	// 当前 iovec 已经使用的偏移量
	size_t iov_offset;
	// 还剩多少字节需要处理
	size_t count;
};

// 从用户空间原子地复制数据到指定页
size_t iov_iter_copy_from_user_atomic(struct page *page,
		struct iov_iter *i, unsigned long offset, size_t bytes);
// 从用户空间复制数据到指定页
size_t iov_iter_copy_from_user(struct page *page,
		struct iov_iter *i, unsigned long offset, size_t bytes);
// 推进iov_iter指定的字节数
void iov_iter_advance(struct iov_iter *i, size_t bytes);
// 确保iov_iter指定的区域在内存中可读，可能导致缺页错误
int iov_iter_fault_in_readable(struct iov_iter *i, size_t bytes);
// 返回当前iov_iter的单个段的剩余字节数
size_t iov_iter_single_seg_count(struct iov_iter *i);

static inline void iov_iter_init(struct iov_iter *i,
			const struct iovec *iov, unsigned long nr_segs,
			size_t count, size_t written)
{
	// 初始化iovec指针
	i->iov = iov;
	// 初始化段数
	i->nr_segs = nr_segs;
	// 从第一个段的开始处开始处理
	i->iov_offset = 0;
	// 设置总共需要处理的字节数
	i->count = count + written;

	// 根据已经写入的字节数推进
	iov_iter_advance(i, written);
}

static inline size_t iov_iter_count(struct iov_iter *i)
{
	// 返回iov_iter中剩余的字节数
	return i->count;
}

/*
 * "descriptor" for what we're up to with a read.
 * This allows us to use the same read code yet
 * have multiple different users of the data that
 * we read from a file.
 *
 * The simplest case just copies the data to user
 * mode.
 */
/*
 * “描述符”，用于跟踪我们在读取操作中的状态。
 * 这允许我们使用相同的读取代码，但可以有多种不同的数据使用方式，
 * 这些数据是从文件中读取的。
 *
 * 最简单的情况就是将数据复制到用户空间。
 */
typedef struct {
	size_t written;	// 已写入的数据量
	size_t count;		// 总计需要读取的数据量
	union {
		char __user *buf;	// 用户空间的缓冲区指针
		void *data;				// 用于其他目的的泛型数据指针
	} arg;	// 参数，可以是用户空间的缓冲区或其他数据
	// 错误码，如果读取过程中发生错误则设置
	int error;
	// 结构体名称为 read_descriptor_t
} read_descriptor_t;

typedef int (*read_actor_t)(read_descriptor_t *, struct page *,
		unsigned long, unsigned long);

/* 地址空间操作结构体，定义了文件系统或块设备处理页面操作时的具体行为 */
// 为缓存对象实现的页I/O操作
struct address_space_operations {
	// 写入页面到磁盘
	int (*writepage)(struct page *page, struct writeback_control *wbc);
	// 从磁盘读取页面到内存
	int (*readpage)(struct file *, struct page *);
	// 同步页面（确保页面完全写入磁盘）
	void (*sync_page)(struct page *);

	/* Write back some dirty pages from this mapping. */
	/* 从这个映射写回一些脏页面 */
	int (*writepages)(struct address_space *, struct writeback_control *);

	/* Set a page dirty.  Return true if this dirtied it */
	/* 设置页面为脏。如果此操作让页面变脏则返回true */
	int (*set_page_dirty)(struct page *page);

	// 批量读取多个页面
	int (*readpages)(struct file *filp, struct address_space *mapping,
			struct list_head *pages, unsigned nr_pages);

	// 开始写操作前的处理
	int (*write_begin)(struct file *, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned flags,
				struct page **pagep, void **fsdata);
	// 完成写操作之后的处理
	int (*write_end)(struct file *, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned copied,
				struct page *page, void *fsdata);

	/* Unfortunately this kludge is needed for FIBMAP. Don't use it */
	/* 不幸地，这个替代品是FIBMAP所需要的。请勿使用它 */
	sector_t (*bmap)(struct address_space *, sector_t);
	// 使页面无效
	void (*invalidatepage) (struct page *, unsigned long);
	// 释放一个页面
	int (*releasepage) (struct page *, gfp_t);
	// 执行直接I/O操作
	ssize_t (*direct_IO)(int, struct kiocb *, const struct iovec *iov,
			loff_t offset, unsigned long nr_segs);
	// 获取执行内存的地址
	int (*get_xip_mem)(struct address_space *, pgoff_t, int,
						void **, unsigned long *);
	/* migrate the contents of a page to the specified target */
	/* 迁移一个页面的内容到指定的目标 */
	int (*migratepage) (struct address_space *,
			struct page *, struct page *);
	// 清洗页面，通常用于清除页面的缓存状态
	int (*launder_page) (struct page *);
	// 检查页面的部分区域是否是最新的
	int (*is_partially_uptodate) (struct page *, read_descriptor_t *,
					unsigned long);
	// 错误移除页面处理
	int (*error_remove_page)(struct address_space *, struct page *);
};

/*
 * pagecache_write_begin/pagecache_write_end must be used by general code
 * to write into the pagecache.
 */
/*
 * pagecache_write_begin/pagecache_write_end 必须由常规代码使用，
 * 用于向页高速缓存写入数据。
 */
// 准备写入页高速缓存前的初始化工作
// file: 操作的文件
// mapping: 文件关联的地址空间，管理文件或块设备的内存映射
// pos: 写入的起始位置
// len: 写入的长度
// flags: 操作标志
// pagep: 输出参数，返回找到或创建的页面的指针
// fsdata: 文件系统可能使用的私有数据指针
int pagecache_write_begin(struct file *, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned flags,
				struct page **pagep, void **fsdata);

// 完成写入页高速缓存后的清理工作
// file: 操作的文件
// mapping: 文件关联的地址空间
// pos: 写入的起始位置
// len: 请求写入的长度
// copied: 实际写入的长度
// page: 涉及的页
// fsdata: 文件系统可能使用的私有数据指针
int pagecache_write_end(struct file *, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned copied,
				struct page *page, void *fsdata);

struct backing_dev_info;
/* 
 * 地址空间结构体，用于管理文件或块设备的内存映射（页高速缓存）
 * 使用该对象管理缓存项和页I/O操作
 */
struct address_space {
	/* 所属的 inode 或块设备 */
	// 通常address_space会和索引节点(inode)关联，此时host指向此inode
	// 如果address_space不是和索引节点关联，比如和swapper关联，则host域是NULL。
	struct inode		*host;		/* owner: inode, block_device */
	/* 所有页面的基数树 */
	struct radix_tree_root	page_tree;	/* radix tree of all pages */
	/* 保护基数树的自旋锁 */
	spinlock_t		tree_lock;	/* and lock protecting it */
	/* 可写的 VM_SHARED 映射的计数 */
	unsigned int		i_mmap_writable;/* count VM_SHARED mappings */
	/* 私有和共享映射的优先级树 */
	// imap是一个优先搜索树，它的搜索范围包含了在address_space范围内所有共享的和私有的映射页面。
	// 优先搜索树是将堆和radix结合形成的快速检索树。可以帮助内核高效找到关联的被缓存文件
	struct prio_tree_root	i_mmap;		/* tree of private and shared mappings */
	/* VM_NONLINEAR 映射列表 */
	struct list_head	i_mmap_nonlinear;/*list VM_NONLINEAR mappings */
	/* 保护映射树、计数和列表的自旋锁 */
	spinlock_t		i_mmap_lock;	/* protect tree, count, list */
	/* 用于处理与截断操作相关的竞态条件 */
	// 截断计数
	unsigned int		truncate_count;	/* Cover race condition with truncate */
	/* 总页数 */
	// address_space页面总数
	unsigned long		nrpages;	/* number of total pages */
	/* 从这里开始回写 */
	// 回写的起始偏移
	pgoff_t			writeback_index;/* writeback starts here */
	/* 方法操作集 */
	const struct address_space_operations *a_ops;	/* methods */
	/* 错误位/内存分配标志 */
	// gfp_mask掩码（内存分配时使用）与错误标识
	unsigned long		flags;		/* error bits/gfp mask */
	/* 后备设备信息，如预读取等 */
	// 预读信息
	struct backing_dev_info *backing_dev_info; /* device readahead, etc */
	// 私有address_space锁
	spinlock_t		private_lock;	/* for use by the address_space */
	// 私有address_space链表
	struct list_head	private_list;	/* ditto */
	// 相关的缓冲
	struct address_space	*assoc_mapping;	/* ditto */
} __attribute__((aligned(sizeof(long))));	/* 确保结构体按长整型大小对齐 */
// 确保结构体按长整型大小对齐，这在大多数架构上已经是默认行为，但在一些架构如CRIS上需要显式声明。
	/*
	 * On most architectures that alignment is already the case; but
	 * must be enforced here for CRIS, to let the least signficant bit
	 * of struct page's "mapping" pointer be used for PAGE_MAPPING_ANON.
	 */

// 与底层的块设备操作相关
struct block_device {
	// 设备号，用作搜索键。
	dev_t			bd_dev;  /* not a kdev_t - it's a search key */
	// 将要被移除的inode指针。
	struct inode *		bd_inode;	/* will die */
	// 关联的超级块。
	struct super_block *	bd_super;
	// 打开设备的次数计数。
	int			bd_openers;
	// 用于打开/关闭设备的互斥锁。
	struct mutex		bd_mutex;	/* open/close mutex */
	// 与设备关联的inode列表。
	struct list_head	bd_inodes;
	// 设备的持有者。
	void *			bd_holder;
	// 设备持有者的数量。
	int			bd_holders;
#ifdef CONFIG_SYSFS
	// 系统文件系统中的持有者列表。
	struct list_head	bd_holder_list;
#endif
	// 包含此设备的父设备。
	struct block_device *	bd_contains;
	// 设备的块大小。
	unsigned		bd_block_size;
	// 设备的分区结构。
	struct hd_struct *	bd_part;
	/* number of times partitions within this device have been opened. */
	// 设备内分区被打开的次数。
	unsigned		bd_part_count;
	// 设备是否被废弃标志。
	int			bd_invalidated;
	// 关联的通用磁盘结构。
	struct gendisk *	bd_disk;
	// 设备链表。
	struct list_head	bd_list;
	/*
	 * Private data.  You must have bd_claim'ed the block_device
	 * to use this.  NOTE:  bd_claim allows an owner to claim
	 * the same device multiple times, the owner must take special
	 * care to not mess up bd_private for that case.
	 */
	// 私有数据，仅在持有设备时可用。
	unsigned long		bd_private;

	/* The counter of freeze processes */
	// 冻结进程的计数器。
	int			bd_fsfreeze_count;
	/* Mutex for freeze */
	// 冻结操作的互斥锁。
	struct mutex		bd_fsfreeze_mutex;
};

/*
 * Radix-tree tags, for tagging dirty and writeback pages within the pagecache
 * radix trees
 */
#define PAGECACHE_TAG_DIRTY	0
#define PAGECACHE_TAG_WRITEBACK	1

int mapping_tagged(struct address_space *mapping, int tag);

/*
 * Might pages of this file be mapped into userspace?
 */
static inline int mapping_mapped(struct address_space *mapping)
{
	return	!prio_tree_empty(&mapping->i_mmap) ||
		!list_empty(&mapping->i_mmap_nonlinear);
}

/*
 * Might pages of this file have been modified in userspace?
 * Note that i_mmap_writable counts all VM_SHARED vmas: do_mmap_pgoff
 * marks vma as VM_SHARED if it is shared, and the file was opened for
 * writing i.e. vma may be mprotected writable even if now readonly.
 */
static inline int mapping_writably_mapped(struct address_space *mapping)
{
	return mapping->i_mmap_writable != 0;
}

/*
 * Use sequence counter to get consistent i_size on 32-bit processors.
 */
#if BITS_PER_LONG==32 && defined(CONFIG_SMP)
#include <linux/seqlock.h>
#define __NEED_I_SIZE_ORDERED
#define i_size_ordered_init(inode) seqcount_init(&inode->i_size_seqcount)
#else
#define i_size_ordered_init(inode) do { } while (0)
#endif

struct posix_acl;
#define ACL_NOT_CACHED ((void *)(-1))

// 索引节点对象，该对象包含了内核在操作文件或目录时需要的全部信息。
struct inode {
	// 放在inode_unused或inode_in_use中
	// 指向哈希链表指针，用于查询，已经inode号码和对应超级块的时候，通过哈希表来快速查询地址。
	// 为了加快查找效率，将正在使用的和脏的inode放入一个哈希表中，
	// 但是不同的inode的哈希值可能相等，hash值相等的inode通过过i_hash成员连接。
	struct hlist_node	i_hash;		/* 散列表 */
	struct list_head	i_list;		/* backing dev IO list */	/* 索引节点链表 */
	struct list_head	i_sb_list;	/* 超级块链表 */
	// 一个给定的inode可能由多个链接（如/a/b/c，就存在/和目录a和目录b），所以就可能有多个目录项对象，因此用一个链表连接它们
	// 指向目录项链表指针，因为一个inode可以对应多个dentry(a/b和b/b是同一个硬链接的情况)，
	// 因此用一个链表将于本inode关联的目录项都连在一起。
	struct list_head	i_dentry;		/* 目录项链表 */
	unsigned long		i_ino;				/* 节点号 */
	// 访问该inode结构体对象的进程数
	atomic_t		i_count;					/* 引用计数 */
	// 硬链接计数，等于0时将文件从磁盘移除
	unsigned int		i_nlink;			/* 硬链接数 */
	uid_t			i_uid;							/* 使用者id */
	gid_t			i_gid;							/* 使用组的id */
	dev_t			i_rdev;							/* 实际设备标识符 */
	unsigned int		i_blkbits;		/* 以位为单位的块大小 */
	u64			i_version;						/* 版本号 */
	loff_t			i_size;						/* 以字节为单位的文件大小 */
#ifdef __NEED_I_SIZE_ORDERED
	seqcount_t		i_size_seqcount;	/* 对i_size进行串行计数 */
#endif
	struct timespec		i_atime;			/* 最后访问时间 */
	struct timespec		i_mtime;			/* 最后修改时间 */
	struct timespec		i_ctime;			/* 最后改变时间 */
	blkcnt_t		i_blocks;						/* 文件的块数 */
	unsigned short          i_bytes;	/* 使用的字节数 */
	umode_t			i_mode;							/* 访问权限 */
	spinlock_t		i_lock;	/* i_blocks, i_bytes, maybe i_size */		/* 自旋锁 */
	struct mutex		i_mutex;					/* 索引节点自旋锁 */
	struct rw_semaphore	i_alloc_sem;	/* 嵌入i_sem内部 */
	// 描述了VFS用以操作索引节点对象的所有方法，这些方法由文件系统实现。调用方式如下i->i_op->truncate(i)
	// 索引节点操作函数指针，指向了inode_operation结构体，提供与inode相关的操作
	const struct inode_operations	*i_op;	/* 索引节点操作表 */
	// 指向file_operations结构提供文件操作，在file结构体中也有指向file_operations结构的指针。
	const struct file_operations	*i_fop;	/* former ->i_op->default_file_ops */	/*缺省的索引节点操作*/
	// inode所属文件系统的超级块指针
	struct super_block	*i_sb;				/* 相关的超级块 */
	struct file_lock	*i_flock;				/* 文件锁链表 */
	struct address_space	*i_mapping;	/* 相关的地址映射 */
	struct address_space	i_data;			/* 设备地址映射 */
#ifdef CONFIG_QUOTA
	struct dquot		*i_dquot[MAXQUOTAS];	/* 索引结点的磁盘限额 */
#endif
	struct list_head	i_devices;					/* 块设备链表 */
	union {
		// 下面三种结构互斥，所以放在一个union中。inode可以表示下面三者之一或三者都不是。
		struct pipe_inode_info	*i_pipe;		/* 管道信息 */			// i_pipe指向一个代表用名管道的数据结构
		struct block_device	*i_bdev;				/* 块设备驱动 */		// i_bdev指向块设备结构体
		struct cdev		*i_cdev;							/* 字符设备驱动 */	// i_cdev指向字符设备结构体
	};

	__u32			i_generation;		// 生成号，用于文件系统的唯一性验证。

#ifdef CONFIG_FSNOTIFY
	// 事件掩码，指定inode关心的事件。
	__u32			i_fsnotify_mask; /* all events this inode cares about */
	// fsnotify标记项链表。
	struct hlist_head	i_fsnotify_mark_entries; /* fsnotify mark entries */
#endif

#ifdef CONFIG_INOTIFY
	struct list_head	inotify_watches; /* watches on this inode */	/* 索引节点通知监测链表 */
	struct mutex		inotify_mutex;	/* protects the watches list */	/* 保护inotify_watches */
#endif

	unsigned long		i_state;		/* 状态标志 */
	unsigned long		dirtied_when;	/* jiffies of first dirtying */	/* 第一次弄脏数据的时间 */

	unsigned int		i_flags;		/* 文件系统标志 */

	atomic_t		i_writecount;		/* 写者计数 */
#ifdef CONFIG_SECURITY
	void			*i_security;			/* 安全模块 */
#endif
#ifdef CONFIG_FS_POSIX_ACL
	// POSIX访问控制列表。
	struct posix_acl	*i_acl;
	// 默认POSIX访问控制列表。
	struct posix_acl	*i_default_acl;
#endif
	void			*i_private; /* fs or device private pointer */	/* fs私有指针 */
};

/*
 * inode->i_mutex nesting subclasses for the lock validator:
 *
 * 0: the object of the current VFS operation
 * 1: parent
 * 2: child/target
 * 3: quota file
 *
 * The locking order between these classes is
 * parent -> child -> normal -> xattr -> quota
 */
enum inode_i_mutex_lock_class
{
	I_MUTEX_NORMAL,
	I_MUTEX_PARENT,
	I_MUTEX_CHILD,
	I_MUTEX_XATTR,
	I_MUTEX_QUOTA
};

/*
 * NOTE: in a 32bit arch with a preemptable kernel and
 * an UP compile the i_size_read/write must be atomic
 * with respect to the local cpu (unlike with preempt disabled),
 * but they don't need to be atomic with respect to other cpus like in
 * true SMP (so they need either to either locally disable irq around
 * the read or for example on x86 they can be still implemented as a
 * cmpxchg8b without the need of the lock prefix). For SMP compiles
 * and 64bit archs it makes no difference if preempt is enabled or not.
 */
static inline loff_t i_size_read(const struct inode *inode)
{
#if BITS_PER_LONG==32 && defined(CONFIG_SMP)
	loff_t i_size;
	unsigned int seq;

	do {
		seq = read_seqcount_begin(&inode->i_size_seqcount);
		i_size = inode->i_size;
	} while (read_seqcount_retry(&inode->i_size_seqcount, seq));
	return i_size;
#elif BITS_PER_LONG==32 && defined(CONFIG_PREEMPT)
	loff_t i_size;

	preempt_disable();
	i_size = inode->i_size;
	preempt_enable();
	return i_size;
#else
	return inode->i_size;
#endif
}

/*
 * NOTE: unlike i_size_read(), i_size_write() does need locking around it
 * (normally i_mutex), otherwise on 32bit/SMP an update of i_size_seqcount
 * can be lost, resulting in subsequent i_size_read() calls spinning forever.
 */
static inline void i_size_write(struct inode *inode, loff_t i_size)
{
#if BITS_PER_LONG==32 && defined(CONFIG_SMP)
	write_seqcount_begin(&inode->i_size_seqcount);
	inode->i_size = i_size;
	write_seqcount_end(&inode->i_size_seqcount);
#elif BITS_PER_LONG==32 && defined(CONFIG_PREEMPT)
	preempt_disable();
	inode->i_size = i_size;
	preempt_enable();
#else
	inode->i_size = i_size;
#endif
}

static inline unsigned iminor(const struct inode *inode)
{
	return MINOR(inode->i_rdev);
}

static inline unsigned imajor(const struct inode *inode)
{
	return MAJOR(inode->i_rdev);
}

extern struct block_device *I_BDEV(struct inode *inode);

struct fown_struct {
	/* 保护 pid, uid, euid 字段的读写锁 */
	// 保护 `pid`, `uid`, `euid` 字段的读写锁，确保它们的并发访问安全
	rwlock_t lock;          /* protects pid, uid, euid fields */
	/* 应该发送 SIGIO 的 pid 或 -pgrp */
	// 表示应该发送 SIGIO 信号的进程 ID 或进程组 ID
	struct pid *pid;	/* pid or -pgrp where SIGIO should be sent */
	/* 发送 SIGIO 的进程组类型 */
	// 表示发送 SIGIO 信号的进程组类型
	enum pid_type pid_type;	/* Kind of process group SIGIO should be sent to */
	 /* 设置所有者的进程的 uid/euid */
	 // 表示设置所有者的进程的用户 ID 和有效用户 ID
	uid_t uid, euid;	/* uid/euid of process setting the owner */
	/* 要在 IO 上发送的 posix.1b 实时信号 */
	// 表示在 IO 上发送的 posix.1b 实时信号编号
	int signum;		/* posix.1b rt signal to be delivered on IO */
};

/*
 * Track a single file's readahead state
 */
/*
 * 跟踪单个文件的预读状态
 */
struct file_ra_state {
	/* 预读开始的位置 */
	pgoff_t start;			/* where readahead started */
	/* 预读页数 */
	unsigned int size;		/* # of readahead pages */
	/* 当只剩下 # 个页时进行异步预读 */
	// 表示当预读窗口中只剩下指定页数时进行异步预读
	unsigned int async_size;	/* do asynchronous readahead when
					   there are only # of pages ahead */

	/* 最大预读窗口 */
	// 表示最大预读窗口大小
	unsigned int ra_pages;		/* Maximum readahead window */
	/* mmap 访问的缓存未命中统计 */
	unsigned int mmap_miss;		/* Cache miss stat for mmap accesses */
	/* 缓存上次 read() 位置 */
	loff_t prev_pos;		/* Cache last read() position */
};

/*
 * Check if @index falls in the readahead windows.
 */
/*
 * 检查 @index 是否在预读窗口内
 */
static inline int ra_has_index(struct file_ra_state *ra, pgoff_t index)
{
	// 检查 `index` 是否在预读窗口内
	return (index >= ra->start &&
		index <  ra->start + ra->size);
}

// 表示文件挂载写入已获取
#define FILE_MNT_WRITE_TAKEN	1
// 表示文件挂载写入已释放
#define FILE_MNT_WRITE_RELEASED	2

// 文件对象，表示进程已打开的文件。文件对象是已打开的文件在内存中的表示。该对象由open系统调用创建，close系统调用撤销
// 文件对象实际上没有对应的磁盘数据，通过f_dentry指针指向相关的目录项对象，目录项会指向相关的索引节点，索引节点会记录文件是否为脏的。
// 和进程相关，描述了进程相关的文件
struct file {
	/*
	 * fu_list becomes invalid after file_free is called and queued via
	 * fu_rcuhead for RCU freeing
	 */
	// 系统级的打开文件表是由所有内核创建的FILE对象通过其fu_list成员连接成的双向循环链表。
	// 即系统级打开文件表中的表项就是FILE对象，FILE对象中有目录项成员dentry，
	// dentry中有指向inode索引节点的指针，即通过打开文件表中的表项，就能找到对应的inode。
	union {
		struct list_head	fu_list;		// 文件对象链表，用于将所有的打开的FILE文件连接起来形成打开文件表
		struct rcu_head 	fu_rcuhead;	// RCU(Read-Copy Update)是Linux 2.6内核中新的锁机制
	} f_u;
	struct path		f_path;						// 包含dentry和mnt两个成员，用于确定文件路径
#define f_dentry	f_path.dentry		// f_path的成员之一，当前文件的dentry结构
#define f_vfsmnt	f_path.mnt			// 表示当前文件所在文件系统的挂载根目录
	const struct file_operations	*f_op;	// 与该文件相关联的操作函数
	spinlock_t		f_lock;  /* f_ep_links, f_flags, no IRQ */	// 单个文件结构锁
	// 文件的引用计数(有多少进程打开该文件)
	atomic_long_t		f_count;		// 文件对象的使用计数
	// 对应于open时指定的flag
	unsigned int 		f_flags;		// 打开文件时所指定的标志
	// 读写模式：open的mod_t mode参数
	fmode_t			f_mode;					// 文件的访问模式
	// 当前文件指针位置
	loff_t			f_pos;					// 文件当前的位移量(文件指针)
	// 该结构的作用是通过信号进行I/O时间通知的数据。
	struct fown_struct	f_owner;		// 拥有者通过信号进行异步I/O数据传送
	const struct cred	*f_cred;			// 文件的信任状
	// 在linux/include/linux/fs.h中定义，文件预读相关
	struct file_ra_state	f_ra;			// 预读状态

	// 记录文件的版本号，每次使用之后递增
	u64			f_version;		// 版本号
#ifdef CONFIG_SECURITY
	void			*f_security;		// 安全模块
#endif
	/* needed for tty driver, and maybe others */
	/*tty 驱动程序需要，也许其他驱动程序需要*/
	void			*private_data;		// tty设备驱动的钩子

#ifdef CONFIG_EPOLL
	/* Used by fs/eventpoll.c to link all the hooks to this file */
	struct list_head	f_ep_links;		// 事件池链表
#endif /* #ifdef CONFIG_EPOLL */
	struct address_space	*f_mapping;		// 页缓存映射
#ifdef CONFIG_DEBUG_WRITECOUNT
	unsigned long f_mnt_write_state;		// 调试状态
#endif
};
extern spinlock_t files_lock;
#define file_list_lock() spin_lock(&files_lock);
#define file_list_unlock() spin_unlock(&files_lock);

#define get_file(x)	atomic_long_inc(&(x)->f_count)
#define file_count(x)	atomic_long_read(&(x)->f_count)

#ifdef CONFIG_DEBUG_WRITECOUNT
static inline void file_take_write(struct file *f)
{
	WARN_ON(f->f_mnt_write_state != 0);
	f->f_mnt_write_state = FILE_MNT_WRITE_TAKEN;
}
static inline void file_release_write(struct file *f)
{
	f->f_mnt_write_state |= FILE_MNT_WRITE_RELEASED;
}
static inline void file_reset_write(struct file *f)
{
	f->f_mnt_write_state = 0;
}
static inline void file_check_state(struct file *f)
{
	/*
	 * At this point, either both or neither of these bits
	 * should be set.
	 */
	WARN_ON(f->f_mnt_write_state == FILE_MNT_WRITE_TAKEN);
	WARN_ON(f->f_mnt_write_state == FILE_MNT_WRITE_RELEASED);
}
static inline int file_check_writeable(struct file *f)
{
	if (f->f_mnt_write_state == FILE_MNT_WRITE_TAKEN)
		return 0;
	printk(KERN_WARNING "writeable file with no "
			    "mnt_want_write()\n");
	WARN_ON(1);
	return -EINVAL;
}
#else /* !CONFIG_DEBUG_WRITECOUNT */
static inline void file_take_write(struct file *filp) {}
static inline void file_release_write(struct file *filp) {}
static inline void file_reset_write(struct file *filp) {}
static inline void file_check_state(struct file *filp) {}
static inline int file_check_writeable(struct file *filp)
{
	return 0;
}
#endif /* CONFIG_DEBUG_WRITECOUNT */

#define	MAX_NON_LFS	((1UL<<31) - 1)

/* Page cache limit. The filesystems should put that into their s_maxbytes 
   limits, otherwise bad things can happen in VM. */ 
#if BITS_PER_LONG==32
#define MAX_LFS_FILESIZE	(((u64)PAGE_CACHE_SIZE << (BITS_PER_LONG-1))-1) 
#elif BITS_PER_LONG==64
#define MAX_LFS_FILESIZE 	0x7fffffffffffffffUL
#endif

#define FL_POSIX	1
#define FL_FLOCK	2
#define FL_ACCESS	8	/* not trying to lock, just looking */
#define FL_EXISTS	16	/* when unlocking, test for existence */
#define FL_LEASE	32	/* lease held on this file */
#define FL_CLOSE	64	/* unlock on close */
#define FL_SLEEP	128	/* A blocking lock */

/*
 * Special return value from posix_lock_file() and vfs_lock_file() for
 * asynchronous locking.
 */
#define FILE_LOCK_DEFERRED 1

/*
 * The POSIX file lock owner is determined by
 * the "struct files_struct" in the thread group
 * (or NULL for no owner - BSD locks).
 *
 * Lockd stuffs a "host" pointer into this.
 */
typedef struct files_struct *fl_owner_t;

struct file_lock_operations {
	void (*fl_copy_lock)(struct file_lock *, struct file_lock *);
	void (*fl_release_private)(struct file_lock *);
};

struct lock_manager_operations {
	int (*fl_compare_owner)(struct file_lock *, struct file_lock *);
	void (*fl_notify)(struct file_lock *);	/* unblock callback */
	int (*fl_grant)(struct file_lock *, struct file_lock *, int);
	void (*fl_copy_lock)(struct file_lock *, struct file_lock *);
	void (*fl_release_private)(struct file_lock *);
	void (*fl_break)(struct file_lock *);
	int (*fl_mylease)(struct file_lock *, struct file_lock *);
	int (*fl_change)(struct file_lock **, int);
};

struct lock_manager {
	struct list_head list;
};

void locks_start_grace(struct lock_manager *);
void locks_end_grace(struct lock_manager *);
int locks_in_grace(void);

/* that will die - we need it for nfs_lock_info */
#include <linux/nfs_fs_i.h>

struct file_lock {
	struct file_lock *fl_next;	/* singly linked list for this inode  */
	struct list_head fl_link;	/* doubly linked list of all locks */
	struct list_head fl_block;	/* circular list of blocked processes */
	fl_owner_t fl_owner;
	unsigned char fl_flags;
	unsigned char fl_type;
	unsigned int fl_pid;
	struct pid *fl_nspid;
	wait_queue_head_t fl_wait;
	struct file *fl_file;
	loff_t fl_start;
	loff_t fl_end;

	struct fasync_struct *	fl_fasync; /* for lease break notifications */
	unsigned long fl_break_time;	/* for nonblocking lease breaks */

	const struct file_lock_operations *fl_ops;	/* Callbacks for filesystems */
	const struct lock_manager_operations *fl_lmops;	/* Callbacks for lockmanagers */
	union {
		struct nfs_lock_info	nfs_fl;
		struct nfs4_lock_info	nfs4_fl;
		struct {
			struct list_head link;	/* link in AFS vnode's pending_locks list */
			int state;		/* state of grant or error if -ve */
		} afs;
	} fl_u;
};

/* The following constant reflects the upper bound of the file/locking space */
#ifndef OFFSET_MAX
#define INT_LIMIT(x)	(~((x)1 << (sizeof(x)*8 - 1)))
#define OFFSET_MAX	INT_LIMIT(loff_t)
#define OFFT_OFFSET_MAX	INT_LIMIT(off_t)
#endif

#include <linux/fcntl.h>

extern void send_sigio(struct fown_struct *fown, int fd, int band);

#ifdef CONFIG_FILE_LOCKING
extern int fcntl_getlk(struct file *, struct flock __user *);
extern int fcntl_setlk(unsigned int, struct file *, unsigned int,
			struct flock __user *);

#if BITS_PER_LONG == 32
extern int fcntl_getlk64(struct file *, struct flock64 __user *);
extern int fcntl_setlk64(unsigned int, struct file *, unsigned int,
			struct flock64 __user *);
#endif

extern int fcntl_setlease(unsigned int fd, struct file *filp, long arg);
extern int fcntl_getlease(struct file *filp);

/* fs/locks.c */
extern void locks_init_lock(struct file_lock *);
extern void locks_copy_lock(struct file_lock *, struct file_lock *);
extern void __locks_copy_lock(struct file_lock *, const struct file_lock *);
extern void locks_remove_posix(struct file *, fl_owner_t);
extern void locks_remove_flock(struct file *);
extern void locks_release_private(struct file_lock *);
extern void posix_test_lock(struct file *, struct file_lock *);
extern int posix_lock_file(struct file *, struct file_lock *, struct file_lock *);
extern int posix_lock_file_wait(struct file *, struct file_lock *);
extern int posix_unblock_lock(struct file *, struct file_lock *);
extern int vfs_test_lock(struct file *, struct file_lock *);
extern int vfs_lock_file(struct file *, unsigned int, struct file_lock *, struct file_lock *);
extern int vfs_cancel_lock(struct file *filp, struct file_lock *fl);
extern int flock_lock_file_wait(struct file *filp, struct file_lock *fl);
extern int __break_lease(struct inode *inode, unsigned int flags);
extern void lease_get_mtime(struct inode *, struct timespec *time);
extern int generic_setlease(struct file *, long, struct file_lock **);
extern int vfs_setlease(struct file *, long, struct file_lock **);
extern int lease_modify(struct file_lock **, int);
extern int lock_may_read(struct inode *, loff_t start, unsigned long count);
extern int lock_may_write(struct inode *, loff_t start, unsigned long count);
#else /* !CONFIG_FILE_LOCKING */
static inline int fcntl_getlk(struct file *file, struct flock __user *user)
{
	return -EINVAL;
}

static inline int fcntl_setlk(unsigned int fd, struct file *file,
			      unsigned int cmd, struct flock __user *user)
{
	return -EACCES;
}

#if BITS_PER_LONG == 32
static inline int fcntl_getlk64(struct file *file, struct flock64 __user *user)
{
	return -EINVAL;
}

static inline int fcntl_setlk64(unsigned int fd, struct file *file,
				unsigned int cmd, struct flock64 __user *user)
{
	return -EACCES;
}
#endif
static inline int fcntl_setlease(unsigned int fd, struct file *filp, long arg)
{
	return 0;
}

static inline int fcntl_getlease(struct file *filp)
{
	return 0;
}

static inline void locks_init_lock(struct file_lock *fl)
{
	return;
}

static inline void __locks_copy_lock(struct file_lock *new, struct file_lock *fl)
{
	return;
}

static inline void locks_copy_lock(struct file_lock *new, struct file_lock *fl)
{
	return;
}

static inline void locks_remove_posix(struct file *filp, fl_owner_t owner)
{
	return;
}

static inline void locks_remove_flock(struct file *filp)
{
	return;
}

static inline void posix_test_lock(struct file *filp, struct file_lock *fl)
{
	return;
}

static inline int posix_lock_file(struct file *filp, struct file_lock *fl,
				  struct file_lock *conflock)
{
	return -ENOLCK;
}

static inline int posix_lock_file_wait(struct file *filp, struct file_lock *fl)
{
	return -ENOLCK;
}

static inline int posix_unblock_lock(struct file *filp,
				     struct file_lock *waiter)
{
	return -ENOENT;
}

static inline int vfs_test_lock(struct file *filp, struct file_lock *fl)
{
	return 0;
}

static inline int vfs_lock_file(struct file *filp, unsigned int cmd,
				struct file_lock *fl, struct file_lock *conf)
{
	return -ENOLCK;
}

static inline int vfs_cancel_lock(struct file *filp, struct file_lock *fl)
{
	return 0;
}

static inline int flock_lock_file_wait(struct file *filp,
				       struct file_lock *request)
{
	return -ENOLCK;
}

static inline int __break_lease(struct inode *inode, unsigned int mode)
{
	return 0;
}

static inline void lease_get_mtime(struct inode *inode, struct timespec *time)
{
	return;
}

static inline int generic_setlease(struct file *filp, long arg,
				    struct file_lock **flp)
{
	return -EINVAL;
}

static inline int vfs_setlease(struct file *filp, long arg,
			       struct file_lock **lease)
{
	return -EINVAL;
}

static inline int lease_modify(struct file_lock **before, int arg)
{
	return -EINVAL;
}

static inline int lock_may_read(struct inode *inode, loff_t start,
				unsigned long len)
{
	return 1;
}

static inline int lock_may_write(struct inode *inode, loff_t start,
				 unsigned long len)
{
	return 1;
}

#endif /* !CONFIG_FILE_LOCKING */


struct fasync_struct {
	int	magic;
	int	fa_fd;
	struct	fasync_struct	*fa_next; /* singly linked list */
	struct	file 		*fa_file;
};

#define FASYNC_MAGIC 0x4601

/* SMP safe fasync helpers: */
extern int fasync_helper(int, struct file *, int, struct fasync_struct **);
/* can be called from interrupts */
extern void kill_fasync(struct fasync_struct **, int, int);
/* only for net: no internal synchronization */
extern void __kill_fasync(struct fasync_struct *, int, int);

extern int __f_setown(struct file *filp, struct pid *, enum pid_type, int force);
extern int f_setown(struct file *filp, unsigned long arg, int force);
extern void f_delown(struct file *filp);
extern pid_t f_getown(struct file *filp);
extern int send_sigurg(struct fown_struct *fown);

/*
 *	Umount options
 */

#define MNT_FORCE	0x00000001	/* Attempt to forcibily umount */
#define MNT_DETACH	0x00000002	/* Just detach from the tree */
#define MNT_EXPIRE	0x00000004	/* Mark for expiry */
#define UMOUNT_NOFOLLOW	0x00000008	/* Don't follow symlink on umount */
#define UMOUNT_UNUSED	0x80000000	/* Flag guaranteed to be unused */

extern struct list_head super_blocks;		// 链表，用来将所有的super_block（超级块）连接起来
extern spinlock_t sb_lock;

#define sb_entry(list)  list_entry((list), struct super_block, s_list)
#define S_BIAS (1<<30)
// 超级块
struct super_block {
	struct list_head	s_list;		/* Keep this first */		/* 指向所有超级块的链表，该结构会插入到super_blocks链表 */
	dev_t			s_dev;		/* search index; _not_ kdev_t */	/* 设备标识符 */
	unsigned char		s_dirt;	/* 修改（脏）标志，用于判断超级块对象中数据是否脏了即被修改过了，即与磁盘上的超级块区域是否一致。 */
	unsigned char		s_blocksize_bits;			/* 以位为单位的块大小 */
	unsigned long		s_blocksize;		/* 以字节为单位的块大小 */
	loff_t			s_maxbytes;	/* Max file size */		/* 文件大小上限 */
	// 指向file_system_type类型的指针，file_system_type结构体用于保存具体的文件系统的信息。
	struct file_system_type	*s_type;		/* 文件系统类型 */
	// super_operations结构体类型的指针，因为一个超级块对应一种文件系统，
	// 而每种文件系统的操作函数可能是不同的。super_operations结构体由一些函数指针组成，
	// 这些函数指针用特定文件系统的超级块区域操作函数来初始化。
	// 比如里边会有函数实现获取和返回底层文件系统inode的方法。
	const struct super_operations	*s_op;	/* 超级块方法（对超级块操作的方法） */
	const struct dquot_operations	*dq_op;	/* 磁盘限额方法 */
	const struct quotactl_ops	*s_qcop;		/* 限额控制方法 */
	const struct export_operations *s_export_op;	/* 导出方法 */
	unsigned long		s_flags;		/* 挂载标志 */
	unsigned long		s_magic;		/* 文件系统的幻数 */
	struct dentry		*s_root;		/* 目录挂载点 */
	struct rw_semaphore	s_umount;	/* 卸载信号量 */
	struct mutex		s_lock;				/* 超级块锁 */
	int			s_count;							/* 超级块引用计数 */
	int			s_need_sync;					/* 尚未同步标志 */
	atomic_t		s_active;					/* 活动引用计数 */
#ifdef CONFIG_SECURITY
	void                    *s_security;		/* 安全模块 */
#endif
	struct xattr_handler	**s_xattr;				/* 拓展的属性操作 */

	// 指向超级块对应文件系统中的所有inode索引节点的链表。
	struct list_head	s_inodes;	/* all inodes */		/* inodes链表 */
	struct hlist_head	s_anon;		/* anonymous dentries for (nfs) exporting */
	// 该超级块表是的文件系统中所有被打开的文件。
	struct list_head	s_files;	/* 被分配文件链表 */
	/* s_dentry_lru and s_nr_dentry_unused are protected by dcache_lock */
	struct list_head	s_dentry_lru;	/* unused dentry lru */		/* 未被使用目录项链表 */
	int			s_nr_dentry_unused;	/* # of dentry on lru */			/* 链表中目录项的数目 */

	struct block_device	*s_bdev;		/* 相关的块设备 */
	struct backing_dev_info *s_bdi;
	struct mtd_info		*s_mtd;				/* 存储磁盘信息 */
	struct list_head	s_instances;	/* 该类型的文件系统 */
	struct quota_info	s_dquot;	/* Diskquota specific options */	/* 限额相关选项 */

	int			s_frozen;		/* frozen标志位 */
	wait_queue_head_t	s_wait_unfrozen;	/* 冻结的等待队列 */

	char s_id[32];				/* Informational name */	/* 文本名字 */

	// 指向指定文件系统的super_block比如，ext4_sb_info
	void 			*s_fs_info;	/* Filesystem private info */	/* 文件系统特殊信息 */
	fmode_t			s_mode;		/* 安装权限 */

	/* Granularity of c/m/atime in ns.
	   Cannot be worse than a second */
	u32		   s_time_gran;		/* 时间戳粒度 */

	/*
	 * The next field is for VFS *only*. No filesystems have any business
	 * even looking at it. You had been warned.
	 */
	struct mutex s_vfs_rename_mutex;	/* Kludge */	/* 重命名锁 */

	/*
	 * Filesystem subtype.  If non-empty the filesystem type field
	 * in /proc/mounts will be "type.subtype"
	 */
	char *s_subtype;		/* 子类型名称 */

	/*
	 * Saved mount options for lazy filesystems using
	 * generic_show_options()
	 */
	char *s_options;		/* 已存安装选项 */
};

extern struct timespec current_fs_time(struct super_block *sb);

/*
 * Snapshotting support.
 */
enum {
	SB_UNFROZEN = 0,
	SB_FREEZE_WRITE	= 1,
	SB_FREEZE_TRANS = 2,
};

#define vfs_check_frozen(sb, level) \
	wait_event((sb)->s_wait_unfrozen, ((sb)->s_frozen < (level)))

#define get_fs_excl() atomic_inc(&current->fs_excl)
#define put_fs_excl() atomic_dec(&current->fs_excl)
#define has_fs_excl() atomic_read(&current->fs_excl)

#define is_owner_or_cap(inode)	\
	((current_fsuid() == (inode)->i_uid) || capable(CAP_FOWNER))

/* not quite ready to be deprecated, but... */
extern void lock_super(struct super_block *);
extern void unlock_super(struct super_block *);

/*
 * VFS helper functions..
 */
extern int vfs_create(struct inode *, struct dentry *, int, struct nameidata *);
extern int vfs_mkdir(struct inode *, struct dentry *, int);
extern int vfs_mknod(struct inode *, struct dentry *, int, dev_t);
extern int vfs_symlink(struct inode *, struct dentry *, const char *);
extern int vfs_link(struct dentry *, struct inode *, struct dentry *);
extern int vfs_rmdir(struct inode *, struct dentry *);
extern int vfs_unlink(struct inode *, struct dentry *);
extern int vfs_rename(struct inode *, struct dentry *, struct inode *, struct dentry *);

/*
 * VFS dentry helper functions.
 */
extern void dentry_unhash(struct dentry *dentry);

/*
 * VFS file helper functions.
 */
extern int file_permission(struct file *, int);

/*
 * VFS FS_IOC_FIEMAP helper definitions.
 */
struct fiemap_extent_info {
	unsigned int fi_flags;		/* Flags as passed from user */
	unsigned int fi_extents_mapped;	/* Number of mapped extents */
	unsigned int fi_extents_max;	/* Size of fiemap_extent array */
	struct fiemap_extent *fi_extents_start; /* Start of fiemap_extent
						 * array */
};
int fiemap_fill_next_extent(struct fiemap_extent_info *info, u64 logical,
			    u64 phys, u64 len, u32 flags);
int fiemap_check_flags(struct fiemap_extent_info *fieinfo, u32 fs_flags);

/*
 * File types
 *
 * NOTE! These match bits 12..15 of stat.st_mode
 * (ie "(i_mode >> 12) & 15").
 */
#define DT_UNKNOWN	0
#define DT_FIFO		1
#define DT_CHR		2
#define DT_DIR		4
#define DT_BLK		6
#define DT_REG		8
#define DT_LNK		10
#define DT_SOCK		12
#define DT_WHT		14

/*
 * This is the "filldir" function type, used by readdir() to let
 * the kernel specify what kind of dirent layout it wants to have.
 * This allows the kernel to read directories into kernel space or
 * to have different dirent layouts depending on the binary type.
 */
typedef int (*filldir_t)(void *, const char *, int, loff_t, u64, unsigned);
struct block_device_operations;

/* These macros are for out of kernel modules to test that
 * the kernel supports the unlocked_ioctl and compat_ioctl
 * fields in struct file_operations. */
#define HAVE_COMPAT_IOCTL 1
#define HAVE_UNLOCKED_IOCTL 1

/*
 * NOTE:
 * read, write, poll, fsync, readv, writev, unlocked_ioctl and compat_ioctl
 * can be called without the big kernel lock held in all filesystems.
 */
// 其中包括进程针对已打开文件所能调用的方法,比如read()和write()等方法
// 文件操作表，其中的操作是UNIX系统调用的基础。具体文件系统可以为每一种操作做专门实现，或者如果存在通用操作，也可以使用通用操作。
struct file_operations {
	// 指向该操作表所在模块的指针，用于模块引用计数
	struct module *owner;
	// 该函数用于更新偏移量指针，由系统调用llseek()调用
	loff_t (*llseek) (struct file *, loff_t, int);
	// 从给定文件的offset偏移处读取count字节到buf中，由系统调用read()调用
	ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
	 // 从给定buf中取出count字节数据，写入给定文件的offset偏移处，更新文件指针，由系统调用write()调用
	ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
	// 从给定文件，以异步方式读取count字节的数据到buf中，由系统调用aio_read()调用
	ssize_t (*aio_read) (struct kiocb *, const struct iovec *, unsigned long, loff_t);
	// 以异步方式从给定buf中取出count字节数据，写入由iocb描述的文件中，由系统调用aio_write()调用
	ssize_t (*aio_write) (struct kiocb *, const struct iovec *, unsigned long, loff_t);
	// 返回目录列表中的下一个目录，由系统调用readdir()调用
	int (*readdir) (struct file *, void *, filldir_t);
	// 该函数睡眠等待给定的文件活动，由系统调用poll()调用
	unsigned int (*poll) (struct file *, struct poll_table_struct *);
	// 给设备发送命令参数对。当文件是一个被打开的设备节点时，可以通过它进行设置操作
	// 进行设置操作由系统调用ioctl调用，调用者必须持有BKL
	int (*ioctl) (struct inode *, struct file *, unsigned int, unsigned long);
	// 功能与ioctl()类似，但不需要调用者持有BKL，如果用户空间调用ioctl()系统调用，
	// VFS可以调用unlocked_ioctl(),它与ioctl()实现一个即可，一般优先实现unlocked_ioctl()
	long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
	// 是ioctl()函数的可移植变种，被32位应用程序用在64位系统上，不需要调用者持有BKL。目的是为64位系统提供32位ioctl的兼容方法。
	long (*compat_ioctl) (struct file *, unsigned int, unsigned long);
	// 将给定的文件映射到指定的地址空间上，由系统调用mmap()调用
	int (*mmap) (struct file *, struct vm_area_struct *);
	// 创建一个新的文件对象，并将它与相应索引节点对象关联，由系统调用open()调用
	int (*open) (struct inode *, struct file *);
	// 当已打开文件的引用计数减少时，该函数被VFS调用，作用根据具体文件系统而定
	int (*flush) (struct file *, fl_owner_t id);
	// 当文件最后一个引用被注销时（比如最后一个共享文件描述符的进程调用了close()或退出时），该函数被VFS调用
	int (*release) (struct inode *, struct file *);
	// 将给定文件的所有缓存数据写回磁盘，由系统调用fsync()调用
	int (*fsync) (struct file *, struct dentry *, int datasync);
	// 将iocb描述的文件所有缓存数据写回磁盘，由系统调用aio_fsync()调用
	int (*aio_fsync) (struct kiocb *, int datasync);
	// 用于打开或关闭异步I/O的通告信号
	int (*fasync) (int, struct file *, int);
	// 给指定文件上锁
	int (*lock) (struct file *, int, struct file_lock *);
	// 用来从一个文件向另一个文件发送数据
	ssize_t (*sendpage) (struct file *, struct page *, int, size_t, loff_t *, int);
	// 用来获取未使用的地址空间来映射给定的文件
	unsigned long (*get_unmapped_area)(struct file *, unsigned long, unsigned long, unsigned long, unsigned long);
	// 当给出SETFL命令时，该函数用来检查传递给fcntl()系统调用的flags的有效性。与大多数VFS操作一样，
	// 文件系统不需要实现check_flags()。目前只有NFS文件系统实现了。这个函数能使文件系统限制无效的SETFL标志，
	// 不限制的话，普通的fcntl()函数能使标志生效。在NFS文件系统中，不允许O_APPEND和O_DIRECT相结合。
	int (*check_flags)(int);
	// 实现文件锁定，提供POSIX兼容的锁定机制，由flock()系统调用
	int (*flock) (struct file *, int, struct file_lock *);
	// 实现splice写入，数据移动操作，由splice_write()系统调用
	ssize_t (*splice_write)(struct pipe_inode_info *, struct file *, loff_t *, size_t, unsigned int);
	// 实现splice读取，数据移动操作，由splice_read()系统调用
	ssize_t (*splice_read)(struct file *, loff_t *, struct pipe_inode_info *, size_t, unsigned int);
	// 设置或释放文件租约，由setlease()系统调用
	int (*setlease)(struct file *, long, struct file_lock **);
};

// 其中包括内核针对特定文件所能调用的方法，比如create()和link()等方法
// 描述了VFS用以操作索引节点对象的所有方法，这些方法由文件系统实现。
struct inode_operations {
	// VFS通过系统调用create()和open()来调用该函数，从而为dentry对象创建一个新的索引节点。创建时通过mode指定初始模式
	int (*create) (struct inode *dir,struct dentry *dentry,int mode, struct nameidata *);
	// 在特定目录中寻找索引节点，该索引节点要对应于dentry中给出的文件名
	struct dentry * (*lookup) (struct inode *dir,struct dentry *dentry, struct nameidata *);
	// 被系统调用link()定用，用来创建硬链接。硬链接名称由dentry参数指定，连接对象是dir目录中的old_dentry目录项所代表的文件
	int (*link) (struct dentry *old_dentry,struct inode *dir,struct dentry *dentry);
	// 该函数被系统调用unlink()调用，从目录dir中删除目录项dentry指定的索引节点对象
	int (*unlink) (struct inode *dir,struct dentry *dentry);
	// 该函数被系统调用symlink()调用，创建符号连接。符号连接名称由symname指定，连接对象是dir目录中的dentry目录项
	int (*symlink) (struct inode *dir,struct dentry *dentry,const char *symname);
	// 该函数被系统调用mkdir()调用，创建一个新目录。创建时使用mode指定的初始模式。
	int (*mkdir) (struct inode *dir,struct dentry *dentry,int mode);
	// 该函数被系统调用rmdir()调用，删除dir目录中的dentry目录项所代表的文件
	int (*rmdir) (struct inode *dir,struct dentry *dentry);
	// 该函数被mknod()调用，创建特殊文件（设备文件、命名管道或套间字）。要创建的文件放在dir目录中，其目录项为dentry，关联的设备为rdev，初始权限由mode指定。
	int (*mknod) (struct inode *dir,struct dentry *dentry,int mode,dev_t rdev);
	// VFS调用该函数来移动文件。文件源路经在old_dir目录中，源文件由old_dentry目录项指定，目标路径在new_dir目录中，目标文件由new_dentry指定。
	int (*rename) (struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry);
	// 被系统调用readlink()调用，拷贝数据到特定的缓冲buffer中。拷贝的数据来自dentry指定的符号连接，拷贝大小最大可达buflen字节。
	int (*readlink) (struct dentry *dentry, char __user *buffer,int bufelen);
	// 由VFS调用，从一个符号连接查找它指向的索引节点。由dentry指向的连接被解析，其结果存放在由nd指向的nameidata结构体中。
	void * (*follow_link) (struct dentry *dentry, struct nameidata *nd);
	// 在follow_link()调用后，该函数由VFS调用进行清除工作。
	void (*put_link) (struct dentry *, struct nameidata *, void *);
	// 由VFS调用，修改文件的大小。调用前，索引节点i_size项必须设置为预期的大小
	void (*truncate) (struct inode *);
	// 该函数用来检查给定的inode所代表的文件是否允许特定的访问模式，如果允许返回0,否则返回负值的错误码。
	// 多数文件系统都将此区域设置为NULL，使用VFS提供的通用方法进行检查。
	int (*permission) (struct inode *, int);
	// 验证对inode的访问控制列表（ACL）权限。
	int (*check_acl)(struct inode *, int);
	// 该函数被notify_change()调用，再修改索引节点后，通知发生了“改变事件”
	int (*setattr) (struct dentry *, struct iattr *);
	// 在通知索引节点需要从磁盘中更新时，VFS会调用该函数，拓展属性允许key/value这样的一对值与文件相关联。
	int (*getattr) (struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat);
	// 由VFS调用，给dentry指定的文件设置拓展属性。属性名为name，值为value
	int (*setxattr) (struct dentry *dentry, const char *name,const void *value,size_t size,int flags);
	// 由VFS调用，向value中拷贝给定文件的拓展属性name对应的数值。
	ssize_t (*getxattr) (struct dentry *dentry, const char *name, void *value, size_t size);
	// 将特定文件的所有属性列表拷贝到一个缓列表中
	ssize_t (*listxattr) (struct dentry *, char *, size_t);
	// 从给定文件中删除指定的属性
	int (*removexattr) (struct dentry *, const char *);
	// 截断文件的特定范围，由truncate_range()调用。
	void (*truncate_range)(struct inode *, loff_t, loff_t);
	// 文件空间分配，由fallocate()调用。
	long (*fallocate)(struct inode *inode, int mode, loff_t offset,
			  loff_t len);
	 // 映射文件的物理存储，由fiemap()调用。
	int (*fiemap)(struct inode *, struct fiemap_extent_info *, u64 start,
		      u64 len);
};

struct seq_file;

ssize_t rw_copy_check_uvector(int type, const struct iovec __user * uvector,
				unsigned long nr_segs, unsigned long fast_segs,
				struct iovec *fast_pointer,
				struct iovec **ret_pointer);

extern ssize_t vfs_read(struct file *, char __user *, size_t, loff_t *);
extern ssize_t vfs_write(struct file *, const char __user *, size_t, loff_t *);
extern ssize_t vfs_readv(struct file *, const struct iovec __user *,
		unsigned long, loff_t *);
extern ssize_t vfs_writev(struct file *, const struct iovec __user *,
		unsigned long, loff_t *);

// 其中包括内核针对特定文件系统所能调用的方法，比如write_inode()和sync_fs()等方法。
// 该结构体的每一项都是一个指向超级块操作函数的指针，超级块操作函数执行文件系统和索引节点的低层操作。
// 当文件系统需要对超级块执行操作时，首先要在超级块对象中寻找需要的操作方法，如文件系统要写自己超级块sb->s_op->write_super(sb);
struct super_operations {
	// 在给定的超级块下创建和初始化一个新的索引节点对象。
	struct inode *(*alloc_inode)(struct super_block *sb);
	// 用于释放给定索引节点
	void (*destroy_inode)(struct inode *);

	// VFS在索引节点脏（被修改）时会调用此函数。日志文件系统（如ext3和ext4）执行该函数进行日志更新。
	void (*dirty_inode) (struct inode *);
	// 用于将给定的索引节点写入磁盘。
	int (*write_inode) (struct inode *, struct writeback_control *wbc);
	// 在最后一个指向索引节点的引用被释放后，VFS会调用该函数。VFS只需要简单地删除这个索引节点后，普通UNIX文件系统就不会定义这个函数了。
	void (*drop_inode) (struct inode *);
	// 用于从磁盘上删除给定的索引节点
	void (*delete_inode) (struct inode *);
	// 在卸载文件系统时由VFS调用，用来释放超级块，调用者必须一直持有s_lock锁
	void (*put_super) (struct super_block *);
	// 用给定的超级块更新磁盘上的超级块。VFS通过该函数对内存中的超级块和磁盘上的超级块进行同步。调用者必须一直持有s_lock锁。
	void (*write_super) (struct super_block *);
	// 使文件系统的数据元与磁盘上的文件系统进行同步，wait参数指定操作是否同步。
	int (*sync_fs)(struct super_block *sb, int wait);
	// 冻结文件系统。
	int (*freeze_fs) (struct super_block *);
	// 解冻文件系统。
	int (*unfreeze_fs) (struct super_block *);
	// vfs通过该函数获取文件系统的状态
	int (*statfs) (struct dentry *, struct kstatfs *);
	// 当指定新的安装选项重新安装文件系统时，VFS会调用该函数。调用者必须一直持有s_lock锁。
	int (*remount_fs) (struct super_block *, int *, char *);
	// vfs调用该函数释放索引节点，并清空包含相关数据的所有页面
	void (*clear_inode) (struct inode *);
	// VFS调用该函数中断安装操作。该函数被网络文件系统使用，如NFS。
	void (*umount_begin) (struct super_block *);

	// 显示文件系统的选项。
	int (*show_options)(struct seq_file *, struct vfsmount *);
	// 显示文件系统的统计信息。
	int (*show_stats)(struct seq_file *, struct vfsmount *);
#ifdef CONFIG_QUOTA
	// 读取配额数据。
	ssize_t (*quota_read)(struct super_block *, int, char *, size_t, loff_t);
	// 写入配额数据。
	ssize_t (*quota_write)(struct super_block *, int, const char *, size_t, loff_t);
#endif
	// 尝试释放块设备的页面。
	int (*bdev_try_to_free_page)(struct super_block*, struct page*, gfp_t);
};

/*
 * Inode state bits.  Protected by inode_lock.
 *
 * Three bits determine the dirty state of the inode, I_DIRTY_SYNC,
 * I_DIRTY_DATASYNC and I_DIRTY_PAGES.
 *
 * Four bits define the lifetime of an inode.  Initially, inodes are I_NEW,
 * until that flag is cleared.  I_WILL_FREE, I_FREEING and I_CLEAR are set at
 * various stages of removing an inode.
 *
 * Two bits are used for locking and completion notification, I_NEW and I_SYNC.
 *
 * I_DIRTY_SYNC		Inode is dirty, but doesn't have to be written on
 *			fdatasync().  i_atime is the usual cause.
 * I_DIRTY_DATASYNC	Data-related inode changes pending. We keep track of
 *			these changes separately from I_DIRTY_SYNC so that we
 *			don't have to write inode on fdatasync() when only
 *			mtime has changed in it.
 * I_DIRTY_PAGES	Inode has dirty pages.  Inode itself may be clean.
 * I_NEW		Serves as both a mutex and completion notification.
 *			New inodes set I_NEW.  If two processes both create
 *			the same inode, one of them will release its inode and
 *			wait for I_NEW to be released before returning.
 *			Inodes in I_WILL_FREE, I_FREEING or I_CLEAR state can
 *			also cause waiting on I_NEW, without I_NEW actually
 *			being set.  find_inode() uses this to prevent returning
 *			nearly-dead inodes.
 * I_WILL_FREE		Must be set when calling write_inode_now() if i_count
 *			is zero.  I_FREEING must be set when I_WILL_FREE is
 *			cleared.
 * I_FREEING		Set when inode is about to be freed but still has dirty
 *			pages or buffers attached or the inode itself is still
 *			dirty.
 * I_CLEAR		Set by clear_inode().  In this state the inode is clean
 *			and can be destroyed.
 *
 *			Inodes that are I_WILL_FREE, I_FREEING or I_CLEAR are
 *			prohibited for many purposes.  iget() must wait for
 *			the inode to be completely released, then create it
 *			anew.  Other functions will just ignore such inodes,
 *			if appropriate.  I_NEW is used for waiting.
 *
 * I_SYNC		Synchonized write of dirty inode data.  The bits is
 *			set during data writeback, and cleared with a wakeup
 *			on the bit address once it is done.
 *
 * Q: What is the difference between I_WILL_FREE and I_FREEING?
 */
#define I_DIRTY_SYNC		1
#define I_DIRTY_DATASYNC	2
#define I_DIRTY_PAGES		4
#define __I_NEW			3
#define I_NEW			(1 << __I_NEW)
#define I_WILL_FREE		16
#define I_FREEING		32
#define I_CLEAR			64
#define __I_SYNC		7
#define I_SYNC			(1 << __I_SYNC)

#define I_DIRTY (I_DIRTY_SYNC | I_DIRTY_DATASYNC | I_DIRTY_PAGES)

extern void __mark_inode_dirty(struct inode *, int);
static inline void mark_inode_dirty(struct inode *inode)
{
	__mark_inode_dirty(inode, I_DIRTY);
}

static inline void mark_inode_dirty_sync(struct inode *inode)
{
	__mark_inode_dirty(inode, I_DIRTY_SYNC);
}

/**
 * inc_nlink - directly increment an inode's link count
 * @inode: inode
 *
 * This is a low-level filesystem helper to replace any
 * direct filesystem manipulation of i_nlink.  Currently,
 * it is only here for parity with dec_nlink().
 */
static inline void inc_nlink(struct inode *inode)
{
	inode->i_nlink++;
}

static inline void inode_inc_link_count(struct inode *inode)
{
	inc_nlink(inode);
	mark_inode_dirty(inode);
}

/**
 * drop_nlink - directly drop an inode's link count
 * @inode: inode
 *
 * This is a low-level filesystem helper to replace any
 * direct filesystem manipulation of i_nlink.  In cases
 * where we are attempting to track writes to the
 * filesystem, a decrement to zero means an imminent
 * write when the file is truncated and actually unlinked
 * on the filesystem.
 */
static inline void drop_nlink(struct inode *inode)
{
	inode->i_nlink--;
}

/**
 * clear_nlink - directly zero an inode's link count
 * @inode: inode
 *
 * This is a low-level filesystem helper to replace any
 * direct filesystem manipulation of i_nlink.  See
 * drop_nlink() for why we care about i_nlink hitting zero.
 */
static inline void clear_nlink(struct inode *inode)
{
	inode->i_nlink = 0;
}

static inline void inode_dec_link_count(struct inode *inode)
{
	drop_nlink(inode);
	mark_inode_dirty(inode);
}

/**
 * inode_inc_iversion - increments i_version
 * @inode: inode that need to be updated
 *
 * Every time the inode is modified, the i_version field will be incremented.
 * The filesystem has to be mounted with i_version flag
 */

static inline void inode_inc_iversion(struct inode *inode)
{
       spin_lock(&inode->i_lock);
       inode->i_version++;
       spin_unlock(&inode->i_lock);
}

extern void touch_atime(struct vfsmount *mnt, struct dentry *dentry);
static inline void file_accessed(struct file *file)
{
	if (!(file->f_flags & O_NOATIME))
		touch_atime(file->f_path.mnt, file->f_path.dentry);
}

int sync_inode(struct inode *inode, struct writeback_control *wbc);

// 用来描述各种特定文件系统类型，如ext3、ext4或UDF。
// 每个注册的文件系统都由该结构体来表示,该结构体描述了文件系统、功能、行为及其性能
// 每种文件系统不管被安装多少个实例到系统中，都只有一个file_system_type结构。
// 当文件系统被实际安装时，有一个vfsmount结构体在安装点被创建。该结构体用来代表文件系统的实例，即一个安装点
struct file_system_type {
	const char *name;		// 文件系统名字
	int fs_flags;		// 文件系统类型标志
	// get_sb()函数从磁盘读取超级块，并且在文件系统被安装时，在内存中组装超级块对象，剩余的函数描述文件系统属性。
	int (*get_sb) (struct file_system_type *, int,
		       const char *, void *, struct vfsmount *);		// 从磁盘中读取超级块
	// 终止访问超级块
	void (*kill_sb) (struct super_block *);
	struct module *owner;		// 文件系统模块
	struct file_system_type * next;		// 链表中下一个文件系统类型
	struct list_head fs_supers;				// 超级块对象链表

	// 以下字段运行时使锁生效
	struct lock_class_key s_lock_key;
	struct lock_class_key s_umount_key;

	struct lock_class_key i_lock_key;
	struct lock_class_key i_mutex_key;
	struct lock_class_key i_mutex_dir_key;
	struct lock_class_key i_alloc_sem_key;
};

extern int get_sb_ns(struct file_system_type *fs_type, int flags, void *data,
	int (*fill_super)(struct super_block *, void *, int),
	struct vfsmount *mnt);
extern int get_sb_bdev(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data,
	int (*fill_super)(struct super_block *, void *, int),
	struct vfsmount *mnt);
extern int get_sb_single(struct file_system_type *fs_type,
	int flags, void *data,
	int (*fill_super)(struct super_block *, void *, int),
	struct vfsmount *mnt);
extern int get_sb_nodev(struct file_system_type *fs_type,
	int flags, void *data,
	int (*fill_super)(struct super_block *, void *, int),
	struct vfsmount *mnt);
void generic_shutdown_super(struct super_block *sb);
void kill_block_super(struct super_block *sb);
void kill_anon_super(struct super_block *sb);
void kill_litter_super(struct super_block *sb);
void deactivate_super(struct super_block *sb);
void deactivate_locked_super(struct super_block *sb);
int set_anon_super(struct super_block *s, void *data);
struct super_block *sget(struct file_system_type *type,
			int (*test)(struct super_block *,void *),
			int (*set)(struct super_block *,void *),
			void *data);
extern int get_sb_pseudo(struct file_system_type *, char *,
	const struct super_operations *ops, unsigned long,
	struct vfsmount *mnt);
extern void simple_set_mnt(struct vfsmount *mnt, struct super_block *sb);
int __put_super_and_need_restart(struct super_block *sb);
void put_super(struct super_block *sb);

/* Alas, no aliases. Too much hassle with bringing module.h everywhere */
#define fops_get(fops) \
	(((fops) && try_module_get((fops)->owner) ? (fops) : NULL))
#define fops_put(fops) \
	do { if (fops) module_put((fops)->owner); } while(0)

// 注册和取消注册文件系统
// struct file_system_type 描述了您的文件系统。当请求将文件系统挂载到命名空间中的目录时，
// VFS 将为特定文件系统调用适当的 mount() 方法。引用 ->mount() 返回的树的新 vfsmount 
// 将附加到挂载点，以便当路径名解析到达挂载点时，它将跳转到该 vfsmount 的根。
extern int register_filesystem(struct file_system_type *);
extern int unregister_filesystem(struct file_system_type *);
extern struct vfsmount *kern_mount_data(struct file_system_type *, void *data);
#define kern_mount(type) kern_mount_data(type, NULL)
extern int may_umount_tree(struct vfsmount *);
extern int may_umount(struct vfsmount *);
extern long do_mount(char *, char *, char *, unsigned long, void *);
extern struct vfsmount *collect_mounts(struct path *);
extern void drop_collected_mounts(struct vfsmount *);
extern int iterate_mounts(int (*)(struct vfsmount *, void *), void *,
			  struct vfsmount *);
extern int vfs_statfs(struct dentry *, struct kstatfs *);

extern int current_umask(void);

/* /sys/fs */
extern struct kobject *fs_kobj;

extern int rw_verify_area(int, struct file *, loff_t *, size_t);

#define FLOCK_VERIFY_READ  1
#define FLOCK_VERIFY_WRITE 2

#ifdef CONFIG_FILE_LOCKING
extern int locks_mandatory_locked(struct inode *);
extern int locks_mandatory_area(int, struct inode *, struct file *, loff_t, size_t);

/*
 * Candidates for mandatory locking have the setgid bit set
 * but no group execute bit -  an otherwise meaningless combination.
 */

static inline int __mandatory_lock(struct inode *ino)
{
	return (ino->i_mode & (S_ISGID | S_IXGRP)) == S_ISGID;
}

/*
 * ... and these candidates should be on MS_MANDLOCK mounted fs,
 * otherwise these will be advisory locks
 */

static inline int mandatory_lock(struct inode *ino)
{
	return IS_MANDLOCK(ino) && __mandatory_lock(ino);
}

static inline int locks_verify_locked(struct inode *inode)
{
	if (mandatory_lock(inode))
		return locks_mandatory_locked(inode);
	return 0;
}

static inline int locks_verify_truncate(struct inode *inode,
				    struct file *filp,
				    loff_t size)
{
	if (inode->i_flock && mandatory_lock(inode))
		return locks_mandatory_area(
			FLOCK_VERIFY_WRITE, inode, filp,
			size < inode->i_size ? size : inode->i_size,
			(size < inode->i_size ? inode->i_size - size
			 : size - inode->i_size)
		);
	return 0;
}

static inline int break_lease(struct inode *inode, unsigned int mode)
{
	if (inode->i_flock)
		return __break_lease(inode, mode);
	return 0;
}
#else /* !CONFIG_FILE_LOCKING */
static inline int locks_mandatory_locked(struct inode *inode)
{
	return 0;
}

static inline int locks_mandatory_area(int rw, struct inode *inode,
				       struct file *filp, loff_t offset,
				       size_t count)
{
	return 0;
}

static inline int __mandatory_lock(struct inode *inode)
{
	return 0;
}

static inline int mandatory_lock(struct inode *inode)
{
	return 0;
}

static inline int locks_verify_locked(struct inode *inode)
{
	return 0;
}

static inline int locks_verify_truncate(struct inode *inode, struct file *filp,
					size_t size)
{
	return 0;
}

static inline int break_lease(struct inode *inode, unsigned int mode)
{
	return 0;
}

#endif /* CONFIG_FILE_LOCKING */

/* fs/open.c */

extern int do_truncate(struct dentry *, loff_t start, unsigned int time_attrs,
		       struct file *filp);
extern int do_fallocate(struct file *file, int mode, loff_t offset,
			loff_t len);
extern long do_sys_open(int dfd, const char __user *filename, int flags,
			int mode);
extern struct file *filp_open(const char *, int, int);
extern struct file * dentry_open(struct dentry *, struct vfsmount *, int,
				 const struct cred *);
extern int filp_close(struct file *, fl_owner_t id);
extern char * getname(const char __user *);

/* fs/ioctl.c */

extern int ioctl_preallocate(struct file *filp, void __user *argp);

/* fs/dcache.c */
extern void __init vfs_caches_init_early(void);
extern void __init vfs_caches_init(unsigned long);

extern struct kmem_cache *names_cachep;

#define __getname_gfp(gfp)	kmem_cache_alloc(names_cachep, (gfp))
#define __getname()		__getname_gfp(GFP_KERNEL)
#define __putname(name)		kmem_cache_free(names_cachep, (void *)(name))
#ifndef CONFIG_AUDITSYSCALL
#define putname(name)   __putname(name)
#else
extern void putname(const char *name);
#endif

#ifdef CONFIG_BLOCK
extern int register_blkdev(unsigned int, const char *);
extern void unregister_blkdev(unsigned int, const char *);
extern struct block_device *bdget(dev_t);
extern struct block_device *bdgrab(struct block_device *bdev);
extern void bd_set_size(struct block_device *, loff_t size);
extern void bd_forget(struct inode *inode);
extern void bdput(struct block_device *);
extern struct block_device *open_by_devnum(dev_t, fmode_t);
extern void invalidate_bdev(struct block_device *);
extern int sync_blockdev(struct block_device *bdev);
extern struct super_block *freeze_bdev(struct block_device *);
extern void emergency_thaw_all(void);
extern int thaw_bdev(struct block_device *bdev, struct super_block *sb);
extern int fsync_bdev(struct block_device *);
#else
static inline void bd_forget(struct inode *inode) {}
static inline int sync_blockdev(struct block_device *bdev) { return 0; }
static inline void invalidate_bdev(struct block_device *bdev) {}

static inline struct super_block *freeze_bdev(struct block_device *sb)
{
	return NULL;
}

static inline int thaw_bdev(struct block_device *bdev, struct super_block *sb)
{
	return 0;
}
#endif
extern int sync_filesystem(struct super_block *);
extern const struct file_operations def_blk_fops;
extern const struct file_operations def_chr_fops;
extern const struct file_operations bad_sock_fops;
extern const struct file_operations def_fifo_fops;
#ifdef CONFIG_BLOCK
extern int ioctl_by_bdev(struct block_device *, unsigned, unsigned long);
extern int blkdev_ioctl(struct block_device *, fmode_t, unsigned, unsigned long);
extern long compat_blkdev_ioctl(struct file *, unsigned, unsigned long);
extern int blkdev_get(struct block_device *, fmode_t);
extern int blkdev_put(struct block_device *, fmode_t);
extern int bd_claim(struct block_device *, void *);
extern void bd_release(struct block_device *);
#ifdef CONFIG_SYSFS
extern int bd_claim_by_disk(struct block_device *, void *, struct gendisk *);
extern void bd_release_from_disk(struct block_device *, struct gendisk *);
#else
#define bd_claim_by_disk(bdev, holder, disk)	bd_claim(bdev, holder)
#define bd_release_from_disk(bdev, disk)	bd_release(bdev)
#endif
#endif

/* fs/char_dev.c */
#define CHRDEV_MAJOR_HASH_SIZE	255
extern int alloc_chrdev_region(dev_t *, unsigned, unsigned, const char *);
extern int register_chrdev_region(dev_t, unsigned, const char *);
extern int __register_chrdev(unsigned int major, unsigned int baseminor,
			     unsigned int count, const char *name,
			     const struct file_operations *fops);
extern void __unregister_chrdev(unsigned int major, unsigned int baseminor,
				unsigned int count, const char *name);
extern void unregister_chrdev_region(dev_t, unsigned);
extern void chrdev_show(struct seq_file *,off_t);

static inline int register_chrdev(unsigned int major, const char *name,
				  const struct file_operations *fops)
{
	return __register_chrdev(major, 0, 256, name, fops);
}

static inline void unregister_chrdev(unsigned int major, const char *name)
{
	__unregister_chrdev(major, 0, 256, name);
}

/* fs/block_dev.c */
#define BDEVNAME_SIZE	32	/* Largest string for a blockdev identifier */
#define BDEVT_SIZE	10	/* Largest string for MAJ:MIN for blkdev */

#ifdef CONFIG_BLOCK
#define BLKDEV_MAJOR_HASH_SIZE	255
extern const char *__bdevname(dev_t, char *buffer);
extern const char *bdevname(struct block_device *bdev, char *buffer);
extern struct block_device *lookup_bdev(const char *);
extern struct block_device *open_bdev_exclusive(const char *, fmode_t, void *);
extern void close_bdev_exclusive(struct block_device *, fmode_t);
extern void blkdev_show(struct seq_file *,off_t);

#else
#define BLKDEV_MAJOR_HASH_SIZE	0
#endif

extern void init_special_inode(struct inode *, umode_t, dev_t);

/* Invalid inode operations -- fs/bad_inode.c */
extern void make_bad_inode(struct inode *);
extern int is_bad_inode(struct inode *);

extern const struct file_operations read_pipefifo_fops;
extern const struct file_operations write_pipefifo_fops;
extern const struct file_operations rdwr_pipefifo_fops;

extern int fs_may_remount_ro(struct super_block *);

#ifdef CONFIG_BLOCK
/*
 * return READ, READA, or WRITE
 */
#define bio_rw(bio)		((bio)->bi_rw & (RW_MASK | RWA_MASK))

/*
 * return data direction, READ or WRITE
 */
#define bio_data_dir(bio)	((bio)->bi_rw & 1)

extern void check_disk_size_change(struct gendisk *disk,
				   struct block_device *bdev);
extern int revalidate_disk(struct gendisk *);
extern int check_disk_change(struct block_device *);
extern int __invalidate_device(struct block_device *);
extern int invalidate_partition(struct gendisk *, int);
#endif
extern int invalidate_inodes(struct super_block *);
unsigned long invalidate_mapping_pages(struct address_space *mapping,
					pgoff_t start, pgoff_t end);

static inline void invalidate_remote_inode(struct inode *inode)
{
	if (S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	    S_ISLNK(inode->i_mode))
		invalidate_mapping_pages(inode->i_mapping, 0, -1);
}
extern int invalidate_inode_pages2(struct address_space *mapping);
extern int invalidate_inode_pages2_range(struct address_space *mapping,
					 pgoff_t start, pgoff_t end);
extern int write_inode_now(struct inode *, int);
extern int filemap_fdatawrite(struct address_space *);
extern int filemap_flush(struct address_space *);
extern int filemap_fdatawait(struct address_space *);
extern int filemap_fdatawait_range(struct address_space *, loff_t lstart,
				   loff_t lend);
extern int filemap_write_and_wait(struct address_space *mapping);
extern int filemap_write_and_wait_range(struct address_space *mapping,
				        loff_t lstart, loff_t lend);
extern int __filemap_fdatawrite_range(struct address_space *mapping,
				loff_t start, loff_t end, int sync_mode);
extern int filemap_fdatawrite_range(struct address_space *mapping,
				loff_t start, loff_t end);

extern int vfs_fsync_range(struct file *file, struct dentry *dentry,
			   loff_t start, loff_t end, int datasync);
extern int vfs_fsync(struct file *file, struct dentry *dentry, int datasync);
extern int generic_write_sync(struct file *file, loff_t pos, loff_t count);
extern void sync_supers(void);
extern void emergency_sync(void);
extern void emergency_remount(void);
#ifdef CONFIG_BLOCK
extern sector_t bmap(struct inode *, sector_t);
#endif
extern int notify_change(struct dentry *, struct iattr *);
extern int inode_permission(struct inode *, int);
extern int generic_permission(struct inode *, int,
		int (*check_acl)(struct inode *, int));

static inline bool execute_ok(struct inode *inode)
{
	return (inode->i_mode & S_IXUGO) || S_ISDIR(inode->i_mode);
}

extern int get_write_access(struct inode *);
extern int deny_write_access(struct file *);
static inline void put_write_access(struct inode * inode)
{
	atomic_dec(&inode->i_writecount);
}
static inline void allow_write_access(struct file *file)
{
	if (file)
		atomic_inc(&file->f_path.dentry->d_inode->i_writecount);
}
extern int do_pipe_flags(int *, int);
extern struct file *create_read_pipe(struct file *f, int flags);
extern struct file *create_write_pipe(int flags);
extern void free_write_pipe(struct file *);

extern struct file *do_filp_open(int dfd, const char *pathname,
		int open_flag, int mode, int acc_mode);
extern int may_open(struct path *, int, int);

extern int kernel_read(struct file *, loff_t, char *, unsigned long);
extern struct file * open_exec(const char *);
 
/* fs/dcache.c -- generic fs support functions */
extern int is_subdir(struct dentry *, struct dentry *);
extern int path_is_under(struct path *, struct path *);
extern ino_t find_inode_number(struct dentry *, struct qstr *);

#include <linux/err.h>

/* needed for stackable file system support */
extern loff_t default_llseek(struct file *file, loff_t offset, int origin);

extern loff_t vfs_llseek(struct file *file, loff_t offset, int origin);

extern int inode_init_always(struct super_block *, struct inode *);
extern void inode_init_once(struct inode *);
extern void inode_add_to_lists(struct super_block *, struct inode *);
extern void iput(struct inode *);
extern struct inode * igrab(struct inode *);
extern ino_t iunique(struct super_block *, ino_t);
extern int inode_needs_sync(struct inode *inode);
extern void generic_delete_inode(struct inode *inode);
extern void generic_drop_inode(struct inode *inode);
extern int generic_detach_inode(struct inode *inode);

extern struct inode *ilookup5_nowait(struct super_block *sb,
		unsigned long hashval, int (*test)(struct inode *, void *),
		void *data);
extern struct inode *ilookup5(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data);
extern struct inode *ilookup(struct super_block *sb, unsigned long ino);

extern struct inode * iget5_locked(struct super_block *, unsigned long, int (*test)(struct inode *, void *), int (*set)(struct inode *, void *), void *);
extern struct inode * iget_locked(struct super_block *, unsigned long);
extern int insert_inode_locked4(struct inode *, unsigned long, int (*test)(struct inode *, void *), void *);
extern int insert_inode_locked(struct inode *);
extern void unlock_new_inode(struct inode *);

extern void __iget(struct inode * inode);
extern void iget_failed(struct inode *);
extern void clear_inode(struct inode *);
extern void destroy_inode(struct inode *);
extern void __destroy_inode(struct inode *);
extern struct inode *new_inode(struct super_block *);
extern int should_remove_suid(struct dentry *);
extern int file_remove_suid(struct file *);

extern void __insert_inode_hash(struct inode *, unsigned long hashval);
extern void remove_inode_hash(struct inode *);
static inline void insert_inode_hash(struct inode *inode) {
	__insert_inode_hash(inode, inode->i_ino);
}

extern void file_move(struct file *f, struct list_head *list);
extern void file_kill(struct file *f);
#ifdef CONFIG_BLOCK
struct bio;
// 定义了submit_bio函数，其目的是将一个bio结构体提交到块设备层进行I/O操作。
extern void submit_bio(int, struct bio *);
extern int bdev_read_only(struct block_device *);
#endif
extern int set_blocksize(struct block_device *, int);
extern int sb_set_blocksize(struct super_block *, int);
extern int sb_min_blocksize(struct super_block *, int);

extern int generic_file_mmap(struct file *, struct vm_area_struct *);
extern int generic_file_readonly_mmap(struct file *, struct vm_area_struct *);
extern int file_read_actor(read_descriptor_t * desc, struct page *page, unsigned long offset, unsigned long size);
int generic_write_checks(struct file *file, loff_t *pos, size_t *count, int isblk);
extern ssize_t generic_file_aio_read(struct kiocb *, const struct iovec *, unsigned long, loff_t);
extern ssize_t __generic_file_aio_write(struct kiocb *, const struct iovec *, unsigned long,
		loff_t *);
extern ssize_t generic_file_aio_write(struct kiocb *, const struct iovec *, unsigned long, loff_t);
extern ssize_t generic_file_direct_write(struct kiocb *, const struct iovec *,
		unsigned long *, loff_t, loff_t *, size_t, size_t);
extern ssize_t generic_file_buffered_write(struct kiocb *, const struct iovec *,
		unsigned long, loff_t, loff_t *, size_t, ssize_t);
extern ssize_t do_sync_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos);
extern ssize_t do_sync_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos);
extern int generic_segment_checks(const struct iovec *iov,
		unsigned long *nr_segs, size_t *count, int access_flags);

/* fs/block_dev.c */
extern ssize_t blkdev_aio_write(struct kiocb *iocb, const struct iovec *iov,
				unsigned long nr_segs, loff_t pos);
extern int blkdev_fsync(struct file *filp, struct dentry *dentry, int datasync);

/* fs/splice.c */
extern ssize_t generic_file_splice_read(struct file *, loff_t *,
		struct pipe_inode_info *, size_t, unsigned int);
extern ssize_t default_file_splice_read(struct file *, loff_t *,
		struct pipe_inode_info *, size_t, unsigned int);
extern ssize_t generic_file_splice_write(struct pipe_inode_info *,
		struct file *, loff_t *, size_t, unsigned int);
extern ssize_t generic_splice_sendpage(struct pipe_inode_info *pipe,
		struct file *out, loff_t *, size_t len, unsigned int flags);
extern long do_splice_direct(struct file *in, loff_t *ppos, struct file *out,
		size_t len, unsigned int flags);

extern void
file_ra_state_init(struct file_ra_state *ra, struct address_space *mapping);
extern loff_t no_llseek(struct file *file, loff_t offset, int origin);
extern loff_t generic_file_llseek(struct file *file, loff_t offset, int origin);
extern loff_t generic_file_llseek_unlocked(struct file *file, loff_t offset,
			int origin);
extern int generic_file_open(struct inode * inode, struct file * filp);
extern int nonseekable_open(struct inode * inode, struct file * filp);

#ifdef CONFIG_FS_XIP
extern ssize_t xip_file_read(struct file *filp, char __user *buf, size_t len,
			     loff_t *ppos);
extern int xip_file_mmap(struct file * file, struct vm_area_struct * vma);
extern ssize_t xip_file_write(struct file *filp, const char __user *buf,
			      size_t len, loff_t *ppos);
extern int xip_truncate_page(struct address_space *mapping, loff_t from);
#else
static inline int xip_truncate_page(struct address_space *mapping, loff_t from)
{
	return 0;
}
#endif

#ifdef CONFIG_BLOCK
// 定义了直接IO操作的函数原型
ssize_t __blockdev_direct_IO(int rw, struct kiocb *iocb, struct inode *inode,
	struct block_device *bdev, const struct iovec *iov, loff_t offset,
	unsigned long nr_segs, get_block_t get_block, dio_iodone_t end_io,
	int lock_type);

enum {
	/* need locking between buffered and direct access */
	/* 需要在缓冲访问和直接访问之间进行锁定 */
	DIO_LOCKING	= 0x01,

	/* filesystem does not support filling holes */
	/* 文件系统不支持填充空洞 */
	DIO_SKIP_HOLES	= 0x02,
};

// 提供直接IO访问的标准版本，使用锁定和跳过空洞的处理
static inline ssize_t blockdev_direct_IO(int rw, struct kiocb *iocb,
	struct inode *inode, struct block_device *bdev, const struct iovec *iov,
	loff_t offset, unsigned long nr_segs, get_block_t get_block,
	dio_iodone_t end_io)
{
	return __blockdev_direct_IO(rw, iocb, inode, bdev, iov, offset,
				    nr_segs, get_block, end_io,
				    DIO_LOCKING | DIO_SKIP_HOLES);
}

// 提供直接IO访问的版本，不使用锁定机制
static inline ssize_t blockdev_direct_IO_no_locking(int rw, struct kiocb *iocb,
	struct inode *inode, struct block_device *bdev, const struct iovec *iov,
	loff_t offset, unsigned long nr_segs, get_block_t get_block,
	dio_iodone_t end_io)
{
	return __blockdev_direct_IO(rw, iocb, inode, bdev, iov, offset,
				nr_segs, get_block, end_io, 0);
}
#endif

extern const struct file_operations generic_ro_fops;

#define special_file(m) (S_ISCHR(m)||S_ISBLK(m)||S_ISFIFO(m)||S_ISSOCK(m))

extern int vfs_readlink(struct dentry *, char __user *, int, const char *);
extern int vfs_follow_link(struct nameidata *, const char *);
extern int page_readlink(struct dentry *, char __user *, int);
extern void *page_follow_link_light(struct dentry *, struct nameidata *);
extern void page_put_link(struct dentry *, struct nameidata *, void *);
extern int __page_symlink(struct inode *inode, const char *symname, int len,
		int nofs);
extern int page_symlink(struct inode *inode, const char *symname, int len);
extern const struct inode_operations page_symlink_inode_operations;
extern int generic_readlink(struct dentry *, char __user *, int);
extern void generic_fillattr(struct inode *, struct kstat *);
extern int vfs_getattr(struct vfsmount *, struct dentry *, struct kstat *);
void __inode_add_bytes(struct inode *inode, loff_t bytes);
void inode_add_bytes(struct inode *inode, loff_t bytes);
void inode_sub_bytes(struct inode *inode, loff_t bytes);
loff_t inode_get_bytes(struct inode *inode);
void inode_set_bytes(struct inode *inode, loff_t bytes);

extern int vfs_readdir(struct file *, filldir_t, void *);

extern int vfs_stat(char __user *, struct kstat *);
extern int vfs_lstat(char __user *, struct kstat *);
extern int vfs_fstat(unsigned int, struct kstat *);
extern int vfs_fstatat(int , char __user *, struct kstat *, int);

extern int do_vfs_ioctl(struct file *filp, unsigned int fd, unsigned int cmd,
		    unsigned long arg);
extern int __generic_block_fiemap(struct inode *inode,
				  struct fiemap_extent_info *fieinfo,
				  loff_t start, loff_t len,
				  get_block_t *get_block);
extern int generic_block_fiemap(struct inode *inode,
				struct fiemap_extent_info *fieinfo, u64 start,
				u64 len, get_block_t *get_block);

extern void get_filesystem(struct file_system_type *fs);
extern void put_filesystem(struct file_system_type *fs);
extern struct file_system_type *get_fs_type(const char *name);
extern struct super_block *get_super(struct block_device *);
extern struct super_block *get_active_super(struct block_device *bdev);
extern struct super_block *user_get_super(dev_t);
extern void drop_super(struct super_block *sb);

extern int dcache_dir_open(struct inode *, struct file *);
extern int dcache_dir_close(struct inode *, struct file *);
extern loff_t dcache_dir_lseek(struct file *, loff_t, int);
extern int dcache_readdir(struct file *, void *, filldir_t);
extern int simple_getattr(struct vfsmount *, struct dentry *, struct kstat *);
extern int simple_statfs(struct dentry *, struct kstatfs *);
extern int simple_link(struct dentry *, struct inode *, struct dentry *);
extern int simple_unlink(struct inode *, struct dentry *);
extern int simple_rmdir(struct inode *, struct dentry *);
extern int simple_rename(struct inode *, struct dentry *, struct inode *, struct dentry *);
extern int simple_sync_file(struct file *, struct dentry *, int);
extern int simple_empty(struct dentry *);
extern int simple_readpage(struct file *file, struct page *page);
extern int simple_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata);
extern int simple_write_end(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct page *page, void *fsdata);

extern struct dentry *simple_lookup(struct inode *, struct dentry *, struct nameidata *);
extern ssize_t generic_read_dir(struct file *, char __user *, size_t, loff_t *);
extern const struct file_operations simple_dir_operations;
extern const struct inode_operations simple_dir_inode_operations;
struct tree_descr { char *name; const struct file_operations *ops; int mode; };
struct dentry *d_alloc_name(struct dentry *, const char *);
extern int simple_fill_super(struct super_block *, int, struct tree_descr *);
extern int simple_pin_fs(struct file_system_type *, struct vfsmount **mount, int *count);
extern void simple_release_fs(struct vfsmount **mount, int *count);

extern ssize_t simple_read_from_buffer(void __user *to, size_t count,
			loff_t *ppos, const void *from, size_t available);

extern int simple_fsync(struct file *, struct dentry *, int);

#ifdef CONFIG_MIGRATION
extern int buffer_migrate_page(struct address_space *,
				struct page *, struct page *);
#else
#define buffer_migrate_page NULL
#endif

extern int inode_change_ok(const struct inode *, struct iattr *);
extern int inode_newsize_ok(const struct inode *, loff_t offset);
extern int __must_check inode_setattr(struct inode *, struct iattr *);

extern void file_update_time(struct file *file);

extern int generic_show_options(struct seq_file *m, struct vfsmount *mnt);
extern void save_mount_options(struct super_block *sb, char *options);
extern void replace_mount_options(struct super_block *sb, char *options);

static inline ino_t parent_ino(struct dentry *dentry)
{
	ino_t res;

	spin_lock(&dentry->d_lock);
	res = dentry->d_parent->d_inode->i_ino;
	spin_unlock(&dentry->d_lock);
	return res;
}

/* Transaction based IO helpers */

/*
 * An argresp is stored in an allocated page and holds the
 * size of the argument or response, along with its content
 */
struct simple_transaction_argresp {
	ssize_t size;
	char data[0];
};

#define SIMPLE_TRANSACTION_LIMIT (PAGE_SIZE - sizeof(struct simple_transaction_argresp))

char *simple_transaction_get(struct file *file, const char __user *buf,
				size_t size);
ssize_t simple_transaction_read(struct file *file, char __user *buf,
				size_t size, loff_t *pos);
int simple_transaction_release(struct inode *inode, struct file *file);

void simple_transaction_set(struct file *file, size_t n);

/*
 * simple attribute files
 *
 * These attributes behave similar to those in sysfs:
 *
 * Writing to an attribute immediately sets a value, an open file can be
 * written to multiple times.
 *
 * Reading from an attribute creates a buffer from the value that might get
 * read with multiple read calls. When the attribute has been read
 * completely, no further read calls are possible until the file is opened
 * again.
 *
 * All attributes contain a text representation of a numeric value
 * that are accessed with the get() and set() functions.
 */
#define DEFINE_SIMPLE_ATTRIBUTE(__fops, __get, __set, __fmt)		\
static int __fops ## _open(struct inode *inode, struct file *file)	\
{									\
	__simple_attr_check_format(__fmt, 0ull);			\
	return simple_attr_open(inode, file, __get, __set, __fmt);	\
}									\
static const struct file_operations __fops = {				\
	.owner	 = THIS_MODULE,						\
	.open	 = __fops ## _open,					\
	.release = simple_attr_release,					\
	.read	 = simple_attr_read,					\
	.write	 = simple_attr_write,					\
};

static inline void __attribute__((format(printf, 1, 2)))
__simple_attr_check_format(const char *fmt, ...)
{
	/* don't do anything, just let the compiler check the arguments; */
}

int simple_attr_open(struct inode *inode, struct file *file,
		     int (*get)(void *, u64 *), int (*set)(void *, u64),
		     const char *fmt);
int simple_attr_release(struct inode *inode, struct file *file);
ssize_t simple_attr_read(struct file *file, char __user *buf,
			 size_t len, loff_t *ppos);
ssize_t simple_attr_write(struct file *file, const char __user *buf,
			  size_t len, loff_t *ppos);

struct ctl_table;
int proc_nr_files(struct ctl_table *table, int write,
		  void __user *buffer, size_t *lenp, loff_t *ppos);

int __init get_filesystem_list(char *buf);

#define ACC_MODE(x) ("\004\002\006\006"[(x)&O_ACCMODE])
#define OPEN_FMODE(flag) ((__force fmode_t)((flag + 1) & O_ACCMODE))

#endif /* __KERNEL__ */
#endif /* _LINUX_FS_H */
