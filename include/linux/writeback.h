/*
 * include/linux/writeback.h
 */
#ifndef WRITEBACK_H
#define WRITEBACK_H

#include <linux/sched.h>
#include <linux/fs.h>

struct backing_dev_info;	// 代表后台设备信息结构体

// 用于操作inode锁的自旋锁。
extern spinlock_t inode_lock;	// 用于控制对inode结构体的并发访问的自旋锁
// 当前正在使用的inode链表，i_count > 0且 i_nlink > 0
extern struct list_head inode_in_use;	// 包含所有当前被系统活跃使用的inode的链表，这些inode的引用计数大于0且链接数也大于0
// 目前未被使用的inode节点链表，即尚在内存中没有销毁，但是没有进程使用，i_count为0。
extern struct list_head inode_unused;	// 包含所有当前未被使用的inode的链表，这些inode的引用计数为0

/*
 * fs/fs-writeback.c
 */
/*
 * 文件系统写回逻辑的枚举类型，用于控制写回行为。
 */
// WB_SYNC_ALL 表示当遇到锁住的 inode 时，它必须等待该 inode 解锁，而不能跳过。WB_SYNC_NONE 表示跳过被锁住的 inode；
enum writeback_sync_modes {
	/* 不等待任何操作 */
	// 不等待被锁的 inode 解锁，直接跳过
	WB_SYNC_NONE,	/* Don't wait on anything */
	/* 等待所有映射完成 */
	// 等待所有映射完成，即在遇到被锁的 inode 时会等待其解锁
	WB_SYNC_ALL,	/* Wait on every mapping */
};

/*
 * A control structure which tells the writeback code what to do.  These are
 * always on the stack, and hence need no locking.  They are always initialised
 * in a manner such that unspecified fields are set to zero.
 */
/*
 * 写回控制结构，告诉写回代码应执行的操作。这些结构体通常在栈上创建，
 * 因此不需要锁定。它们总是以这样的方式初始化：未指定的字段设置为零。
 */
struct writeback_control {
	/* 如果非NULL，只写回此队列 */
	struct backing_dev_info *bdi;	/* If !NULL, only write back this
					   queue */
	/* 如果非NULL，只写回这个超级块的inodes */
	struct super_block *sb;		/* if !NULL, only write inodes from
					   this super_block */
	/* 写回操作的同步模式 */
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
	loff_t range_start;	/* 范围开始 */
	loff_t range_end;		/* 范围结束，`end`指定的字节包含在写出请求中 */

	/* 不阻塞在请求队列上 */
	unsigned nonblocking:1;		/* Don't get stuck on request queues */
	/* 输出：队列已满 */
	/* 输出：遇到了队列拥堵 */
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
	unsigned no_nrwrite_index_update:1;		/* 如果设置了此标志，write_cache_pages()不更新wbc->nr_to_write和mapping->writeback_index */
};

/*
 * fs/fs-writeback.c
 */

// 声明后台设备写回结构体
struct bdi_writeback;
// 等待inode操作完成
int inode_wait(void *);	// 等待inode操作完成的函数
// 将指定超级块的所有脏inode写回到磁盘
void writeback_inodes_sb(struct super_block *);	// 将指定超级块的所有脏inode写回到磁盘的函数
// 如果超级块空闲，就写回其所有脏inode
int writeback_inodes_sb_if_idle(struct super_block *);	// 如果超级块空闲，则写回其所有脏inode的函数
// 同步指定超级块的所有inode
void sync_inodes_sb(struct super_block *);	// 同步指定超级块的所有inode的函数
// 根据提供的写回控制结构体参数进行inode写回
void writeback_inodes_wbc(struct writeback_control *wbc);	// 根据提供的写回控制结构体参数进行inode写回的函数
// 执行写回操作，`force_wait`指定是否阻塞等待直到写回完成
long wb_do_writeback(struct bdi_writeback *wb, int force_wait);	// 执行写回操作的函数，`force_wait`指定是否阻塞等待直到写回完成
// 唤醒处理大量页面的写回线程
void wakeup_flusher_threads(long nr_pages);	// 唤醒处理指定数量页面的写回线程的函数

/* writeback.h requires fs.h; it, too, is not included from here. */
/* writeback.h需要fs.h文件；此处未包含。 */
// 静态内联函数，等待指定的inode完成初始化
static inline void wait_on_inode(struct inode *inode)
{
	might_sleep();	// 表示可能会睡眠的位置，用于调试
	wait_on_bit(&inode->i_state, __I_NEW, inode_wait, TASK_UNINTERRUPTIBLE);	// 在inode的i_state位上等待__I_NEW位清除
}
// 静态内联函数，等待指定的inode完成同步操作
static inline void inode_sync_wait(struct inode *inode)
{
	might_sleep();	// 表示可能会睡眠的位置，用于调试
	wait_on_bit(&inode->i_state, __I_SYNC, inode_wait,
							TASK_UNINTERRUPTIBLE);	// 在inode的i_state位上等待__I_SYNC位清除
}

/*
 * mm/page-writeback.c
 */
void laptop_io_completion(void);	// 笔记本电脑的I/O完成回调函数
void laptop_sync_completion(void);	// 笔记本电脑的同步完成回调函数
void throttle_vm_writeout(gfp_t gfp_mask);	// 控制虚拟内存系统的写出速度以减少写出引起的内存分配延迟

/* These are exported to sysctl. */
/* 以下变量被导出到sysctl用于系统调整。 */
extern int dirty_background_ratio;             // 在内存中脏页达到总内存的百分比触发后台写回
extern unsigned long dirty_background_bytes;   // 触发后台写回的脏页的具体字节数
extern int vm_dirty_ratio;                     // 在内存中脏页达到总内存的百分比触发前台写回
extern unsigned long vm_dirty_bytes;           // 触发前台写回的脏页的具体字节数
extern unsigned int dirty_writeback_interval;  // 脏页写回间隔时间（秒）
extern unsigned int dirty_expire_interval;     // 脏页可保留时间（秒）在被写回或丢弃前
extern int vm_highmem_is_dirtyable;            // 是否可以将高端内存页标记为脏
extern int block_dump;                         // 是否启用块设备的调试信息的输出
extern int laptop_mode;                        // 笔记本模式，用于调整系统的电源管理和硬盘的使用

// 计算系统中可标记为脏的内存总量
extern unsigned long determine_dirtyable_memory(void);

// sysctl处理函数，管理后台写回的脏页比例设置
extern int dirty_background_ratio_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos);
// sysctl处理函数，管理后台写回的脏页具体字节设置
extern int dirty_background_bytes_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos);
// sysctl处理函数，管理前台写回的脏页比例设置
extern int dirty_ratio_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos);
// sysctl处理函数，管理前台写回的脏页具体字节设置
extern int dirty_bytes_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos);

// 控制表结构体前向声明
struct ctl_table;
// sysctl处理函数，管理写回间隔的时间设置（以0.01秒为单位）
int dirty_writeback_centisecs_handler(struct ctl_table *, int,
				      void __user *, size_t *, loff_t *);

// 根据系统的脏页限制和指定的后台设备信息，获取脏页的限制
void get_dirty_limits(unsigned long *pbackground, unsigned long *pdirty,
		      unsigned long *pbdi_dirty, struct backing_dev_info *bdi);

// 初始化页面写回系统的函数
void page_writeback_init(void);
// 该函数用于平衡（或控制）特定地址空间中的脏页数。它通常在写入操作后调用，
// 以管理和限制系统中脏页的数量。这有助于系统保持稳定性，并确保不会因为过多的脏页而导致性能下降。
void balance_dirty_pages_ratelimited_nr(struct address_space *mapping,
					unsigned long nr_pages_dirtied);

// balance_dirty_pages_ratelimited_nr 的简化版本，默认处理一个脏页。
// 这个函数简化了常见的调用场景，其中只有一个页面变脏。
static inline void
balance_dirty_pages_ratelimited(struct address_space *mapping)
{
	balance_dirty_pages_ratelimited_nr(mapping, 1);
}

// 这是一个函数指针类型，用于页面的写操作。它接受一个页面对象、一个写回控制对象和额外的数据参数。
typedef int (*writepage_t)(struct page *page, struct writeback_control *wbc,
				void *data);

// 通用的writepages方法，用于执行地址空间的页面写回操作
int generic_writepages(struct address_space *mapping,
		       struct writeback_control *wbc);
// 用于写入页缓存的函数，遍历地址空间中的所有页面，并使用指定的writepage函数处理每个脏页
int write_cache_pages(struct address_space *mapping,
		      struct writeback_control *wbc, writepage_t writepage,
		      void *data);
// 用于处理写入页缓存的函数，do_writepages函数负责将脏页从页缓存写回到磁盘。
int do_writepages(struct address_space *mapping, struct writeback_control *wbc);
// 设置页面脏位并根据情况进行平衡，通常在页面因写操作被标记为脏时调用
// 在页面被标记为脏后，设置其脏位并进行必要的平衡	
void set_page_dirty_balance(struct page *page, int page_mkwrite);
// 设置写回操作的速率限制
void writeback_set_ratelimit(void);

/* pdflush.c */
// 全局变量，用于表示系统中pdflush线程的数量，可通过sysctl进行只读访问
extern int nr_pdflush_threads;	/* Global so it can be exported to sysctl
				   read-only. */	/* 全局变量，用于导出到sysctl的只读访问 */

#endif		/* WRITEBACK_H */
