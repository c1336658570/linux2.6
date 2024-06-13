/*
 * include/linux/backing-dev.h
 *
 * low-level device information and state which is propagated up through
 * to high-level code.
 */

#ifndef _LINUX_BACKING_DEV_H
#define _LINUX_BACKING_DEV_H

#include <linux/percpu_counter.h>
#include <linux/log2.h>
#include <linux/proportions.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/writeback.h>
#include <asm/atomic.h>

struct page;
struct device;
struct dentry;

// 与后备设备（通常指存储设备）相关的信息和操作，包括写回控制、设备拥塞状态监测等功能。

/*
 * Bits in backing_dev_info.state
 */
/*
 * backing_dev_info.state的位标识
 */
enum bdi_state {
	/* 正在激活的过程中 */
	BDI_pending,		/* On its way to being activated */
	/* 默认嵌入的写回控制已分配 */
	BDI_wb_alloc,		/* Default embedded wb allocated */
	/* 异步（写）队列正在变满 */
	BDI_async_congested,	/* The async (write) queue is getting full */
		/* 同步队列正在变满 */
	BDI_sync_congested,	/* The sync queue is getting full */
	/* 已完成bdi_register()调用 */
	BDI_registered,		/* bdi_register() was done */
	/* 从这里开始是可用的位 */
	BDI_unused,		/* Available bits start here */
};

// 定义函数指针类型，用于设备拥塞检测
typedef int (congested_fn)(void *, int);

/*
 * 定义不同类型的后备设备信息统计项。
 */
enum bdi_stat_item {
	BDI_RECLAIMABLE,	/* 可回收的: 这些项目可以被内存回收 */
	BDI_WRITEBACK,		/* 正在写回: 数据正在被写回磁盘 */
	NR_BDI_STAT_ITEMS	/* 后备设备信息统计项的数量 */
};

/*
 * 定义统计批处理的大小，依据CPU数量进行计算。
 */
#define BDI_STAT_BATCH (8*(1+ilog2(nr_cpu_ids)))

/*
 * 描述后备设备的写回信息。
 */
struct bdi_writeback {
	/* 挂在bdi下的列表 */
	struct list_head list;			/* hangs off the bdi */

	/* 指向父backing_dev_info的指针 */
	struct backing_dev_info *bdi;		/* our parent bdi */
	unsigned int nr;	/* 编号 */

	/* 上次老数据刷新时间 */
	unsigned long last_old_flush;		/* last old data flush */

	/* 写回任务 */
	struct task_struct	*task;		/* writeback task */
	/* 脏inode列表 */
	struct list_head	b_dirty;	/* dirty inodes */
	/* 等待写回的inode链表 */
	struct list_head	b_io;		/* parked for writeback */
	/* 需要更多写回处理的inode链表 */
	struct list_head	b_more_io;	/* parked for more writeback */
};


// address_space中有一个指针指向该结构，代表预读信息
/*
 * 后备设备信息结构。
 */
struct backing_dev_info {
	struct list_head bdi_list;	/* BDI列表 */
	struct rcu_head rcu_head;	/* 用于RCU同步的头部 */
	/* 最大预读大小，以PAGE_CACHE_SIZE为单位 */
	unsigned long ra_pages;	/* max readahead in PAGE_CACHE_SIZE units */
	/* 使用此变量时始终使用原子位操作 */
	unsigned long state;	/* Always use atomic bitops on this */
	/* 设备能力 */
	unsigned int capabilities; /* Device capabilities */
	/* 如果设备是md/dm，这是函数指针 */
	/*  拥塞检测函数 */
	congested_fn *congested_fn; /* Function pointer if device is md/dm */
	/* 拥塞函数的辅助数据指针 */
	void *congested_data;	/* Pointer to aux data for congested func */
	// 当有必要将堆积的写操作提交到设备时调用。
	// 触发设备IO的函数
	void (*unplug_io_fn)(struct backing_dev_info *, struct page *);
	// 触发IO函数的数据
	void *unplug_io_data;

	char *name;	// 设备的名称

	/* 每个CPU的设备统计数据 */
	struct percpu_counter bdi_stat[NR_BDI_STAT_ITEMS];

	// 完成度量的本地per-cpu属性
	/* 完成的操作统计 */
	struct prop_local_percpu completions;
	/* 脏页超出标记 */
	int dirty_exceeded;	// 标记脏页数超出限制

	// 最小比例（通常与回写相关）
	unsigned int min_ratio;
	// 最大比例和最大比例分数
	unsigned int max_ratio, max_prop_frac;

	/* 此bdi的默认写回信息 */
	struct bdi_writeback wb;  /* default writeback info for this bdi */
	/* 保护wb_list更新的锁 */
	spinlock_t wb_lock;	  /* protects update side of wb_list */
	/* 挂在这个bdi下的刷新线程 */
	struct list_head wb_list; /* the flusher threads hanging off this bdi */
	/* 已注册任务的位掩码 */
	unsigned long wb_mask;	  /* bitmask of registered tasks */
	/* 已注册任务的数量 */
	unsigned int wb_cnt;	  /* number of registered tasks */

	// 工作列表
	struct list_head work_list;

	// 关联的设备
	struct device *dev;

#ifdef CONFIG_DEBUG_FS
	struct dentry *debug_dir;
	struct dentry *debug_stats;
#endif
};

int bdi_init(struct backing_dev_info *bdi);
void bdi_destroy(struct backing_dev_info *bdi);

int bdi_register(struct backing_dev_info *bdi, struct device *parent,
		const char *fmt, ...);
int bdi_register_dev(struct backing_dev_info *bdi, dev_t dev);
void bdi_unregister(struct backing_dev_info *bdi);
int bdi_setup_and_register(struct backing_dev_info *, char *, unsigned int);
void bdi_start_writeback(struct backing_dev_info *bdi, struct super_block *sb,
				long nr_pages);
int bdi_writeback_task(struct bdi_writeback *wb);
int bdi_has_dirty_io(struct backing_dev_info *bdi);

extern spinlock_t bdi_lock;
extern struct list_head bdi_list;

static inline int wb_has_dirty_io(struct bdi_writeback *wb)
{
	return !list_empty(&wb->b_dirty) ||
	       !list_empty(&wb->b_io) ||
	       !list_empty(&wb->b_more_io);
}

static inline void __add_bdi_stat(struct backing_dev_info *bdi,
		enum bdi_stat_item item, s64 amount)
{
	__percpu_counter_add(&bdi->bdi_stat[item], amount, BDI_STAT_BATCH);
}

static inline void __inc_bdi_stat(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	__add_bdi_stat(bdi, item, 1);
}

static inline void inc_bdi_stat(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	unsigned long flags;

	local_irq_save(flags);
	__inc_bdi_stat(bdi, item);
	local_irq_restore(flags);
}

static inline void __dec_bdi_stat(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	__add_bdi_stat(bdi, item, -1);
}

static inline void dec_bdi_stat(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	unsigned long flags;

	local_irq_save(flags);
	__dec_bdi_stat(bdi, item);
	local_irq_restore(flags);
}

static inline s64 bdi_stat(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	return percpu_counter_read_positive(&bdi->bdi_stat[item]);
}

static inline s64 __bdi_stat_sum(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	return percpu_counter_sum_positive(&bdi->bdi_stat[item]);
}

static inline s64 bdi_stat_sum(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	s64 sum;
	unsigned long flags;

	local_irq_save(flags);
	sum = __bdi_stat_sum(bdi, item);
	local_irq_restore(flags);

	return sum;
}

extern void bdi_writeout_inc(struct backing_dev_info *bdi);

/*
 * maximal error of a stat counter.
 */
static inline unsigned long bdi_stat_error(struct backing_dev_info *bdi)
{
#ifdef CONFIG_SMP
	return nr_cpu_ids * BDI_STAT_BATCH;
#else
	return 1;
#endif
}

int bdi_set_min_ratio(struct backing_dev_info *bdi, unsigned int min_ratio);
int bdi_set_max_ratio(struct backing_dev_info *bdi, unsigned int max_ratio);

/*
 * Flags in backing_dev_info::capability
 *
 * The first three flags control whether dirty pages will contribute to the
 * VM's accounting and whether writepages() should be called for dirty pages
 * (something that would not, for example, be appropriate for ramfs)
 *
 * WARNING: these flags are closely related and should not normally be
 * used separately.  The BDI_CAP_NO_ACCT_AND_WRITEBACK combines these
 * three flags into a single convenience macro.
 *
 * BDI_CAP_NO_ACCT_DIRTY:  Dirty pages shouldn't contribute to accounting
 * BDI_CAP_NO_WRITEBACK:   Don't write pages back
 * BDI_CAP_NO_ACCT_WB:     Don't automatically account writeback pages
 *
 * These flags let !MMU mmap() govern direct device mapping vs immediate
 * copying more easily for MAP_PRIVATE, especially for ROM filesystems.
 *
 * BDI_CAP_MAP_COPY:       Copy can be mapped (MAP_PRIVATE)
 * BDI_CAP_MAP_DIRECT:     Can be mapped directly (MAP_SHARED)
 * BDI_CAP_READ_MAP:       Can be mapped for reading
 * BDI_CAP_WRITE_MAP:      Can be mapped for writing
 * BDI_CAP_EXEC_MAP:       Can be mapped for execution
 *
 * BDI_CAP_SWAP_BACKED:    Count shmem/tmpfs objects as swap-backed.
 */
#define BDI_CAP_NO_ACCT_DIRTY	0x00000001
#define BDI_CAP_NO_WRITEBACK	0x00000002
#define BDI_CAP_MAP_COPY	0x00000004
#define BDI_CAP_MAP_DIRECT	0x00000008
#define BDI_CAP_READ_MAP	0x00000010
#define BDI_CAP_WRITE_MAP	0x00000020
#define BDI_CAP_EXEC_MAP	0x00000040
#define BDI_CAP_NO_ACCT_WB	0x00000080
#define BDI_CAP_SWAP_BACKED	0x00000100

#define BDI_CAP_VMFLAGS \
	(BDI_CAP_READ_MAP | BDI_CAP_WRITE_MAP | BDI_CAP_EXEC_MAP)

#define BDI_CAP_NO_ACCT_AND_WRITEBACK \
	(BDI_CAP_NO_WRITEBACK | BDI_CAP_NO_ACCT_DIRTY | BDI_CAP_NO_ACCT_WB)

#if defined(VM_MAYREAD) && \
	(BDI_CAP_READ_MAP != VM_MAYREAD || \
	 BDI_CAP_WRITE_MAP != VM_MAYWRITE || \
	 BDI_CAP_EXEC_MAP != VM_MAYEXEC)
#error please change backing_dev_info::capabilities flags
#endif

extern struct backing_dev_info default_backing_dev_info;
extern struct backing_dev_info noop_backing_dev_info;
void default_unplug_io_fn(struct backing_dev_info *bdi, struct page *page);

int writeback_in_progress(struct backing_dev_info *bdi);

static inline int bdi_congested(struct backing_dev_info *bdi, int bdi_bits)
{
	if (bdi->congested_fn)
		return bdi->congested_fn(bdi->congested_data, bdi_bits);
	return (bdi->state & bdi_bits);
}

static inline int bdi_read_congested(struct backing_dev_info *bdi)
{
	return bdi_congested(bdi, 1 << BDI_sync_congested);
}

static inline int bdi_write_congested(struct backing_dev_info *bdi)
{
	return bdi_congested(bdi, 1 << BDI_async_congested);
}

static inline int bdi_rw_congested(struct backing_dev_info *bdi)
{
	return bdi_congested(bdi, (1 << BDI_sync_congested) |
				  (1 << BDI_async_congested));
}

enum {
	BLK_RW_ASYNC	= 0,
	BLK_RW_SYNC	= 1,
};

void clear_bdi_congested(struct backing_dev_info *bdi, int sync);
void set_bdi_congested(struct backing_dev_info *bdi, int sync);
long congestion_wait(int sync, long timeout);


static inline bool bdi_cap_writeback_dirty(struct backing_dev_info *bdi)
{
	return !(bdi->capabilities & BDI_CAP_NO_WRITEBACK);
}

static inline bool bdi_cap_account_dirty(struct backing_dev_info *bdi)
{
	return !(bdi->capabilities & BDI_CAP_NO_ACCT_DIRTY);
}

static inline bool bdi_cap_account_writeback(struct backing_dev_info *bdi)
{
	/* Paranoia: BDI_CAP_NO_WRITEBACK implies BDI_CAP_NO_ACCT_WB */
	return !(bdi->capabilities & (BDI_CAP_NO_ACCT_WB |
				      BDI_CAP_NO_WRITEBACK));
}

static inline bool bdi_cap_swap_backed(struct backing_dev_info *bdi)
{
	return bdi->capabilities & BDI_CAP_SWAP_BACKED;
}

static inline bool bdi_cap_flush_forker(struct backing_dev_info *bdi)
{
	return bdi == &default_backing_dev_info;
}

static inline bool mapping_cap_writeback_dirty(struct address_space *mapping)
{
	return bdi_cap_writeback_dirty(mapping->backing_dev_info);
}

static inline bool mapping_cap_account_dirty(struct address_space *mapping)
{
	return bdi_cap_account_dirty(mapping->backing_dev_info);
}

static inline bool mapping_cap_swap_backed(struct address_space *mapping)
{
	return bdi_cap_swap_backed(mapping->backing_dev_info);
}

/*
 * 调用调度器进行进程调度，通常用于I/O等待中。
 */
static inline int bdi_sched_wait(void *word)
{
	// 进行调度，让出CPU
	schedule();
	return 0;
}


/*
 * 运行给定的后备设备的解锁函数，通常在I/O请求后调用以优化I/O处理。
 */
static inline void blk_run_backing_dev(struct backing_dev_info *bdi,
				       struct page *page)
{
	// 检查bdi是否非空且具有unplug函数
	if (bdi && bdi->unplug_io_fn)
		// 调用解锁函数，传递后备设备信息和相关页面
		bdi->unplug_io_fn(bdi, page);
}

/*
 * 对给定的地址空间执行块设备操作，通常在完成对页的处理后调用以优化I/O处理。
 */
static inline void blk_run_address_space(struct address_space *mapping)
{
	// 如果映射存在
	if (mapping)
		 // 调用运行后备设备函数，传递地址空间的后备设备信息
		blk_run_backing_dev(mapping->backing_dev_info, NULL);
}

#endif		/* _LINUX_BACKING_DEV_H */
