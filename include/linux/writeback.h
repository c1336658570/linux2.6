/*
 * include/linux/writeback.h
 */
#ifndef WRITEBACK_H
#define WRITEBACK_H

#include <linux/sched.h>
#include <linux/fs.h>

struct backing_dev_info;

// 用于操作inode锁的自旋锁。
extern spinlock_t inode_lock;
// 当前正在使用的inode链表，i_count > 0且 i_nlink > 0
extern struct list_head inode_in_use;
// 目前未被使用的inode节点链表，即尚在内存中没有销毁，但是没有进程使用，i_count为0。
extern struct list_head inode_unused;

/*
 * fs/fs-writeback.c
 */
/*
 * 文件系统写回逻辑的枚举类型，用于控制写回行为。
 */
enum writeback_sync_modes {
	/* 不等待任何操作 */
	WB_SYNC_NONE,	/* Don't wait on anything */
	/* 等待所有映射完成 */
	WB_SYNC_ALL,	/* Wait on every mapping */
};

/*
 * A control structure which tells the writeback code what to do.  These are
 * always on the stack, and hence need no locking.  They are always initialised
 * in a manner such that unspecified fields are set to zero.
 */
/*
 * 写回控制结构，告诉写回代码需要执行什么操作。这些结构通常在栈上初始化，
 * 因此不需要加锁。未指定的字段将初始化为零。
 */
struct writeback_control {
	/* 如果非NULL，只写回此队列 */
	struct backing_dev_info *bdi;	/* If !NULL, only write back this
					   queue */
	/* 如果非NULL，只写回这个超级块的inodes */
	struct super_block *sb;		/* if !NULL, only write inodes from
					   this super_block */
	/* 写回同步模式 */
	enum writeback_sync_modes sync_mode;
	/* 如果非NULL，只写回早于此时间的inodes */
	unsigned long *older_than_this;	/* If !NULL, only write back inodes
					   older than this */
	 /* 调用writeback_inodes_wb的时间，用于防止额外的作业和活锁 */
	unsigned long wb_start;         /* Time writeback_inodes_wb was
					   called. This is needed to avoid
					   extra jobs and livelock */
	/* 写回多少页面，并且每写回一页都会递减此计数 */
	long nr_to_write;		/* Write this many pages, and decrement
					   this for each page written */
	/* 未写回的页面数 */
	long pages_skipped;		/* Pages which were not written */

	/*
	 * For a_ops->writepages(): is start or end are non-zero then this is
	 * a hint that the filesystem need only write out the pages inside that
	 * byterange.  The byte at `end' is included in the writeout request.
	 */
	/*
	 * 对于a_ops->writepages()：如果start或end非零，则表明文件系统只需写回那个字节范围内的页面。
	 * `end`所指的字节也包括在写出请求中。
	 */
	loff_t range_start;
	loff_t range_end;

	/* 不阻塞在请求队列上 */
	unsigned nonblocking:1;		/* Don't get stuck on request queues */
	/* 输出：队列已满 */
	unsigned encountered_congestion:1; /* An output: a queue is full */
	/* kupdate方式的写回 */
	unsigned for_kupdate:1;		/* A kupdate writeback */
	/* 后台写回 */
	unsigned for_background:1;	/* A background writeback */
	/* 从页面分配器调用 */
	unsigned for_reclaim:1;		/* Invoked from the page allocator */
	/* range_start是循环的 */
	unsigned range_cyclic:1;	/* range_start is cyclic */
	/* 需要派发更多IO */
	unsigned more_io:1;		/* more io to be dispatched */
	/*
	 * write_cache_pages() won't update wbc->nr_to_write and
	 * mapping->writeback_index if no_nrwrite_index_update
	 * is set.  write_cache_pages() may write more than we
	 * requested and we want to make sure nr_to_write and
	 * writeback_index are updated in a consistent manner
	 * so we use a single control to update them
	 */
	/*
	 * 如果设置了no_nrwrite_index_update，则write_cache_pages()不会更新wbc->nr_to_write和
	 * mapping->writeback_index。write_cache_pages()可能会写回比请求更多的页面，我们希望确保nr_to_write和
	 * writeback_index以一致的方式更新，因此使用单个控制标志来更新它们。
	 */
	unsigned no_nrwrite_index_update:1;
};

/*
 * fs/fs-writeback.c
 */	
struct bdi_writeback;
int inode_wait(void *);
void writeback_inodes_sb(struct super_block *);
int writeback_inodes_sb_if_idle(struct super_block *);
void sync_inodes_sb(struct super_block *);
void writeback_inodes_wbc(struct writeback_control *wbc);
long wb_do_writeback(struct bdi_writeback *wb, int force_wait);
void wakeup_flusher_threads(long nr_pages);

/* writeback.h requires fs.h; it, too, is not included from here. */
static inline void wait_on_inode(struct inode *inode)
{
	might_sleep();
	wait_on_bit(&inode->i_state, __I_NEW, inode_wait, TASK_UNINTERRUPTIBLE);
}
static inline void inode_sync_wait(struct inode *inode)
{
	might_sleep();
	wait_on_bit(&inode->i_state, __I_SYNC, inode_wait,
							TASK_UNINTERRUPTIBLE);
}


/*
 * mm/page-writeback.c
 */
void laptop_io_completion(void);
void laptop_sync_completion(void);
void throttle_vm_writeout(gfp_t gfp_mask);

/* These are exported to sysctl. */
extern int dirty_background_ratio;
extern unsigned long dirty_background_bytes;
extern int vm_dirty_ratio;
extern unsigned long vm_dirty_bytes;
extern unsigned int dirty_writeback_interval;
extern unsigned int dirty_expire_interval;
extern int vm_highmem_is_dirtyable;
extern int block_dump;
extern int laptop_mode;

extern unsigned long determine_dirtyable_memory(void);

extern int dirty_background_ratio_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos);
extern int dirty_background_bytes_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos);
extern int dirty_ratio_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos);
extern int dirty_bytes_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos);

struct ctl_table;
int dirty_writeback_centisecs_handler(struct ctl_table *, int,
				      void __user *, size_t *, loff_t *);

void get_dirty_limits(unsigned long *pbackground, unsigned long *pdirty,
		      unsigned long *pbdi_dirty, struct backing_dev_info *bdi);

void page_writeback_init(void);
// 该函数用于平衡（或控制）特定地址空间中的脏页数。它通常在写入操作后调用，
// 以管理和限制系统中脏页的数量。这有助于系统保持稳定性，并确保不会因为过多的脏页而导致性能下降。
void balance_dirty_pages_ratelimited_nr(struct address_space *mapping,
					unsigned long nr_pages_dirtied);

// balance_dirty_pages_ratelimited_nr的简化版本，默认处理一个脏页。
// 这个函数简化了常见的调用场景，其中只有一个页面变脏。
static inline void
balance_dirty_pages_ratelimited(struct address_space *mapping)
{
	balance_dirty_pages_ratelimited_nr(mapping, 1);
}

typedef int (*writepage_t)(struct page *page, struct writeback_control *wbc,
				void *data);

// 通用的writepages方法进行页面写回
int generic_writepages(struct address_space *mapping,
		       struct writeback_control *wbc);
int write_cache_pages(struct address_space *mapping,
		      struct writeback_control *wbc, writepage_t writepage,
		      void *data);
// 用于处理写入页缓存的函数，do_writepages函数负责将脏页从页缓存写回到磁盘。
int do_writepages(struct address_space *mapping, struct writeback_control *wbc);
void set_page_dirty_balance(struct page *page, int page_mkwrite);
void writeback_set_ratelimit(void);

/* pdflush.c */
extern int nr_pdflush_threads;	/* Global so it can be exported to sysctl
				   read-only. */


#endif		/* WRITEBACK_H */
