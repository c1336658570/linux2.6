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
/*
 * 定义了一个枚举类型 bdi_state，这些枚举值用来表示与后备设备信息 (backing_dev_info) 相关的不同状态位。
 * 这些状态位反映了后备设备在其生命周期中的各种状态，例如是否正在被激活、是否已分配默认的嵌入式写回控制、
 * 队列是否拥堵以及设备是否已经注册等。这些状态有助于内核管理与存储设备相关的各种操作，确保数据的正确管理和流畅的
 * 数据写回操作。
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
/*
 * 定义了一个 struct bdi_writeback 数据结构，该结构用于描述后备设备（通常是存储设备）的写回信息。
 * 该结构包含了与写回操作相关的所有信息，包括与其相关联的 backing_dev_info 结构、管理脏inode的列表
 * 以及负责执行实际写回操作的线程等。此结构是操作系统中用于管理和调度I/O写回操作的关键部分，
 * 确保数据可以有效地写入底层存储设备。每个 bdi_writeback 结构都通过其 list 成员挂载在相应 
 * backing_dev_info 的 wb_list 链表上，由此组织多个写回队列以支持复杂的写回逻辑。
 */
// 管理一个块设备所有的回写任务。
struct bdi_writeback {
	/* 挂在bdi下的列表 */
	// 是backing_dev_info中wb_list链表的一个元素
	struct list_head list;			/* hangs off the bdi */

	/* 指向父backing_dev_info的指针 */
	struct backing_dev_info *bdi;		/* our parent bdi */
	// 该编号和backing_dev_info中的wb_mask相关，nr 编号是 n，backing_dev_info 中的 wb_mask 的第 n 位就被置为 1
	unsigned int nr;	/* 编号 */

	/* 上次老数据刷新时间 */
	unsigned long last_old_flush;		/* last old data flush */

	/* 写回任务线程 */
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
/*
 * 该数据结构描述了backing_dev的所有信息，通常块设备的request queue中会包含backing_dev对象。
 * 定义了后备设备信息，包括用于I/O预读和回写的各种控制信息，设备能力，以及与设备拥塞相关的功能。
 * 它支持一系列操作和状态监控，通过各种列表和计数器来跟踪和管理与设备相关的操作，如设备的脏页写回、
 * 预读控制等。此外，结构还包含指向特定设备的指针和调试信息，方便系统管理和问题诊断。
 */
// 描述一个块设备。
struct backing_dev_info {
	// 挂在全局的bdi_list下，bdi_list在mm/backing-dev.c定义
	struct list_head bdi_list;	/* 挂载于全局后备设备列表 */
	struct rcu_head rcu_head;	/* 用于RCU同步的头部 */
	/* 最大预读大小，以PAGE_CACHE_SIZE为单位 */
	unsigned long ra_pages;	/* max readahead in PAGE_CACHE_SIZE units */
	/* 使用此变量时始终使用原子位操作，其实是上面定义的enum bdi_state */
	unsigned long state;	/* Always use atomic bitops on this */
	/* 设备能力标志 */
	unsigned int capabilities; /* Device capabilities */
	/* 如果设备是md/dm，这是函数指针（如果设备是多设备或设备映射，此为拥塞检测函数指针） */
	/* 拥塞检测函数 */
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
	struct prop_local_percpu completions;	/* 用于统计完成的操作 */
	/* 脏页超出标记 */
	/* 脏页计数是否超过预设的限制 */
	int dirty_exceeded;	// 标记脏页数超出限制

	// 最小比例（通常与回写相关）
	unsigned int min_ratio;	/* 最小的回写比例 */
	// 最大比例和最大比例分数
	unsigned int max_ratio, max_prop_frac;	/* 最大的回写比例和比例分数 */

	/* 此bdi的默认写回信息结构 */
	struct bdi_writeback wb;  /* default writeback info for this bdi */
	/* 保护wb_list更新的锁 */
	spinlock_t wb_lock;	  /* protects update side of wb_list */
	/* 挂在这个bdi下的刷新线程 */
	// 该设备下的bdi_writeback链表
	struct list_head wb_list; /* the flusher threads hanging off this bdi */
	// bid_writeback 有一个 nr 编号字段，nr 编号是 n，wb_mask 的第 n 位就被置为 1
	/* 已注册任务的位掩码 */
	unsigned long wb_mask;	  /* bitmask of registered tasks */
	// 有多少个 bdi_writeback，wb_cnt就是多少
	/* 已注册任务的数量 */
	unsigned int wb_cnt;	  /* number of registered tasks */

	// 工作列表，连接struct bdi_work
	struct list_head work_list;	/* 工作项列表 */


	// 关联的设备
	struct device *dev;

#ifdef CONFIG_DEBUG_FS
	struct dentry *debug_dir;	/* 调试目录入口 */
	struct dentry *debug_stats;	/* 调试统计数据入口 */
#endif
};

// 初始化后备设备信息结构
int bdi_init(struct backing_dev_info *bdi);
// 销毁后备设备信息结构
void bdi_destroy(struct backing_dev_info *bdi);

// 注册后备设备信息结构，将其与父设备关联，并提供一个格式化的名称
int bdi_register(struct backing_dev_info *bdi, struct device *parent,
		const char *fmt, ...);
// 将后备设备信息结构关联到一个设备号
int bdi_register_dev(struct backing_dev_info *bdi, dev_t dev);
// 注销后备设备信息结构
void bdi_unregister(struct backing_dev_info *bdi);
// 设置并注册后备设备信息结构，为其命名并指定能力
int bdi_setup_and_register(struct backing_dev_info *, char *, unsigned int);
// 开始后备设备的写回操作，指定相关的超级块和页数
void bdi_start_writeback(struct backing_dev_info *bdi, struct super_block *sb,
				long nr_pages);
// 执行后备设备的写回任务
int bdi_writeback_task(struct bdi_writeback *wb);
// 检查后备设备是否有脏的I/O操作
int bdi_has_dirty_io(struct backing_dev_info *bdi);

extern spinlock_t bdi_lock;	// 全局的后备设备信息结构的锁
extern struct list_head bdi_list;	// 全局的后备设备信息列表

/*
 * 检查写回控制是否有脏的I/O操作。
 */
static inline int wb_has_dirty_io(struct bdi_writeback *wb)
{
	// 检查三个列表中是否有任何一个不为空：脏inode列表、等待写回的inode列表、需要更多写回处理的inode列表
	return !list_empty(&wb->b_dirty) ||
	       !list_empty(&wb->b_io) ||
	       !list_empty(&wb->b_more_io);
}

/*
 * 对后备设备统计信息中的特定项增加指定的数量。
 */
static inline void __add_bdi_stat(struct backing_dev_info *bdi,
		enum bdi_stat_item item, s64 amount)
{
	// 在后备设备的统计数据中，为指定项增加数量，使用BDI_STAT_BATCH作为批处理大小
	__percpu_counter_add(&bdi->bdi_stat[item], amount, BDI_STAT_BATCH);
}

/*
 * 增加后备设备统计信息中的特定项。
 */
static inline void __inc_bdi_stat(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	// 调用__add_bdi_stat函数，增加1到指定的统计项
	__add_bdi_stat(bdi, item, 1);
}

/*
 * 安全地增加后备设备统计信息中的特定项。
 */
static inline void inc_bdi_stat(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	unsigned long flags;

	local_irq_save(flags);	// 保存当前中断状态，并禁用本地中断
	__inc_bdi_stat(bdi, item);	// 无中断安全地增加统计项
	local_irq_restore(flags);	// 恢复之前的中断状态
}

/*
 * 减少后备设备统计信息中的特定项。
 */
static inline void __dec_bdi_stat(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	// 调用__add_bdi_stat函数，减少1从指定的统计项
	__add_bdi_stat(bdi, item, -1);
}

/*
 * 安全地减少后备设备统计信息中的特定项。
 */
static inline void dec_bdi_stat(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	unsigned long flags;

	// 保存当前中断状态，并禁用本地中断
	local_irq_save(flags);
	// 无中断安全地减少统计项
	__dec_bdi_stat(bdi, item);
	// 恢复之前的中断状态
	local_irq_restore(flags);
}

/*
 * 获取后备设备统计信息中的特定项的当前值。
 */
static inline s64 bdi_stat(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	// 从后备设备的per-cpu计数器中读取并返回正值
	return percpu_counter_read_positive(&bdi->bdi_stat[item]);
}

/*
 * 计算后备设备统计信息中的特定项的总和。
 */
static inline s64 __bdi_stat_sum(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	// 对后备设备的per-cpu计数器中的统计项求和，并返回正值
	return percpu_counter_sum_positive(&bdi->bdi_stat[item]);
}

/*
 * 安全地计算后备设备统计信息中的特定项的总和。
 */
static inline s64 bdi_stat_sum(struct backing_dev_info *bdi,
		enum bdi_stat_item item)
{
	s64 sum;	// 用于存储总和的变量
	unsigned long flags;	// 用于存储中断状态的变量

	local_irq_save(flags);	// 保存当前中断状态，并禁用本地中断
	sum = __bdi_stat_sum(bdi, item);	// 计算统计项的总和
	local_irq_restore(flags);	// 恢复之前的中断状态

	return sum;	// 返回计算得到的总和
}

/*
 * 增加后备设备的写出计数。
 */
// 此函数用于增加给定后备设备信息结构中记录的写出（即数据写回到存储设备）操作的计数。
extern void bdi_writeout_inc(struct backing_dev_info *bdi);

/*
 * maximal error of a stat counter.
 */
/*
 * 计算统计计数器的最大误差。
 */
static inline unsigned long bdi_stat_error(struct backing_dev_info *bdi)
{
#ifdef CONFIG_SMP
	// 在多处理器系统中，最大误差是处理器数量乘以统计批处理的大小
	return nr_cpu_ids * BDI_STAT_BATCH;
#else
	// 在单处理器系统中，最大误差为1
	return 1;
#endif
}

/*
 * 设置后备设备信息结构的最小回写比例。
 */
// 此函数用于设置指定后备设备信息结构中的最小回写比例，用于控制在特定条件下可以延迟回写的数据量。
int bdi_set_min_ratio(struct backing_dev_info *bdi, unsigned int min_ratio);
/*
 * 设置后备设备信息结构的最大回写比例。
 */
// 此函数用于设置指定后备设备信息结构中的最大回写比例，用于控制在特定条件下必须强制回写的数据量。
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
/*
 * 后备设备信息（backing_dev_info）的能力标志。
 *
 * 前三个标志控制脏页是否应该计入VM的账户，以及是否应该为脏页调用writepages()。
 * 例如，对于ramfs来说，这通常是不适当的。
 *
 * 警告：这些标志密切相关，通常不应单独使用。BDI_CAP_NO_ACCT_AND_WRITEBACK
 * 将这三个标志组合成一个方便的宏。
 *
 * BDI_CAP_NO_ACCT_DIRTY：脏页不应计入账户
 * BDI_CAP_NO_WRITEBACK：不写回页面
 * BDI_CAP_NO_ACCT_WB：不自动计入写回的页面
 *
 * 这些标志使得!MMU mmap()能够更容易地管理直接设备映射与立即复制之间的关系，特别是
 * 对于MAP_PRIVATE的ROM文件系统。
 *
 * BDI_CAP_MAP_COPY：可以映射副本（MAP_PRIVATE）
 * BDI_CAP_MAP_DIRECT：可以直接映射（MAP_SHARED）
 * BDI_CAP_READ_MAP：可以映射读取
 * BDI_CAP_WRITE_MAP：可以映射写入
 * BDI_CAP_EXEC_MAP：可以映射执行
 *
 * BDI_CAP_SWAP_BACKED：将shmem/tmpfs对象计为支持交换。
 */
#define BDI_CAP_NO_ACCT_DIRTY   0x00000001  // 脏页不计入VM的账户
#define BDI_CAP_NO_WRITEBACK    0x00000002  // 不对脏页执行写回
#define BDI_CAP_MAP_COPY        0x00000004  // 允许映射副本（对应私有映射）
#define BDI_CAP_MAP_DIRECT      0x00000008  // 允许直接映射（对应共享映射）
#define BDI_CAP_READ_MAP        0x00000010  // 允许映射读取
#define BDI_CAP_WRITE_MAP       0x00000020  // 允许映射写入
#define BDI_CAP_EXEC_MAP        0x00000040  // 允许映射执行
#define BDI_CAP_NO_ACCT_WB      0x00000080  // 写回页面不自动计入账户
#define BDI_CAP_SWAP_BACKED     0x00000100  // 将shmem/tmpfs计为支持交换

/*
 * 虚拟内存标志的后备设备信息能力标志集合。
 */
// 这个宏组合了读、写、执行映射的能力，便于设置或检查虚拟内存的权限。
#define BDI_CAP_VMFLAGS \
	(BDI_CAP_READ_MAP | BDI_CAP_WRITE_MAP | BDI_CAP_EXEC_MAP)

/*
 * 一个方便的宏，用于将不计入账户并且不写回的标志组合起来。
 */
// 这个宏组合了不写回、不将脏页计入账户以及不自动计入写回页的标志。
#define BDI_CAP_NO_ACCT_AND_WRITEBACK \
	(BDI_CAP_NO_WRITEBACK | BDI_CAP_NO_ACCT_DIRTY | BDI_CAP_NO_ACCT_WB)

/*
 * 如果虚拟内存权限标志（VM_MAYREAD、VM_MAYWRITE、VM_MAYEXEC）的定义
 * 和BDI_CAP_READ_MAP、BDI_CAP_WRITE_MAP、BDI_CAP_EXEC_MAP不一致，则编译错误。
 */
#if defined(VM_MAYREAD) && \
	(BDI_CAP_READ_MAP != VM_MAYREAD || \
	 BDI_CAP_WRITE_MAP != VM_MAYWRITE || \
	 BDI_CAP_EXEC_MAP != VM_MAYEXEC)
/* 这段预处理指令确保后备设备信息的能力标志与内核的虚拟内存权限标志一致，若不一致则发出编译错误。 */
#error please change backing_dev_info::capabilities flags
#endif

/*
 * 默认的后备设备信息和无操作的后备设备信息的外部声明。
 */
// 这两个变量分别表示系统默认的后备设备信息和一个无任何操作的后备设备信息。
extern struct backing_dev_info default_backing_dev_info;
extern struct backing_dev_info noop_backing_dev_info;
/*
 * 默认的设备拔插函数。
 */
// 这个函数是用来触发设备进行I/O操作的默认函数。
void default_unplug_io_fn(struct backing_dev_info *bdi, struct page *page);

/*
 * 检查指定的后备设备信息是否有正在进行的写回。
 */
// 这个函数用于检查给定的后备设备信息结构是否有正在进行的写回操作。
int writeback_in_progress(struct backing_dev_info *bdi);

/*
 * 判断指定后备设备信息是否拥堵。
 */
static inline int bdi_congested(struct backing_dev_info *bdi, int bdi_bits)
{
	// 如果定义了拥塞检测函数，调用该函数判断拥塞状态
	if (bdi->congested_fn)
		return bdi->congested_fn(bdi->congested_data, bdi_bits);
	// 否则，直接根据状态位判断拥塞状态
	return (bdi->state & bdi_bits);
}

/*
 * 判断后备设备的读操作是否拥堵。
 */
static inline int bdi_read_congested(struct backing_dev_info *bdi)
{
	// 检查同步操作是否拥堵
	return bdi_congested(bdi, 1 << BDI_sync_congested);
}

/*
 * 判断后备设备的写操作是否拥堵。
 */
static inline int bdi_write_congested(struct backing_dev_info *bdi)
{
	// 检查异步操作是否拥堵
	return bdi_congested(bdi, 1 << BDI_async_congested);
}

/*
 * 判断后备设备的读写操作是否拥堵。
 */
static inline int bdi_rw_congested(struct backing_dev_info *bdi)
{
	// 同时检查同步和异步操作是否拥堵
	return bdi_congested(bdi, (1 << BDI_sync_congested) |
				  (1 << BDI_async_congested));
}

/*
 * 枚举值，定义块设备读写操作的类型。
 */
enum {
	BLK_RW_ASYNC    = 0,  // 异步读写
	BLK_RW_SYNC     = 1,  // 同步读写
};

/*
 * 清除后备设备信息的拥塞状态。
 */
void clear_bdi_congested(struct backing_dev_info *bdi, int sync);
/*
 * 设置后备设备信息的拥塞状态。
 */
void set_bdi_congested(struct backing_dev_info *bdi, int sync);
/*
 * 等待拥塞解除或超时。
 */
long congestion_wait(int sync, long timeout);


/*
 * 判断后备设备是否允许写回脏页。
 */
static inline bool bdi_cap_writeback_dirty(struct backing_dev_info *bdi)
{
	// 如果没有设置BDI_CAP_NO_WRITEBACK，则返回true，表示允许写回。
	return !(bdi->capabilities & BDI_CAP_NO_WRITEBACK);
}

/*
 * 判断后备设备是否允许将脏页计入统计。
 */
static inline bool bdi_cap_account_dirty(struct backing_dev_info *bdi)
{
	// 如果没有设置BDI_CAP_NO_ACCT_DIRTY，则返回true，表示允许计入。
	return !(bdi->capabilities & BDI_CAP_NO_ACCT_DIRTY);
}

/*
 * 判断后备设备是否允许在写回时计入统计。
 */
static inline bool bdi_cap_account_writeback(struct backing_dev_info *bdi)
{
	/* Paranoia: BDI_CAP_NO_WRITEBACK implies BDI_CAP_NO_ACCT_WB */
	// 如果没有设置BDI_CAP_NO_ACCT_WB和BDI_CAP_NO_WRITEBACK，则返回true，表示允许计入。
	return !(bdi->capabilities & (BDI_CAP_NO_ACCT_WB |
				      BDI_CAP_NO_WRITEBACK));
}

/*
 * 判断后备设备是否被标记为支持交换。
 */
static inline bool bdi_cap_swap_backed(struct backing_dev_info *bdi)
{
	// 如果设置了BDI_CAP_SWAP_BACKED，则返回true。
	return bdi->capabilities & BDI_CAP_SWAP_BACKED;
}

/*
 * 检查后备设备信息是否为默认的后备设备信息。
 */
static inline bool bdi_cap_flush_forker(struct backing_dev_info *bdi)
{
	// 如果bdi指向默认的后备设备信息，则返回true。
	return bdi == &default_backing_dev_info;
}

/*
 * 判断映射区是否可以进行写回脏页操作。
 */
static inline bool mapping_cap_writeback_dirty(struct address_space *mapping)
{
	// 调用bdi_cap_writeback_dirty来判断映射区的后备设备信息是否允许写回脏页。
	return bdi_cap_writeback_dirty(mapping->backing_dev_info);
}

/*
 * 判断映射区是否允许将脏页计入统计。
 */
static inline bool mapping_cap_account_dirty(struct address_space *mapping)
{
	// 调用bdi_cap_account_dirty来判断映射区的后备设备信息是否允许计入脏页。
	return bdi_cap_account_dirty(mapping->backing_dev_info);
}

/*
 * 判断映射区是否被标记为支持交换。
 */
static inline bool mapping_cap_swap_backed(struct address_space *mapping)
{
	// 调用bdi_cap_swap_backed来判断映射区的后备设备信息是否被标记为支持交换。
	return bdi_cap_swap_backed(mapping->backing_dev_info);
}

/*
 * 调用调度器进行进程调度，通常用于I/O等待中。
 */
static inline int bdi_sched_wait(void *word)
{
	// 执行调度操作，让出当前CPU，以便其他进程或线程可以运行
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
		// 调用该函数，可能用于启动或激活设备上挂起的I/O操作
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
