/*
 * fs/fs-writeback.c
 *
 * Copyright (C) 2002, Linus Torvalds.
 *
 * Contains all the functions related to writing back and waiting
 * upon dirty inodes against superblocks, and writing back dirty
 * pages against inodes.  ie: data writeback.  Writeout of the
 * inode itself is not handled here.
 *
 * 10Apr2002	Andrew Morton
 *		Split out of fs/inode.c
 *		Additions for address_space-based writeback
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/buffer_head.h>
#include "internal.h"

/*
 * 将inode转换为其所在的后备设备信息。
 */
#define inode_to_bdi(inode)	((inode)->i_mapping->backing_dev_info)

/*
 * We don't actually have pdflush, but this one is exported though /proc...
 */
/*
 * 我们实际上没有pdflush，但这个变量通过/proc导出...
 */
/*
 * 记录系统中pdflush线程的数量（虽然现在可能不使用pdflush，但变量仍被保留）。
 */
int nr_pdflush_threads;

/*
 * Passed into wb_writeback(), essentially a subset of writeback_control
 */
/*
 * 传递给wb_writeback()，本质上是writeback_control的子集
 */
/*
 * 定义了写回操作的具体参数，用于控制写回的行为和范围。
 */
struct wb_writeback_args {
	long nr_pages;	/* 要写回的页面数 */
	struct super_block *sb;	/* 特定的文件系统超级块 */
	/* 同步模式，如同步或异步 */
	enum writeback_sync_modes sync_mode;
	/* 是否为定期唤醒的kupdate写回 */
	int for_kupdate:1;
	int range_cyclic:1;	/* 是否周期性地写回 */
	int for_background:1;	/* 是否为后台写回 */
};

/*
 * Work items for the bdi_writeback threads
 */
/*
 * 后备设备写回线程的工作项
 */
// 描述需要回写的任务。
struct bdi_work {
	/* 待处理工作列表 */
	// 挂在backing_dev_info的work_list下
	struct list_head list;		/* pending work list */
	/* 用于RCU释放或清理工作项 */
	struct rcu_head rcu_head;	/* for RCU free/clear of work */

	/*
	 * 下面这两个参数会在 bdi_queue_work 中设置，seen 是一个位图掩码，将 backing_dev_info 的 wb_mask 赋值给seen，
	 * 代表有哪些 bdi_writeback 可以看到该工作，pending 设置为 wb_cnt，wb_cnt是一共有多少个bdi_writeback
	 */
	/* 观察到这项工作的线程数 */
	unsigned long seen;		/* threads that have seen this work */
	/* 还需处理这项工作的线程数 */
	atomic_t pending;		/* number of threads still to do work */

	/* 写回操作的参数 */
	struct wb_writeback_args args;	/* writeback arguments */
	
	/* 状态标志位，参见WS_* */
	unsigned long state;		/* flag bits, see WS_* */
};

/*
 * 定义状态标志位的枚举，用于描述工作项的状态。
 */
enum {
	WS_USED_B = 0,	/* 标志位，用于表示某个资源或对象已被使用 */
	WS_ONSTACK_B,		/* 标志位，用于表示某个资源或对象位于堆栈上 */
};

/*
 * WS_USED: 表示这个工作项已经被使用。
 * WS_ONSTACK: 表示这个工作项位于栈上。
 */
#define WS_USED (1 << WS_USED_B)
#define WS_ONSTACK (1 << WS_ONSTACK_B)

/*
 * 检查工作项是否在栈上。返回true如果工作项的状态标志中WS_ONSTACK位被设置。
 */
static inline bool bdi_work_on_stack(struct bdi_work *work)
{
	return test_bit(WS_ONSTACK_B, &work->state);
}

/*
 * 初始化一个bdi_work结构。设置其RCU头部，复制给定的写回参数到工作项，并标记状态为已使用。
 */
static inline void bdi_work_init(struct bdi_work *work,
				 struct wb_writeback_args *args)
{
	INIT_RCU_HEAD(&work->rcu_head);
	work->args = *args;	// 复制写回控制参数
	work->state = WS_USED;	// 设置状态为 "已使用"
}

/**
 * writeback_in_progress - determine whether there is writeback in progress
 * @bdi: the device's backing_dev_info structure.
 *
 * Determine whether there is writeback waiting to be handled against a
 * backing device.
 */
/**
 * writeback_in_progress - 判断是否有正在进行的写回
 * @bdi: 设备的 backing_dev_info 结构体。
 *
 * 判断指定后备设备是否有待处理的写回操作。
 * 这个函数检查后备设备信息结构（bdi）的工作列表是否为空。
 * 如果不为空，则表示存在等待处理的写回任务。
 */
/*
 * 检查是否有针对某个后备设备的写回正在进行中。
 * 如果bdi的工作列表不为空，表示有写回任务待处理，返回true。
 */
int writeback_in_progress(struct backing_dev_info *bdi)
{
	return !list_empty(&bdi->work_list);	// 检查bdi的工作列表是否为空，不为空返回true
}

/*
 * 清除工作项的使用状态，并通过内存屏障确保操作的顺序性。唤醒等待这个状态位的所有线程。
 */
static void bdi_work_clear(struct bdi_work *work)
{
	// 清除工作项的 '已使用' 状态
	clear_bit(WS_USED_B, &work->state);
	smp_mb__after_clear_bit();	// 在清除位后立即执行内存屏障，保证操作的顺序性
	/*
	 * work can have disappeared at this point. bit waitq functions
	 * should be able to tolerate this, provided bdi_sched_wait does
	 * not dereference it's pointer argument.
	*/
	/*
	 * 此时work可能已经不存在。等待队列函数应该能够处理这种情况，只要bdi_sched_wait
	 * 不解引用它的指针参数。
	 */
	// 唤醒所有在这个状态位上等待的线程
	wake_up_bit(&work->state, WS_USED_B);
}

/*
 * 释放一个bdi_work结构。如果工作项不在栈上，则直接释放它的内存。
 * 如果在栈上，则清除它的使用状态。
 */
static void bdi_work_free(struct rcu_head *head)
{
	// 从 RCU 头部获取 bdi_work 结构
	struct bdi_work *work = container_of(head, struct bdi_work, rcu_head);

	if (!bdi_work_on_stack(work))
		kfree(work);	// 如果不是栈上分配，则释放内存
	else
		// 如果是栈上分配，则清除状态
		bdi_work_clear(work);
}

/*
 * 完成写回工作的清理操作。根据工作项是在堆上还是栈上，采取不同的清理策略。
 * - 如果工作项不在栈上，立即清除已完成/已查看标志。
 * - 如果工作项在栈上，需要延迟清除和释放操作，直到 RCU 宽限期之后，
 *   因为在 bdi_work_clear() 完成唤醒后，栈可能失效。
 */
static void wb_work_complete(struct bdi_work *work)
{
	const enum writeback_sync_modes sync_mode = work->args.sync_mode;	// 获取写回操作的同步模式
	int onstack = bdi_work_on_stack(work);	// 检查工作项是否在栈上

	/*
	 * For allocated work, we can clear the done/seen bit right here.
	 * For on-stack work, we need to postpone both the clear and free
	 * to after the RCU grace period, since the stack could be invalidated
	 * as soon as bdi_work_clear() has done the wakeup.
	 */
	/*
	 * 对于分配的工作项，我们可以在这里直接清除已完成/已查看标志。
	 * 对于栈上的工作项，我们需要将清除和释放的操作推迟到RCU宽限期之后，
	 * 因为一旦bdi_work_clear()完成唤醒操作，栈可能就会失效。
	 */
	if (!onstack)
		bdi_work_clear(work);	// 如果不在栈上，立即清除状态
	if (sync_mode == WB_SYNC_NONE || onstack)
		// 如果是非同步模式或在栈上，安排 RCU 释放
		call_rcu(&work->rcu_head, bdi_work_free);	// 如果是非同步模式或在栈上，通过RCU机制延迟释放
}

/*
 * 清除挂起的工作项。检查工作项是否已完成所有任务，如果是，则进行清理。
 * - 从工作项中检索到参数后，释放对其的引用。
 * - 如果这是最后一次引用，则删除并释放它。
 */
static void wb_clear_pending(struct bdi_writeback *wb, struct bdi_work *work)
{
	/*
	 * The caller has retrieved the work arguments from this work,
	 * drop our reference. If this is the last ref, delete and free it
	 */
	/*
	 * 调用者已经从此工作项中检索了工作参数，
	 * 释放我们的引用。如果这是最后一个引用，则删除并释放它。
	 */
	// 原子减少 pending 计数，并检查是否为0
	if (atomic_dec_and_test(&work->pending)) {
		// 获取对应的后备设备信息
		struct backing_dev_info *bdi = wb->bdi;

		spin_lock(&bdi->wb_lock);
		// 从列表中删除
		list_del_rcu(&work->list);
		spin_unlock(&bdi->wb_lock);

		// 完成工作的清理
		wb_work_complete(work);
	}
}

/*
 * 将工作项排队到后备设备信息结构中。确保工作项被正确地添加到工作列表，
 * 并在必要时唤醒处理写回的线程。
 */
static void bdi_queue_work(struct backing_dev_info *bdi, struct bdi_work *work)
{
	work->seen = bdi->wb_mask;	 // 设置工作项已被查看的掩码
	BUG_ON(!work->seen);				// 断言检查确保工作项被标记为已查看
	// 设置待处理工作的计数
	atomic_set(&work->pending, bdi->wb_cnt);
	BUG_ON(!bdi->wb_cnt);	// 断言检查确保有等待处理的工作

	/*
	 * list_add_tail_rcu() contains the necessary barriers to
	 * make sure the above stores are seen before the item is
	 * noticed on the list
	 */
	/*
	 * list_add_tail_rcu() 包含必要的屏障，
	 * 确保上述存储在项目出现在列表上之前被看到。
	 */
	// 对后备设备信息的写回锁进行加锁
	spin_lock(&bdi->wb_lock);
	// 将工作项添加到工作列表的末尾
	list_add_tail_rcu(&work->list, &bdi->work_list);
	spin_unlock(&bdi->wb_lock);	// 解锁

	/*
	 * If the default thread isn't there, make sure we add it. When
	 * it gets created and wakes up, we'll run this work.
	 */
	/*
	 * 如果默认线程不存在，确保我们添加它。当它被创建并唤醒时，我们将运行这个工作。
	 */
	// 检查默认的写回列表是否为空
	if (unlikely(list_empty_careful(&bdi->wb_list)))
		// 如果为空，唤醒默认的写回任务
		wake_up_process(default_backing_dev_info.wb.task);
	else {
		// 获取写回控制结构
		struct bdi_writeback *wb = &bdi->wb;

		if (wb->task)	// 如果存在写回任务
			// 唤醒该任务以处理写回
			wake_up_process(wb->task);
	}
}

/*
 * Used for on-stack allocated work items. The caller needs to wait until
 * the wb threads have acked the work before it's safe to continue.
 */
/*
 * 用于在栈上分配的工作项。调用者需要等待写回线程确认工作完成，然后才能安全地继续。
 */
static void bdi_wait_on_work_clear(struct bdi_work *work)
{
	// 使用 wait_on_bit() 函数等待指定的工作项完成处理。
	// 等待工作项的状态位 WS_USED_B 被清除
	wait_on_bit(&work->state, WS_USED_B, bdi_sched_wait,
		    TASK_UNINTERRUPTIBLE);	// 使用指定的等待函数和不可中断的任务状态进行等待
}

/*
 * 该函数尝试为非同步写回操作分配并初始化一个 bdi_work 工作项。如果内存分配成功，工作项将被初始化并加入到后备
 * 设备信息（BDI）的工作队列中。如果内存分配失败，则尝试唤醒后备设备信息中的默认写回线程，以处理旧的脏数据的写回。
 */
static void bdi_alloc_queue_work(struct backing_dev_info *bdi,
				 struct wb_writeback_args *args)
{
	struct bdi_work *work;

	/*
	 * This is WB_SYNC_NONE writeback, so if allocation fails just
	 * wakeup the thread for old dirty data writeback
	 */
	/*
   * 这是一个非同步（WB_SYNC_NONE）的写回操作，如果内存分配失败，就唤醒线程来写回旧的脏数据。
   */
	// 尝试分配一个工作项。
	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	if (work) {
		// 如果成功分配，则初始化工作项并将其排队。
		bdi_work_init(work, args);
		bdi_queue_work(bdi, work);
	} else {
		// 如果内存分配失败，尝试唤醒后备设备信息中的默认写回线程。
		struct bdi_writeback *wb = &bdi->wb;	// 获取后备设备的写回控制结构

		// 如果存在写回任务
		if (wb->task)
			// 唤醒该任务以处理写回
			wake_up_process(wb->task);
	}
}

/**
 * bdi_sync_writeback - start and wait for writeback
 * @bdi: the backing device to write from
 * @sb: write inodes from this super_block
 *
 * Description:
 *   This does WB_SYNC_ALL data integrity writeback and waits for the
 *   IO to complete. Callers must hold the sb s_umount semaphore for
 *   reading, to avoid having the super disappear before we are done.
 */
/**
 * bdi_sync_writeback - 启动并等待写回
 * @bdi: 要写回的后备设备
 * @sb: 从这个 super_block 写入 inode
 *
 * 描述:
 *   这个函数执行 WB_SYNC_ALL 数据完整性写回并等待 I/O 完成。
 *   调用者必须持有 sb s_umount 信号量读锁，以避免在操作完成前超级块消失。
 */
// 对指定的后备设备进行数据完整性写回，并等待I/O操作完成。
static void bdi_sync_writeback(struct backing_dev_info *bdi,
			       struct super_block *sb)
{
	struct wb_writeback_args args = {
		.sb		= sb,	// 设置写回操作的 super_block
		// WB_SYNC_ALL 表示当遇到锁住的 inode 时，它必须等待该 inode 解锁，而不能跳过。WB_SYNC_NONE 表示跳过被锁住的 inode；	
		.sync_mode	= WB_SYNC_ALL,	// 设置同步模式为完全同步
		.nr_pages	= LONG_MAX,	// 不限制页面数
		.range_cyclic	= 0,	// 非循环模式
	};
	struct bdi_work work;

	// 初始化工作项，并将其标记为栈上分配
	bdi_work_init(&work, &args);
	work.state |= WS_ONSTACK;

	// 将工作项加入队列
	bdi_queue_work(bdi, &work);
	// 等待工作项完成
	bdi_wait_on_work_clear(&work);
}

/**
 * bdi_start_writeback - start writeback
 * @bdi: the backing device to write from
 * @sb: write inodes from this super_block
 * @nr_pages: the number of pages to write
 *
 * Description:
 *   This does WB_SYNC_NONE opportunistic writeback. The IO is only
 *   started when this function returns, we make no guarentees on
 *   completion. Caller need not hold sb s_umount semaphore.
 *
 */
/**
 * bdi_start_writeback - 启动写回
 * @bdi: 从中写回的后备设备
 * @sb: 从这个 super_block 写入 inode
 * @nr_pages: 要写入的页面数
 *
 * 描述:
 *   这是一个 WB_SYNC_NONE 机会性写回。当此函数返回时，IO 才会开始，
 *   我们不保证完成。调用者无需持有 sb s_umount 信号量。
 */
/*
 * 定义了一个函数 bdi_start_writeback，用于启动对一个后备设备中 super_block 的非同步写回操作。
 * 该函数设置了写回任务的参数，如所操作的 super_block、写回模式（非同步）、涉及的页面数和是否循环写回。
 * 当传入的页面数为零时，函数将其视为后台写回的特殊情况，设置页面数为最大值并标记为后台写回。
 * 最后，这个写回任务会通过 bdi_alloc_queue_work 函数进行分配和排队，启动实际的 IO 操作。
 * 此操作不保证完成时机，且调用者无需持有任何信号量。
 */
void bdi_start_writeback(struct backing_dev_info *bdi, struct super_block *sb,
			 long nr_pages)
{
	struct wb_writeback_args args = {
		.sb		= sb,	// 设置要写回的 super_block
		.sync_mode	= WB_SYNC_NONE,	// 设置写回模式为非同步
		.nr_pages	= nr_pages,	// 设置此次写回涉及的页面数
		.range_cyclic	= 1,	// 启用循环写回
	};

	/*
	 * We treat @nr_pages=0 as the special case to do background writeback,
	 * ie. to sync pages until the background dirty threshold is reached.
	 */
	/*
	 * 我们将 @nr_pages=0 视为特殊情况，用于后台写回，
	 * 即同步页面直到达到后台脏页面阈值。
	 */
	if (!nr_pages) {
		args.nr_pages = LONG_MAX;	// 设置页面数为最大值
		args.for_background = 1;	// 标记为后台写回
	}

	// 分配并排队写回工作
	bdi_alloc_queue_work(bdi, &args);
}

/*
 * Redirty an inode: set its when-it-was dirtied timestamp and move it to the
 * furthest end of its superblock's dirty-inode list.
 *
 * Before stamping the inode's ->dirtied_when, we check to see whether it is
 * already the most-recently-dirtied inode on the b_dirty list.  If that is
 * the case then the inode must have been redirtied while it was being written
 * out and we don't reset its dirtied_when.
 */
/*
 * 将inode重新标记为脏：设置其脏时间戳，并将其移动到其超级块的脏inode列表的最末端。
 *
 * 在设置inode的->dirtied_when时间戳之前，我们检查它是否已经是b_dirty列表上最近被标记脏的inode。
 * 如果是这种情况，则表示inode在写出过程中被重新标记脏了，我们就不重置其dirtied_when。
 */
/*
 * 定义了一个函数 redirty_tail，用于将 inode 重新标记为脏并更新其在超级块的脏 inode 列表中的位置。
 * 首先，它获取与 inode 相关联的后备设备的写回控制结构。如果脏 inode 列表不为空，它会查找列表中最近
 * 一次被标记为脏的 inode，并比较当前 inode 的脏时间戳。如果当前 inode 的脏时间戳早于列表中的 inode，
 * 则将当前 inode 的脏时间戳更新为当前时间（jiffies）。
 */
// 重新把inode移动到wb->b_dirty链表，并可能会再次更新inode的脏时间
static void redirty_tail(struct inode *inode)
{
	// 获取inode对应的后备设备的写回控制结构
	struct bdi_writeback *wb = &inode_to_bdi(inode)->wb;

	if (!list_empty(&wb->b_dirty)) {	// 如果脏inode列表不为空
		struct inode *tail;

		// 获取列表中最新的inode
		tail = list_entry(wb->b_dirty.next, struct inode, i_list);	// 取出wb->b_dirty链表上的第一个脏inode
		// 如果当前inode的脏时间早于列表中的inode
		/*
		 * 如果inode的脏时间比wb->b_dirty链表上的第一个脏inode的脏时间还小还
     * 老，则把inode的脏时间更新为当前时间
		 */
		if (time_before(inode->dirtied_when, tail->dirtied_when))
			// 更新当前inode的脏时间为当前时间
			inode->dirtied_when = jiffies;
	}
	// 把inode移动到wb->b_dirty链表头，wb->b_dirty链表头的inode脏时间肯定是最新的，最大的
	list_move(&inode->i_list, &wb->b_dirty);
}

/*
 * requeue inode for re-scanning after bdi->b_io list is exhausted.
 */
/*
 * 在bdi->b_io列表用尽后，重新排队inode以重新扫描。
 */
/*
 * 定义了一个函数 requeue_io，用于在 bdi->b_io 列表用尽后，将 inode 重新排队以进行重新扫描。
 * 函数首先获取 inode 对应的后备设备的写回控制结构 wb。然后，它将 inode 从当前位置移动到该后备设备的 
 * b_more_io 列表中，这个列表用于存放需要进一步 IO 处理的 inode。这样做可以确保 inode 在所有当前 IO 
 * 操作处理完毕后，能够被重新考虑和处理。
 */
static void requeue_io(struct inode *inode)
{
	// 获取inode对应的后备设备的写回控制结构
	struct bdi_writeback *wb = &inode_to_bdi(inode)->wb;

	// 将inode移动到更多IO处理列表
	list_move(&inode->i_list, &wb->b_more_io);
}

/*
 * 定义了一个函数 inode_sync_complete，用于在 inode 同步操作完成后执行必要的清理和唤醒操作。
 * 函数首先设置一个内存屏障（smp_mb()），以确保之前对 inode 的所有写操作都在继续之前完成，
 * 从而防止因编译器或处理器优化导致的指令重排序。接着，该函数通过 wake_up_bit 唤醒所有在 
 * inode 的 i_state 字段中等待 __I_SYNC 位被清除的进程。
 */
static void inode_sync_complete(struct inode *inode)
{
	/*
	 * Prevent speculative execution through spin_unlock(&inode_lock);
	 */
	/*
	 * 通过spin_unlock(&inode_lock)防止推测性执行。
	 */
	smp_mb();	// 内存屏障，确保之前的写操作完成
	// 唤醒等待I_SYNC的进程
	wake_up_bit(&inode->i_state, __I_SYNC);
}

/*
 * 定义了一个函数 inode_dirtied_after，用于判断 inode 的脏时间是否晚于给定的时间 t。
 * 首先，使用 time_after 函数检查 inode->dirtied_when 是否晚于 t。对于非64位系统，
 * 还有一个额外的检查：如果 dirtied_when 持续被更新，可能会出现看似在未来但实际在遥远过去的情况，
 * 这种“环绕”可能阻碍 bdi 的整体写回过程。因此，使用 time_before_eq 函数确保 dirtied_when 
 * 不晚于当前时间 jiffies，避免时间环绕问题。
 */
static bool inode_dirtied_after(struct inode *inode, unsigned long t)
{
	// 检查inode的脏时间是否晚于指定时间t
	bool ret = time_after(inode->dirtied_when, t);
#ifndef CONFIG_64BIT
	/*
	 * For inodes being constantly redirtied, dirtied_when can get stuck.
	 * It _appears_ to be in the future, but is actually in distant past.
	 * This test is necessary to prevent such wrapped-around relative times
	 * from permanently stopping the whole bdi writeback.
	 */
	/*
	 * 对于持续被标记脏的inodes，dirtied_when可能会卡住。
	 * 它_看起来_是在未来，但实际上是在遥远的过去。
	 * 这个测试是必要的，以防止这种相对时间的环绕永久停止整个bdi的写回。
	 */
	ret = ret && time_before_eq(inode->dirtied_when, jiffies);
#endif
	return ret;
}

/*
 * Move expired dirty inodes from @delaying_queue to @dispatch_queue.
 */
/*
 * 从@delaying_queue移动已到期的脏inode到@dispatch_queue。
 */
/*
 * 定义了一个函数 move_expired_inodes，功能是将过期的脏 inode 从 delaying_queue 移动到 dispatch_queue。
 * 此操作首先遍历 delaying_queue，检查每个 inode 是否过期（通过比较它们的 dirtied_when 时间戳）。
 * 如果 inode 过期，且存在跨越不同超级块的 inode，将设置排序标志 do_sb_sort。过期的 inode 会被移到一个临时列表 
 * tmp 中。根据 do_sb_sort 的值，如果不需要跨超级块排序，则所有 inode 会直接添加到 dispatch_queue；
 * 如果需要排序，则会将同一个超级块的 inode 聚集在一起后再移动到 dispatch_queue。这样做可以优化写回操作的效率，
 * 确保同一超级块的 inode 在一起处理。
 */
static void move_expired_inodes(struct list_head *delaying_queue,
			       struct list_head *dispatch_queue,
				unsigned long *older_than_this)
{
	LIST_HEAD(tmp);	// 初始化一个临时链表头
	// 遍历使用的节点指针
	struct list_head *pos, *node;
	// 跟踪当前inode的超级块
	struct super_block *sb = NULL;
	// inode指针
	struct inode *inode;
	// 标记是否需要根据超级块对inode进行排序
	int do_sb_sort = 0;

	// 遍历@delaying_queue
	while (!list_empty(delaying_queue)) {
		// 获取最旧的inode
		inode = list_entry(delaying_queue->prev, struct inode, i_list);
		// 如果inode更新时间比older_than_this新，则停止
		if (older_than_this &&
		    inode_dirtied_after(inode, *older_than_this))
			break;
		// 如果跨超级块，设置排序标志
		if (sb && sb != inode->i_sb)
			do_sb_sort = 1;
		// 更新当前inode的超级块
		sb = inode->i_sb;
		// 将inode移动到临时列表
		list_move(&inode->i_list, &tmp);
	}

	/* just one sb in list, splice to dispatch_queue and we're done */
	// 如果列表中只有一个超级块的inode，直接接到@dispatch_queue
	if (!do_sb_sort) {
		list_splice(&tmp, dispatch_queue);
		return;
	}

	/* Move inodes from one superblock together */
	// 否则，确保同一个超级块的inode在一起
	while (!list_empty(&tmp)) {	// 循环处理临时列表
	// 获取tmp列表中的最后一个inode
		inode = list_entry(tmp.prev, struct inode, i_list);
		sb = inode->i_sb;	// 获取其超级块
		// 遍历临时列表
		list_for_each_prev_safe(pos, node, &tmp) {
			// 获取当前遍历的inode
			inode = list_entry(pos, struct inode, i_list);
			// 如果inode属于当前处理的超级块
			if (inode->i_sb == sb)
				// 将其移动到dispatch_queue
				list_move(&inode->i_list, dispatch_queue);
		}
	}
}

/*
 * Queue all expired dirty inodes for io, eldest first.
 */
/*
 * 对所有过期的脏inode进行排队以进行IO操作，从最老的inode开始。
 */
/*
 * 用于将所有过期的脏 inode 排队进行 IO 操作。该函数首先使用 list_splice_init 函数将 b_more_io 列表
 * （其中包含需要更多 IO 处理的 inode）初始化并移动到 b_io 列表的末尾。这样做是为了确保先处理已经等待更久的 inode。
 * 接着，通过调用 move_expired_inodes 函数，将 b_dirty 列表中所有过期的脏 inode
 * （根据 older_than_this 时间戳判断过期）移动到 b_io 列表，准备进行 IO 操作。
 */
static void queue_io(struct bdi_writeback *wb, unsigned long *older_than_this)
{
	// 初始化并移动b_more_io列表到b_io列表末尾
	list_splice_init(&wb->b_more_io, wb->b_io.prev);
	// 移动所有符合条件的过期脏inode到IO队列
	move_expired_inodes(&wb->b_dirty, &wb->b_io, older_than_this);
}

/*
 * 写回一个inode。如果inode有指定的write_inode操作且inode不是坏的，调用其super_block的write_inode方法。
 */
static int write_inode(struct inode *inode, struct writeback_control *wbc)
{
	// 检查是否存在write_inode操作且inode不是坏的
	if (inode->i_sb->s_op->write_inode && !is_bad_inode(inode))
		// 调用write_inode操作
		return inode->i_sb->s_op->write_inode(inode, wbc);
	return 0;	// 如果没有write_inode操作或inode是坏的，返回0
}

/*
 * Wait for writeback on an inode to complete.
 */
/*
 * 等待inode上的写回操作完成。
 */
/*
 * 定义了一个函数 inode_wait_for_writeback，用于在一个 inode 上等待写回操作完成。首先，它使用 
 * DEFINE_WAIT_BIT 宏定义了一个等待队列元素 wq，关联到 inode 的 I_SYNC 状态位。然后获取这个状态位对应的
 * 等待队列头 wqh。接下来，代码进入一个循环，其中释放了 inode_lock（以允许其他操作可以访问或修改 inode），
 * 然后在等待队列上等待 I_SYNC 状态位被清除（即写回完成）。等待使用的是 TASK_UNINTERRUPTIBLE，意味着等待是
 * 不可中断的。一旦 __wait_on_bit 返回，代码重新获取 inode_lock 并检查 I_SYNC 状态位。如果该位仍然设置，
 * 循环继续，反之则完成等待。
 */
static void inode_wait_for_writeback(struct inode *inode)
{
	// 定义一个等待队列元素wq，关联到inode的I_SYNC状态
	DEFINE_WAIT_BIT(wq, &inode->i_state, __I_SYNC);
	wait_queue_head_t *wqh;
	
	// 获取与I_SYNC状态位相关的等待队列头
	wqh = bit_waitqueue(&inode->i_state, __I_SYNC);
	do {
		spin_unlock(&inode_lock);	// 解锁inode_lock
		// 在等待队列上等待，直到inode的I_SYNC状态位被清除
		__wait_on_bit(wqh, &wq, inode_wait, TASK_UNINTERRUPTIBLE);
		// 重新加锁inode_lock
		spin_lock(&inode_lock);
		// 检查I_SYNC位，如果仍然设置，则继续等待
	} while (inode->i_state & I_SYNC);
}

/*
 * Write out an inode's dirty pages.  Called under inode_lock.  Either the
 * caller has ref on the inode (either via __iget or via syscall against an fd)
 * or the inode has I_WILL_FREE set (via generic_forget_inode)
 *
 * If `wait' is set, wait on the writeout.
 *
 * The whole writeout design is quite complex and fragile.  We want to avoid
 * starvation of particular inodes when others are being redirtied, prevent
 * livelocks, etc.
 *
 * Called under inode_lock.
 */
/*
 * writeback_single_inode - 写回一个inode的脏页
 * @inode: 要写回的inode
 * @wbc: 写回控制结构
 *
 * 在inode_lock的保护下调用。调用者持有对inode的引用
 * （通过__iget或通过对文件描述符的系统调用），或者inode设置了I_WILL_FREE标志
 * （通过generic_forget_inode）。
 *
 * 如果`wait`被设置，等待写回完成。
 *
 * 整个写回设计非常复杂和脆弱。我们希望在其他inode被重新弄脏时，避免特定inode的饥饿，
 * 防止活锁等问题。
 *
 * 在inode_lock保护下调用。
 */
/*
 * 代码详细描述了在保持 inode_lock 的情况下对单个 inode 进行写回操作的过程，包括在写回前后的状态检查和处理，
 * 确保数据一致性和写回操作的有效执行。同时，它也处理了同步和异步写回的逻辑差异，确保根据调用者的要求正确地处理
 * 写回操作。
 */
static int
writeback_single_inode(struct inode *inode, struct writeback_control *wbc)
{
	// 获取inode的映射
	struct address_space *mapping = inode->i_mapping;
	unsigned dirty;	// 脏页标记
	int ret;		// 返回值

	// 如果没有引用且不是释放状态，则警告
	if (!atomic_read(&inode->i_count))
		WARN_ON(!(inode->i_state & (I_WILL_FREE|I_FREEING)));
	else
		// 如果有引用但设置了将要释放，发出警告
		WARN_ON(inode->i_state & I_WILL_FREE);

	// 如果inode已被锁定用于写回
	if (inode->i_state & I_SYNC) {
		/*
		 * If this inode is locked for writeback and we are not doing
		 * writeback-for-data-integrity, move it to b_more_io so that
		 * writeback can proceed with the other inodes on s_io.
		 *
		 * We'll have another go at writing back this inode when we
		 * completed a full scan of b_io.
		 */
		/*
     * 如果这个inode已经因为写回而被锁定，并且我们没有进行数据完整性的写回，
     * 把它移动到b_more_io，以便可以继续对s_io上的其他inode进行写回。
     *
     * 当我们完成对b_io的完整扫描后，我们将再次尝试对这个inode进行写回。
     */
		// WB_SYNC_ALL 表示当遇到锁住的 inode 时，它必须等待该 inode 解锁，而不能跳过。WB_SYNC_NONE 表示跳过被锁住的 inode；
		if (wbc->sync_mode != WB_SYNC_ALL) {	// 如果不是为了数据完整性的同步写回
			// 重新排队此inode以后处理
			requeue_io(inode);
			return 0;	// 返回成功
		}

		/*
		 * It's a data-integrity sync.  We must wait.
		 */
		/*
     * 这是一个数据完整性同步。我们必须等待。
     */
		// 等待inode写回完成
		inode_wait_for_writeback(inode);
	}

	// 确保此时inode未被锁定
	BUG_ON(inode->i_state & I_SYNC);

	/* Set I_SYNC, reset I_DIRTY */
	/* 设置I_SYNC标志，重置I_DIRTY */
	dirty = inode->i_state & I_DIRTY;
	inode->i_state |= I_SYNC;
	inode->i_state &= ~I_DIRTY;

	// 解锁inode
	spin_unlock(&inode_lock);

	// 执行页面写回
	ret = do_writepages(mapping, wbc);

	/*
	 * Make sure to wait on the data before writing out the metadata.
	 * This is important for filesystems that modify metadata on data
	 * I/O completion.
	 */
	/*
	 * 在写出元数据之前，确保等待数据的写回。
	 * 这对于在数据I/O完成时修改元数据的文件系统来说很重要。
	 */
	// 如果是同步模式
	// WB_SYNC_ALL 表示当遇到锁住的 inode 时，它必须等待该 inode 解锁，而不能跳过。WB_SYNC_NONE 表示跳过被锁住的 inode；
	if (wbc->sync_mode == WB_SYNC_ALL) {
		// 等待文件数据写回完成
		int err = filemap_fdatawait(mapping);
		if (ret == 0)
			ret = err;
	}

	/* Don't write the inode if only I_DIRTY_PAGES was set */
	/* 如果只设置了I_DIRTY_PAGES，则不写回inode */
	// 如果设置了I_DIRTY_SYNC或I_DIRTY_DATASYNC，则写回inode
	if (dirty & (I_DIRTY_SYNC | I_DIRTY_DATASYNC)) {
		int err = write_inode(inode, wbc);
		if (ret == 0)
			ret = err;
	}

	// 重新加锁
	spin_lock(&inode_lock);
	// 清除I_SYNC标志
	inode->i_state &= ~I_SYNC;
	if (!(inode->i_state & (I_FREEING | I_CLEAR))) {
		if ((inode->i_state & I_DIRTY_PAGES) && wbc->for_kupdate) {
			// 如果有更多页被弄脏
			/*
			 * More pages get dirtied by a fast dirtier.
			 */
			/*
			 * 更多页面被快速弄脏者弄脏。
			 */
			goto select_queue;
		} else if (inode->i_state & I_DIRTY) {
			// 如果inode被重新弄脏
			/*
			 * At least XFS will redirty the inode during the
			 * writeback (delalloc) and on io completion (isize).
			 */
			/*
			 * 至少在XFS文件系统中，inode在写回（延迟分配）和I/O完成（文件大小改变）时会被重新弄脏。
			 */
			redirty_tail(inode);
		} else if (mapping_tagged(mapping, PAGECACHE_TAG_DIRTY)) {
			/*
			 * We didn't write back all the pages.  nfs_writepages()
			 * sometimes bales out without doing anything. Redirty
			 * the inode; Move it from b_io onto b_more_io/b_dirty.
			 */
			/*
			 * 我们没有写回所有页面。nfs_writepages()有时候会退出而不进行任何操作。
			 * 重新标记inode脏；把它从b_io移动到b_more_io/b_dirty。
			 */
			/*
			 * akpm: if the caller was the kupdate function we put
			 * this inode at the head of b_dirty so it gets first
			 * consideration.  Otherwise, move it to the tail, for
			 * the reasons described there.  I'm not really sure
			 * how much sense this makes.  Presumably I had a good
			 * reasons for doing it this way, and I'd rather not
			 * muck with it at present.
			 */
			/*
			 * 如果调用者是kupdate函数，我们把这个inode放到b_dirty的头部，使它得到优先考虑。
			 * 否则，移动它到尾部，原因已在那里描述。我不太确定这样做是否有意义。
			 * 大概我有一个好的理由这样做，目前我宁愿不去修改它。
			 */
			// 如果没有写回所有页面
			if (wbc->for_kupdate) {
				/*
				 * For the kupdate function we move the inode
				 * to b_more_io so it will get more writeout as
				 * soon as the queue becomes uncongested.
				 */
				/*
				 * 对于kupdate函数，我们将inode移动到b_more_io，以便在队列不拥堵时可以获得更多的写出操作。
				 */
				inode->i_state |= I_DIRTY_PAGES;
select_queue:
				// 如果写回限额已用完，重新排队此inode
				if (wbc->nr_to_write <= 0) {
					/*
					 * slice used up: queue for next turn
					 */
					/*
					 * 用量已用尽：为下一轮排队
					 */
					requeue_io(inode);
				} else {
					/*
					 * somehow blocked: retry later
					 */
					/*
					 * 以某种方式被阻塞：稍后重试
					 */
					// 否则，稍后重试
					redirty_tail(inode);
				}
			} else {
				// 否则，完全重新弄脏inode
				/*
				 * Otherwise fully redirty the inode so that
				 * other inodes on this superblock will get some
				 * writeout.  Otherwise heavy writing to one
				 * file would indefinitely suspend writeout of
				 * all the other files.
				 */
				/*
				 * 否则，完全重新弄脏inode，以便这个超级块上的其他inode也能得到一些写出。
				 * 否则，一个文件的大量写操作会无限期地暂停其他文件的写出。
				 */
				inode->i_state |= I_DIRTY_PAGES;
				redirty_tail(inode);
			}
		} else if (atomic_read(&inode->i_count)) {
			/*
			 * The inode is clean, inuse
			 */
			/*
			 * inode是干净的，正在使用中
			 */
			// 如果inode干净且正在使用中
			list_move(&inode->i_list, &inode_in_use);
		} else {
			/*
			 * The inode is clean, unused
			 */
			/*
			 * inode是干净的，未使用
			 */
			// 如果inode干净且未使用
			list_move(&inode->i_list, &inode_unused);
		}
	}
	inode_sync_complete(inode);	// 完成inode同步
	return ret;	// 返回结果
}

/*
 * 释放针对超级块的写回锁定并减少其引用计数。
 * 该函数用于在完成对超级块的写回操作后，释放之前加上的读锁（up_read），并通过调用 put_super 函数减少对该超级块的引用计数。
 */
static void unpin_sb_for_writeback(struct super_block *sb)
{
	up_read(&sb->s_umount);	// 释放读锁
	put_super(sb);	// 减少超级块的引用计数
}

/*
 * 枚举类型：定义超级块的锁定状态
 */
enum sb_pin_state {
	SB_PINNED,      // 超级块已锁定
	SB_NOT_PINNED,  // 超级块未锁定
	SB_PIN_FAILED   // 超级块锁定失败
};

/*
 * For WB_SYNC_NONE writeback, the caller does not have the sb pinned
 * before calling writeback. So make sure that we do pin it, so it doesn't
 * go away while we are writing inodes from it.
 */
/*
 * 对于非同步（WB_SYNC_NONE）写回，调用者在调用写回之前没有固定超级块。
 * 因此确保我们确实固定了它，这样在我们从中写入inode时它不会消失。
 */
/*
 * 定义了一个函数 pin_sb_for_writeback，用于固定一个超级块，以确保在执行写回操作期间，
 * 超级块不会被卸载或释放。函数的行为取决于写回控制结构 wbc 中指定的同步模式
 */
static enum sb_pin_state pin_sb_for_writeback(struct writeback_control *wbc,
					      struct super_block *sb)
{
	/*
	 * Caller must already hold the ref for this
	 */
	/*
   * 调用者必须已经持有引用
   */
	if (wbc->sync_mode == WB_SYNC_ALL) {
		WARN_ON(!rwsem_is_locked(&sb->s_umount));
		return SB_NOT_PINNED;
	}
	spin_lock(&sb_lock);
	sb->s_count++;	// 增加超级块的引用计数
	if (down_read_trylock(&sb->s_umount)) {	// 尝试获取读锁
		if (sb->s_root) {	// 如果超级块已经挂载
			spin_unlock(&sb_lock);
			return SB_PINNED;	// 成功固定
		}
		/*
		 * umounted, drop rwsem again and fall through to failure
		 */
		/*
     * 已卸载，再次释放读锁，并处理失败情况
     */
		up_read(&sb->s_umount);
	}
	sb->s_count--;	// 引用计数减少
	spin_unlock(&sb_lock);
	return SB_PIN_FAILED;	// 返回固定失败
}

/*
 * Write a portion of b_io inodes which belong to @sb.
 * If @wbc->sb != NULL, then find and write all such
 * inodes. Otherwise write only ones which go sequentially
 * in reverse order.
 * Return 1, if the caller writeback routine should be
 * interrupted. Otherwise return 0.
 */
/*
 * writeback_sb_inodes - 写回属于指定超级块的一部分b_io inodes
 * @sb: 超级块
 * @wb: 写回设备的信息
 * @wbc: 写回控制结构
 *
 * 如果@wbc->sb不为NULL，则查找并写回所有对应的inodes。
 * 否则只写回那些在逆序中顺序连续的inodes。
 * 如果调用者的写回例程应该被中断，返回1。否则返回0。
 */
// 定义了函数 writeback_sb_inodes，其目的是写回属于特定超级块 sb 的 b_io 链表中的 inode。该函数在写回控制结构 wbc 指示的范围内进行操作
static int writeback_sb_inodes(struct super_block *sb,
			       struct bdi_writeback *wb,
			       struct writeback_control *wbc)
{
	// 当b_io列表不为空时循环处理
	while (!list_empty(&wb->b_io)) {
		// 跳过的页数
		long pages_skipped;
		// 获取列表中最后一个inode
		struct inode *inode = list_entry(wb->b_io.prev,
						 struct inode, i_list);
		if (wbc->sb && sb != inode->i_sb) {
			/* super block given and doesn't
			   match, skip this inode */
			/* 如果指定了超级块且当前inode的超级块不匹配，则跳过此inode */
			redirty_tail(inode);
			continue;
		}
		if (sb != inode->i_sb)
			/* finish with this superblock */
			/* 完成此超级块的处理 */
			return 0;
		if (inode->i_state & (I_NEW | I_WILL_FREE)) {
			// 如果inode处于新建或将被释放的状态，则重新排队
			requeue_io(inode);
			continue;
		}
		/*
		 * Was this inode dirtied after sync_sb_inodes was called?
		 * This keeps sync from extra jobs and livelock.
		 */
		/*
     * 检查此inode是否在sync_sb_inodes被调用后被弄脏
     * 这避免了sync操作造成额外的工作和活锁。
     */
		if (inode_dirtied_after(inode, wbc->wb_start))
			return 1;

		// 检查inode状态的合理性
		BUG_ON(inode->i_state & (I_FREEING | I_CLEAR));	// 如果inode处于释放或清除状态，则触发BUG
		__iget(inode);	// 增加inode的引用计数
		pages_skipped = wbc->pages_skipped;	// 记录之前跳过的页数
		// 写回单个inode
		writeback_single_inode(inode, wbc);
		if (wbc->pages_skipped != pages_skipped) {
			/*
			 * writeback is not making progress due to locked
			 * buffers.  Skip this inode for now.
			 */
			/*
       * 如果写回没有取得进展，可能是因为缓冲区锁定。现在跳过此inode。
       */
			redirty_tail(inode);	// 将inode重新标记为脏，并放回脏列表的尾部
		}
		spin_unlock(&inode_lock);	// 解锁
		iput(inode);	// 减少inode的引用计数
		cond_resched();	// 条件调度，让出CPU
		spin_lock(&inode_lock);	// 加锁
		if (wbc->nr_to_write <= 0) {
			wbc->more_io = 1;	// 如果写回配额已用完，标记需要更多IO处理
			return 1;	// 返回1，提示调用者中断写回例程
		}
		if (!list_empty(&wb->b_more_io))
			wbc->more_io = 1;	// 如果还有更多等待IO处理的inode，标记需要更多IO处理
	}
	/* b_io is empty */
	// b_io列表为空
	return 1;	// 返回1，表示所有待处理的inode都已完成，可以中断写回操作
}

/*
 * writeback_inodes_wb - 处理写回操作的核心函数
 * @wb: 写回控制块，包含相关inode链表等信息
 * @wbc: 写回控制结构，包含写回操作的参数和状态
 *
 * 这个函数执行具体的写回操作，处理wb中的inode列表，根据条件选择合适的inode进行写回。
 * 这个过程包括对超级块的操作锁定，以确保文件系统的一致性。
 */
/*
 * writeback_inodes_wb 函数的实现，主要负责处理写回设备 wb 中的 inode 列表。函数首先锁定 inode 列表，
 * 确保操作的线程安全。根据条件，如果不是定期更新或 b_io 列表为空，则将 IO 队列化。接着，它遍历 b_io 列表中的 inode，
 * 对每个 inode 的超级块进行锁定准备写回，如果锁定成功，则调用 writeback_sb_inodes 函数执行写回操作。
 * 如果写回过程中出现错误或写回设定的 inode 完成，则解锁超级块并退出循环。最后，函数解锁 inode 列表，
 * 保留任何未完成写回的 inode 在 b_io 中。
 */
static void writeback_inodes_wb(struct bdi_writeback *wb,
				struct writeback_control *wbc)
{
	int ret = 0;

	/* 避免活锁 */
	wbc->wb_start = jiffies; /* livelock avoidance */	/* 避免活锁的开始时间 */
	spin_lock(&inode_lock);	// 锁定inode列表，保证线程安全
	// 如果不是为了定期更新，或者列表为空，则队列化IO
	if (!wbc->for_kupdate || list_empty(&wb->b_io))
		queue_io(wb, wbc->older_than_this);

	// 处理所有已标记为IO的inode
	while (!list_empty(&wb->b_io)) {
		// 获取inode列表中最后一个元素
		struct inode *inode = list_entry(wb->b_io.prev,
						 struct inode, i_list);
		// 获取inode所在的超级块
		struct super_block *sb = inode->i_sb;
		enum sb_pin_state state;

		if (wbc->sb && sb != wbc->sb) {
			/* super block given and doesn't
			   match, skip this inode */
			/* 如果指定了超级块且当前inode的超级块不匹配，则跳过 */
			redirty_tail(inode);	// 将inode重新标记为dirty，放回列表末尾
			continue;
		}
		// 锁定超级块，准备写回
		state = pin_sb_for_writeback(wbc, sb);

		// 如果锁定失败，重新排队该inode
		if (state == SB_PIN_FAILED) {
			requeue_io(inode);
			continue;
		}
		// 写回超级块中的inode
		ret = writeback_sb_inodes(sb, wb, wbc);

		// 完成后解锁超级块
		if (state == SB_PINNED)
			unpin_sb_for_writeback(sb);
		// 如果写回发生错误，中断循环
		if (ret)
			break;
	}
	spin_unlock(&inode_lock);	// 解锁inode列表
	/* Leave any unwritten inodes on b_io */
	/* 保留任何未写完的inode在b_io中 */
}

/*
 * writeback_inodes_wbc - 执行文件系统的写回操作
 * @wbc: 控制写回行为的结构体
 *
 * 这个函数简单地调用 writeback_inodes_wb 来执行实际的写回操作，使用从 wbc 结构体提取的特定 bdi (后台设备信息)。
 */
/*
 * 定义了函数 writeback_inodes_wbc，其主要职责是负责文件系统的写回操作。函数首先从 writeback_control 结构体 wbc 
 * 中获取后台设备信息 (backing_dev_info)，然后使用这个信息调用另一个函数 writeback_inodes_wb 来执行实际的写回操作。
 */
void writeback_inodes_wbc(struct writeback_control *wbc)
{
	// 从wbc结构体中获取bdi
	struct backing_dev_info *bdi = wbc->bdi;

	// 使用获取的bdi调用 writeback_inodes_wb 函数执行写回
	writeback_inodes_wb(&bdi->wb, wbc);
}

/*
 * The maximum number of pages to writeout in a single bdi flush/kupdate
 * operation.  We do this so we don't hold I_SYNC against an inode for
 * enormous amounts of time, which would block a userspace task which has
 * been forced to throttle against that inode.  Also, the code reevaluates
 * the dirty each time it has written this many pages.
 */
/*
 * 在单次bdi刷新/定期更新操作中写出的最大页面数。
 * 我们这样做是为了避免在inode上持有I_SYNC过长时间，
 * 这会阻塞一个用户空间任务，该任务可能因为该inode而被迫限流。
 * 此外，代码在写了这么多页面后会重新评估脏页。
 */
#define MAX_WRITEBACK_PAGES     1024

/*
 * 检查当前系统脏页数量是否超过了后台写回阈值。
 */
static inline bool over_bground_thresh(void)
{
	unsigned long background_thresh, dirty_thresh;

	// 获取系统当前的脏页限制
	get_dirty_limits(&background_thresh, &dirty_thresh, NULL, NULL);	// 获取当前的后台和脏页阈值

	// 返回当前脏页数加上不稳定NFS脏页数是否达到或超过了后台写回阈值
	return (global_page_state(NR_FILE_DIRTY) +
		global_page_state(NR_UNSTABLE_NFS) >= background_thresh);
	// 通过 global_page_state 函数获取文件系统脏页 (NR_FILE_DIRTY) 和不稳定NFS脏页 (NR_UNSTABLE_NFS) 的数量
}

/*
 * Explicit flushing or periodic writeback of "old" data.
 *
 * Define "old": the first time one of an inode's pages is dirtied, we mark the
 * dirtying-time in the inode's address_space.  So this periodic writeback code
 * just walks the superblock inode list, writing back any inodes which are
 * older than a specific point in time.
 *
 * Try to run once per dirty_writeback_interval.  But if a writeback event
 * takes longer than a dirty_writeback_interval interval, then leave a
 * one-second gap.
 *
 * older_than_this takes precedence over nr_to_write.  So we'll only write back
 * all dirty pages if they are all attached to "old" mappings.
 */
/*
 * 显式刷新或定期回写“旧”数据。
 *
 * 定义“旧”：当一个inode的页面首次变脏时，我们会在inode的地址空间中标记脏时间。
 * 因此，这个定期回写代码仅遍历超级块的inode列表，回写那些比特定时间点更早的inode。
 *
 * 尝试每个dirty_writeback_interval执行一次。但如果一个回写事件
 * 超过了一个dirty_writeback_interval时间间隔，那么留下一秒的间隙。
 *
 * older_than_this优先于nr_to_write。所以我们只会回写所有附加到“旧”映射的脏页面。
 */
// 声明wb_writeback函数，接收后备设备和回写参数
static long wb_writeback(struct bdi_writeback *wb,
			 struct wb_writeback_args *args)
{
	struct writeback_control wbc = {
		.bdi			= wb->bdi,	// 后备设备信息
		.sb			= args->sb,		// 超级块
		.sync_mode		= args->sync_mode,	// 同步模式
		.older_than_this	= NULL,	// 设定一个旧数据的时间点
		.for_kupdate		= args->for_kupdate,	// 是否为kupdate风格的周期性回写
		.for_background		= args->for_background,	// 是否为后台回写
		.range_cyclic		= args->range_cyclic,	// 是否循环范围
	};
	unsigned long oldest_jif;	// 用于存储最早的jiffies时间
	long wrote = 0;	// 记录写回的页数
	struct inode *inode;	// inode结构体指针


	if (wbc.for_kupdate) {	// 如果是为了周期性更新
		// 设置旧数据的时间界限
		// 设置older_than_this指向oldest_jif
		wbc.older_than_this = &oldest_jif;
		// 计算时间界限
		// 计算最早的有效jiffies时间
		oldest_jif = jiffies -
				msecs_to_jiffies(dirty_expire_interval * 10);
	}
	// 如果不是循环范围
	if (!wbc.range_cyclic) {
		wbc.range_start = 0;	// 设置回写范围的起始
		wbc.range_end = LLONG_MAX;	// 设置回写范围的结束
	}

	for (;;) {
		/*
		 * Stop writeback when nr_pages has been consumed
		 */
		/*
		 * 当nr_pages用尽时停止回写
		 */
		if (args->nr_pages <= 0)
			break;

		/*
		 * For background writeout, stop when we are below the
		 * background dirty threshold
		 */
		/*
		 * 对于后台回写，当我们低于背景脏阈值时停止
		 */
		if (args->for_background && !over_bground_thresh())
			break;

		wbc.more_io = 0;
		// 设置本次最大回写页数
		wbc.nr_to_write = MAX_WRITEBACK_PAGES;
		// 跳过的页数
		wbc.pages_skipped = 0;
		// 执行回写操作
		writeback_inodes_wb(wb, &wbc);
		// 计算剩余未回写页数
		args->nr_pages -= MAX_WRITEBACK_PAGES - wbc.nr_to_write;
		// 累计已回写页数
		wrote += MAX_WRITEBACK_PAGES - wbc.nr_to_write;

		/*
		 * If we consumed everything, see if we have more
		 */
		/*
		 * 如果消耗了全部页面，检查是否还有更多
		 */
		if (wbc.nr_to_write <= 0)
			continue;
		/*
		 * Didn't write everything and we don't have more IO, bail
		 */
		/*
		 * 没有写完所有页面且没有更多IO，退出循环
		 */
		if (!wbc.more_io)
			break;
		/*
		 * Did we write something? Try for more
		 */
		/*
		 * 如果写了一些页面，尝试写更多
		 */
		if (wbc.nr_to_write < MAX_WRITEBACK_PAGES)
			continue;
		/*
		 * Nothing written. Wait for some inode to
		 * become available for writeback. Otherwise
		 * we'll just busyloop.
		 */
		/*
		 * 没有写入任何内容。等待一些inode可用于回写，否则我们将只是忙等。
		 */
		spin_lock(&inode_lock);	// 锁定inode
		if (!list_empty(&wb->b_more_io))  {	// 如果还有更多inode需要IO处理
			// 获取待处理的inode
			inode = list_entry(wb->b_more_io.prev,
						struct inode, i_list);
			inode_wait_for_writeback(inode);	// 等待inode回写完成
		}
		spin_unlock(&inode_lock);	// 解锁inode
	}

	return wrote;	// 返回写回的总页数
}

/*
 * Return the next bdi_work struct that hasn't been processed by this
 * wb thread yet. ->seen is initially set for each thread that exists
 * for this device, when a thread first notices a piece of work it
 * clears its bit. Depending on writeback type, the thread will notify
 * completion on either receiving the work (WB_SYNC_NONE) or after
 * it is done (WB_SYNC_ALL).
 */
/*
 * 返回下一个尚未由此写回线程处理的bdi_work结构。每个存在于该设备的线程
 * 在首次注意到一个工作项时，->seen 最初被设置。线程首次发现工作项时会清除其位。
 * 根据写回类型，线程将在接收工作时（WB_SYNC_NONE）或完成后（WB_SYNC_ALL）通知完成。
 */
/*
 * 从给定的后台设备信息（backing_dev_info）中获取下一个未被当前写回线程处理的工作项（bdi_work）。通过使用RCU锁，
 * 确保在并发环境下安全地访问和修改工作列表。每个工作项的处理状态通过 seen 位标记管理，每个写回线程在处理工作项前会检查
 * 并清除对应的位。这种设计允许系统高效地分配并跟踪工作项的处理状态，以确保所有工作项都能被适当处理。
 */
static struct bdi_work *get_next_work_item(struct backing_dev_info *bdi,
					   struct bdi_writeback *wb)
{
	struct bdi_work *work, *ret = NULL;	// 定义工作指针和返回值

	rcu_read_lock();	// 读取锁定，使用RCU锁来保护遍历过程

	// 遍历bdi的工作列表
	list_for_each_entry_rcu(work, &bdi->work_list, list) {
		if (!test_bit(wb->nr, &work->seen))	// 检查当前线程是否已经看到这个工作项
			continue;	// 如果已看到，则跳过
		clear_bit(wb->nr, &work->seen);	// 清除seen位，标记该工作项已被当前线程处理

		ret = work;	// 设置返回的工作项
		break;	// 跳出循环
	}

	rcu_read_unlock();	// 解锁RCU读锁
	return ret;	// 返回下一个工作项
}

/*
 * 检查是否需要根据旧数据的刷新时间间隔进行写回。
 * @wb: 写回控制块，包含相关写回操作的信息。
 * 返回写回的页数，如果没有进行写回则返回0。
 */
/*
 * 实现了基于时间间隔的脏数据自动写回功能。它首先检查从上一次写回以来是否已经达到了设定的时间间隔
 * （dirty_writeback_interval * 10），如果未达到，则直接返回0，不执行写回。如果时间已到，
 * 它会计算当前系统中的脏页总数（包括文件系统脏页、不稳定的NFS脏页以及活跃的inode数），
 * 并根据这些脏页数调用写回函数 wb_writeback 进行数据写回，最后返回写回的页数。
 * 此过程有助于保证系统的数据一致性和性能，通过定期清理脏数据减少系统的I/O压力。
 */
static long wb_check_old_data_flush(struct bdi_writeback *wb)
{
	unsigned long expired;  // 用于计算过期时间
	long nr_pages;  // 需要写回的页数

	// 计算旧数据的刷新间隔
	expired = wb->last_old_flush +
			msecs_to_jiffies(dirty_writeback_interval * 10);
	// 如果当前时间未到过期时间，则不进行写回
	if (time_before(jiffies, expired))
		return 0;

	// 更新上一次旧数据的刷新时间
	wb->last_old_flush = jiffies;
	// 计算当前系统中脏页的总数，包括文件脏页和不稳定的NFS脏页，以及使用的inode数
	nr_pages = global_page_state(NR_FILE_DIRTY) +
			global_page_state(NR_UNSTABLE_NFS) +
			(inodes_stat.nr_inodes - inodes_stat.nr_unused);

	// 如果有需要写回的页
	if (nr_pages) {
		struct wb_writeback_args args = {
			.nr_pages       = nr_pages,  // 设置需要写回的页数
			.sync_mode      = WB_SYNC_NONE,  // 设置同步模式为非同步
			.for_kupdate    = 1,  // 设置为周期性更新的写回
			.range_cyclic   = 1,  // 设置为循环范围的写回
		};

		// 调用写回函数，执行写回操作
		return wb_writeback(wb, &args);
	}

	// 如果没有需要写回的页，返回0
	return 0;
}

/*
 * Retrieve work items and do the writeback they describe
 */
/*
 * 检索工作项并执行它们所描述的回写
 */
/*
 * wb_do_writeback - 检索工作项并执行它们所描述的回写
 * @wb: 写回控制块，包含相关的后备设备信息和工作列表
 * @force_wait: 是否强制等待回写完成
 *
 * 这个函数负责执行指定写回控制块中的所有工作项。它遍历工作列表，
 * 获取每个工作项，并根据其描述执行数据回写。如果force_wait为真，则
 * 强制所有操作等待直到数据完全写入磁盘。
 */
/*
 * 文件系统的核心写回函数，负责按照后备设备信息 (backing_dev_info) 列表中的工作项描述执行数据回写。
 * 根据每个工作项的同步模式，函数可能立即执行写回或等待直到数据完全写入磁盘。
 * 如果启用了 force_wait，则所有写回操作都将等待完成。此外，函数还负责执行周期性的旧数据回写，
 * 以维护系统的数据完整性和性能。
 */
long wb_do_writeback(struct bdi_writeback *wb, int force_wait)
{
	struct backing_dev_info *bdi = wb->bdi;	// 获取后备设备信息
	struct bdi_work *work;	// 工作项变量
	long wrote = 0;	// 已写入的数据量，初始化为0

	// 循环获取下一个工作项，直到没有工作项
	while ((work = get_next_work_item(bdi, wb)) != NULL) {
		// 获取工作项的参数
		struct wb_writeback_args args = work->args;

		/*
		 * Override sync mode, in case we must wait for completion
		 */
		/*
		 * 在必须等待完成的情况下，覆盖同步模式
		 */
		// 如果强制等待，则设置同步模式为全部同步
		if (force_wait)
			work->args.sync_mode = args.sync_mode = WB_SYNC_ALL;

		/*
		 * If this isn't a data integrity operation, just notify
		 * that we have seen this work and we are now starting it.
		 */
		/*
		 * 如果这不是数据完整性操作，仅通知我们已看到此工作并已开始执行它。
		 */
		// 如果同步模式为无同步，清除挂起的工作项
		if (args.sync_mode == WB_SYNC_NONE)
			wb_clear_pending(wb, work);

		// 执行回写并累加写入的数据量
		wrote += wb_writeback(wb, &args);

		/*
		 * This is a data integrity writeback, so only do the
		 * notification when we have completed the work.
		 */
		/*
		 * 这是一个数据完整性回写，因此只在完成工作后进行通知。
		 */
		// 如果同步模式为全部同步，清除挂起的工作项
		if (args.sync_mode == WB_SYNC_ALL)
			wb_clear_pending(wb, work);
	}

	/*
	 * Check for periodic writeback, kupdated() style
	 */
	/*
	 * 检查周期性回写，类似于kupdated()风格
	 */
	// 检查并处理旧数据的回写，累加写入的数据量
	wrote += wb_check_old_data_flush(wb);

	return wrote;
}

/*
 * Handle writeback of dirty data for the device backed by this bdi. Also
 * wakes up periodically and does kupdated style flushing.
 */
/*
 * 处理设备后备的脏数据回写。此外，定期唤醒并执行kupdated风格的刷新。
 * bdi_writeback_task - 负责执行特定后备设备（backing device）的脏数据回写任务。
 * 这个函数还会定期唤醒，执行类似于传统kupdated守护进程的刷新操作。
 */
/*
 * 这段代码是一个内核线程函数，用于管理一个后备设备的脏数据回写任务。它不断检查是否需要停止，如果不需要，
 * 则执行回写操作并计算已写入的页面数。如果在一定时间内没有新的写入，且闲置时间超过设定的最大值，线程将自动退出。
 * 同时，这个函数还定期通过设置定时器来进行睡眠，从而不会一直占用CPU资源，而是在指定的间隔后被唤醒继续执行任务。
 * 此外，它还会响应系统的冻结请求，使得线程能在系统需要时被正确地冻结。
 */
int bdi_writeback_task(struct bdi_writeback *wb)
{
	// 上一次活动的时间点，使用系统的jiffies计时
	unsigned long last_active = jiffies;
	// 初始化等待时间为最大无符号长整数
	unsigned long wait_jiffies = -1UL;
	long pages_written;	// 已写入的页面数

	// 如果内核线程不应该停止，则继续循环
	while (!kthread_should_stop()) {
		// 执行回写操作，并返回写入的页面数
		pages_written = wb_do_writeback(wb, 0);

		// 如果有页面写入，更新最后活动时间
		if (pages_written)
			last_active = jiffies;
		else if (wait_jiffies != -1UL) {
			unsigned long max_idle;	// 最大闲置时间

			/*
			 * Longest period of inactivity that we tolerate. If we
			 * see dirty data again later, the task will get
			 * recreated automatically.
			 */
			/*
			 * 我们容忍的最长不活跃期。如果我们稍后再次看到脏数据，任务将自动重新创建。
			 */
			// 计算最大闲置时间
			max_idle = max(5UL * 60 * HZ, wait_jiffies);
			// 如果当前时间超过了最后活动时间加最大闲置时间，退出循环
			if (time_after(jiffies, max_idle + last_active))
				break;
		}

		// 计算等待的jiffies数
		wait_jiffies = msecs_to_jiffies(dirty_writeback_interval * 10);
		// 设置可中断的超时等待
		schedule_timeout_interruptible(wait_jiffies);
		try_to_freeze();	// 尝试冻结任务
	}

	return 0;
}

/*
 * Schedule writeback for all backing devices. This does WB_SYNC_NONE
 * writeback, for integrity writeback see bdi_sync_writeback().
 */
/*
 * 为所有后备设备安排回写。这是执行WB_SYNC_NONE模式的回写，对于完整性回写，请查看bdi_sync_writeback()。
 * bdi_writeback_all - 为所有后备设备计划进行回写操作。此操作执行非同步回写模式，
 * 如果需要进行数据完整性保证的回写，请使用 bdi_sync_writeback() 函数。
 */
/*
 * 为所有的后备设备（如硬盘驱动器等）安排脏数据的回写操作。此函数特别用于执行非同步回写，即回写操作不会等待数据完全写入磁盘
 * 即可返回。这种模式适用于不需要立即保证数据完整性的场景。如果需要确保数据的完整性，应使用 bdi_sync_writeback() 函数。
 */
static void bdi_writeback_all(struct super_block *sb, long nr_pages)
{
	struct wb_writeback_args args = {
		.sb		= sb,	// 文件系统的超级块
		.nr_pages	= nr_pages,	// 指定回写的页面数
		.sync_mode	= WB_SYNC_NONE,	// 设置同步模式为不等待完成
	};
	struct backing_dev_info *bdi;	// 后备设备信息的变量

	rcu_read_lock();	// 开始RCU读锁保护

	// 遍历后备设备列表
	list_for_each_entry_rcu(bdi, &bdi_list, bdi_list) {
		if (!bdi_has_dirty_io(bdi))
			continue;	// 如果设备没有脏数据，则跳过

		// 为有脏数据的设备安排回写任务
		bdi_alloc_queue_work(bdi, &args);
	}

	rcu_read_unlock();	// 释放RCU读锁
}

/*
 * Start writeback of `nr_pages' pages.  If `nr_pages' is zero, write back
 * the whole world.
 */
/*
 * 启动`nr_pages'页面的回写。如果`nr_pages'为零，回写所有页面。
 * wakeup_flusher_threads - 启动指定数量页面的回写。如果指定的页面数为零，表示需要回写所有脏页面。
 */
/*
 * 根据提供的页面数来启动回写操作。如果传入的页面数为零，则系统将自动计算所有未稳定的NFS页面和文件系统的脏页面，
 * 然后对这些页面进行回写。这样的设计允许灵活地根据当前系统状态决定回写的范围，从而优化存储设备的I/O性能和系统
 * 的整体响应速度。
 */
void wakeup_flusher_threads(long nr_pages)
{
	// 如果页面数为0，则计算全局脏页面和不稳定的NFS页面之和
	if (nr_pages == 0)
		nr_pages = global_page_state(NR_FILE_DIRTY) +
				global_page_state(NR_UNSTABLE_NFS);
	// 调用bdi_writeback_all函数开始回写
	bdi_writeback_all(NULL, nr_pages);
}

/*
 * block_dump___mark_inode_dirty - 标记inode为脏，并在系统日志中记录相关信息。
 * @inode: 需要标记为脏的inode
 *
 * 如果inode的i_ino不为零或其超级块的s_id不是"bdev"，则获取inode的别名dentry，
 * 并打印关于inode变脏的调试信息到系统日志。这通常用于跟踪和调试文件系统活动。
 */
/*
 * 定义了一个函数 block_dump___mark_inode_dirty，该函数用于标记 inode 为脏并打印相关的调试信息。
 * 这通常在文件系统中某个文件（由 inode 表示）被修改时调用，以帮助开发者跟踪文件系统的状态变化。
 */
static noinline void block_dump___mark_inode_dirty(struct inode *inode)
{
	// 如果inode的索引节点号不为0或文件系统标识不是"bdev"
	if (inode->i_ino || strcmp(inode->i_sb->s_id, "bdev")) {
		struct dentry *dentry;  // 目录项变量
		const char *name = "?";  // 默认文件名

		// 找到与inode关联的dentry
		dentry = d_find_alias(inode);
		if (dentry) {
			spin_lock(&dentry->d_lock);  // 对dentry进行加锁
			name = (const char *) dentry->d_name.name;  // 获取文件名
		}
		// 打印inode变脏的信息到内核调试日志
		printk(KERN_DEBUG
					"%s(%d): dirtied inode %lu (%s) on %s\n",
					current->comm, task_pid_nr(current), inode->i_ino,
					name, inode->i_sb->s_id);
		if (dentry) {
			spin_unlock(&dentry->d_lock);  // 解锁
			dput(dentry);  // 释放dentry的引用
		}
	}
}

/**
 *	__mark_inode_dirty -	internal function
 *	@inode: inode to mark
 *	@flags: what kind of dirty (i.e. I_DIRTY_SYNC)
 *	Mark an inode as dirty. Callers should use mark_inode_dirty or
 *  	mark_inode_dirty_sync.
 *
 * Put the inode on the super block's dirty list.
 *
 * CAREFUL! We mark it dirty unconditionally, but move it onto the
 * dirty list only if it is hashed or if it refers to a blockdev.
 * If it was not hashed, it will never be added to the dirty list
 * even if it is later hashed, as it will have been marked dirty already.
 *
 * In short, make sure you hash any inodes _before_ you start marking
 * them dirty.
 *
 * This function *must* be atomic for the I_DIRTY_PAGES case -
 * set_page_dirty() is called under spinlock in several places.
 *
 * Note that for blockdevs, inode->dirtied_when represents the dirtying time of
 * the block-special inode (/dev/hda1) itself.  And the ->dirtied_when field of
 * the kernel-internal blockdev inode represents the dirtying time of the
 * blockdev's pages.  This is why for I_DIRTY_PAGES we always use
 * page->mapping->host, so the page-dirtying time is recorded in the internal
 * blockdev inode.
 */
/*
 * __mark_inode_dirty - 内部函数
 * @inode: 要标记的inode
 * @flags: 脏的类型（例如：I_DIRTY_SYNC）
 * 将一个inode标记为脏。调用者应使用mark_inode_dirty或mark_inode_dirty_sync。
 *
 * 将inode放置在超级块的脏列表上。
 *
 * 注意！我们无条件地标记它为脏，但只有当它被哈希或者它引用了一个块设备时才将其移动到脏列表上。
 * 如果它未被哈希，则即使稍后被哈希，它也永远不会被添加到脏列表上，因为它已经被标记为脏了。
 *
 * 简而言之，在你开始标记它们为脏之前，确保你已经哈希了任何inodes。
 *
 * 对于I_DIRTY_PAGES情况，这个函数必须是原子的——在几个地方set_page_dirty()是在自旋锁下被调用的。
 *
 * 注意对于块设备，inode->dirtied_when表示块特殊inode（如/dev/hda1）本身的脏时间。
 * 而内核内部块设备inode的->dirtied_when字段代表块设备页面的脏时间。
 * 这就是为什么对于I_DIRTY_PAGES我们总是使用page->mapping->host，这样页面脏时间就记录在内部块设备inode中。
 */
/*
 * 定义了一个核心函数，__mark_inode_dirty，用于将指定的 inode 标记为脏，并根据情况将其添加到相应的超级块的脏列表中。
 * 该函数在标记 inode 为脏时考虑了多种情况，确保了在多线程环境中对 inode 状态的正确管理和更新。
 * 同时，它也处理了特殊情况，如块设备的 inode 和未哈希的 inode，确保只有有效的和需要的 inode 被加入到脏列表中。
 */
void __mark_inode_dirty(struct inode *inode, int flags)
{
	struct super_block *sb = inode->i_sb;	// 获取inode所属的超级块

	/*
	 * Don't do this for I_DIRTY_PAGES - that doesn't actually
	 * dirty the inode itself
	 */
	 /*
   * 对I_DIRTY_PAGES不执行操作，因为这并不实际脏化inode本身
   */
	if (flags & (I_DIRTY_SYNC | I_DIRTY_DATASYNC)) {
		// 如果超级块有dirty_inode操作，则调用之
		if (sb->s_op->dirty_inode)
			sb->s_op->dirty_inode(inode);	// 调用超级块的dirty_inode操作
	}

	/*
	 * make sure that changes are seen by all cpus before we test i_state
	 * -- mikulas
	 */
	/*
   * 确保在我们测试i_state之前，所有的CPU都能看到更改
   */
	smp_mb();	// 执行内存屏障，确保内存操作的可见性

	/* avoid the locking if we can */
	/* 如果可以的话，避免加锁 */
	if ((inode->i_state & flags) == flags)
		return;	// 如果inode的状态已经包含了所有指定的标志，则直接返回

	if (unlikely(block_dump))
		block_dump___mark_inode_dirty(inode);	// 如果启用了块转储，则记录inode的脏信息

	spin_lock(&inode_lock);	// 加锁inode锁
	if ((inode->i_state & flags) != flags) {
		const int was_dirty = inode->i_state & I_DIRTY;	// 检查inode是否已经标记为脏

		inode->i_state |= flags;	// 更新inode状态，添加脏标志

		/*
		 * If the inode is being synced, just update its dirty state.
		 * The unlocker will place the inode on the appropriate
		 * superblock list, based upon its state.
		 */
		/*
     * 如果inode正在同步，只更新它的脏状态。
     * 解锁者将根据inode的状态将其放置在适当的超级块列表上。
     */
		if (inode->i_state & I_SYNC)
			goto out;	// 如果inode处于同步中，则跳过后续操作

		/*
		 * Only add valid (hashed) inodes to the superblock's
		 * dirty list.  Add blockdev inodes as well.
		 */
		/*
     * 只将有效（已哈希）的inodes添加到超级块的脏列表上。块设备的inodes也一样。
     */
		if (!S_ISBLK(inode->i_mode)) {	// 如果不是块设备inode
			if (hlist_unhashed(&inode->i_hash))
				goto out;	// 如果inode未哈希，则跳过
		}
		if (inode->i_state & (I_FREEING|I_CLEAR))
			goto out;	// 如果inode处于释放或清除状态，则跳过

		/*
		 * If the inode was already on b_dirty/b_io/b_more_io, don't
		 * reposition it (that would break b_dirty time-ordering).
		 */
		/*
     * 如果inode已经在b_dirty/b_io/b_more_io上，不要重新定位它（这将打乱b_dirty的时间顺序）。
     */
		if (!was_dirty) {	// 如果inode之前未标记为脏
			struct bdi_writeback *wb = &inode_to_bdi(inode)->wb;
			struct backing_dev_info *bdi = wb->bdi;

			// 检查后备设备信息（BDI）是否支持回写脏数据并且已注册
			if (bdi_cap_writeback_dirty(bdi) &&
			    !test_bit(BDI_registered, &bdi->state)) {
				WARN_ON(1);	// 如果BDI未注册，则发出警告
				printk(KERN_ERR "bdi-%s not registered\n",
								bdi->name);
			}

			inode->dirtied_when = jiffies;	// 记录脏时间
			list_move(&inode->i_list, &wb->b_dirty);	// 将inode移动到脏列表
		}
	}
out:
	spin_unlock(&inode_lock);	// 解锁inode锁
}
EXPORT_SYMBOL(__mark_inode_dirty);

/*
 * Write out a superblock's list of dirty inodes.  A wait will be performed
 * upon no inodes, all inodes or the final one, depending upon sync_mode.
 *
 * If older_than_this is non-NULL, then only write out inodes which
 * had their first dirtying at a time earlier than *older_than_this.
 *
 * If `bdi' is non-zero then we're being asked to writeback a specific queue.
 * This function assumes that the blockdev superblock's inodes are backed by
 * a variety of queues, so all inodes are searched.  For other superblocks,
 * assume that all inodes are backed by the same queue.
 *
 * The inodes to be written are parked on bdi->b_io.  They are moved back onto
 * bdi->b_dirty as they are selected for writing.  This way, none can be missed
 * on the writer throttling path, and we get decent balancing between many
 * throttled threads: we don't want them all piling up on inode_sync_wait.
 */
/*
 * 写出一个超级块的脏inode列表。根据sync_mode的设置，可能会在没有inode、所有inode或最后一个inode上执行等待。
 *
 * 如果older_than_this不为NULL，那么只写出那些在*older_than_this之前首次变脏的inode。
 *
 * 如果`bdi`非零，那么我们被要求回写一个特定的队列。
 * 这个函数假设块设备超级块的inode由多个队列支持，因此搜索所有inode。
 * 对于其他超级块，假设所有inode由同一个队列支持。
 *
 * 被写的inodes被停放在bdi->b_io上。它们在被选中写入时被移回bdi->b_dirty。
 * 这样，就不会在写入限制路径上遗漏任何inode，我们可以在许多受限的线程之间获得良好的平衡：
 * 我们不希望它们都堆积在inode_sync_wait上。
 */
/*
 * 这段代码主要是用来处理超级块中所有脏inode的写回操作。它在写回过程中确保了数据的完整性，即确保所有的脏数据都被正确地写入磁盘。
 * 通过锁的使用保证了操作的原子性，防止了在数据写回过程中的数据不一致问题。同时，这个过程也考虑到了性能，
 * 通过条件调度（cond_resched）允许其他进程有机会运行，避免了长时间占用CPU导致系统反应迟缓。
 */
// 其实就是一个函数先调用bdi_sync_writeback执行写操作，然后再调用该函数用来等待写操作完成
static void wait_sb_inodes(struct super_block *sb)
{
	struct inode *inode, *old_inode = NULL;

	/*
	 * We need to be protected against the filesystem going from
	 * r/o to r/w or vice versa.
	 */
	/*
	 * 我们需要防止文件系统从只读转换为读写或相反的情况。
	 */
	WARN_ON(!rwsem_is_locked(&sb->s_umount));	// 警告如果未锁定超级块的卸载锁

	spin_lock(&inode_lock);	// 锁定inode锁

	/*
	 * Data integrity sync. Must wait for all pages under writeback,
	 * because there may have been pages dirtied before our sync
	 * call, but which had writeout started before we write it out.
	 * In which case, the inode may not be on the dirty list, but
	 * we still have to wait for that writeout.
	 */
	/*
	 * 数据完整性同步。必须等待所有处于写回状态的页面，
	 * 因为可能有页面在我们的同步调用之前就已经变脏，
	 * 但写出开始在我们写之前。在这种情况下，inode可能不在脏列表上，
	 * 但我们仍然需要等待那次写出。
	 */
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		struct address_space *mapping;

		if (inode->i_state & (I_FREEING|I_CLEAR|I_WILL_FREE|I_NEW))
			continue;	// 跳过正在释放或清理的inode
		mapping = inode->i_mapping;
		if (mapping->nrpages == 0)
			continue;	// 如果没有脏页则跳过
		__iget(inode);	// 增加inode的引用计数
		spin_unlock(&inode_lock);	// 解锁inode锁
		/*
		 * We hold a reference to 'inode' so it couldn't have
		 * been removed from s_inodes list while we dropped the
		 * inode_lock.  We cannot iput the inode now as we can
		 * be holding the last reference and we cannot iput it
		 * under inode_lock. So we keep the reference and iput
		 * it later.
		 */
		/*
		 * 我们持有对'inode'的引用，所以它不可能在我们放开inode_lock时
		 * 从s_inodes列表中被移除。我们现在不能放回inode，因为我们可能持有最后一个引用，
		 * 我们不能在持有inode_lock的情况下执行iput。因此我们保留引用，并稍后执行iput。
		 */
		iput(old_inode);	// 释放先前的inode
		old_inode = inode;

		filemap_fdatawait(mapping);	// 等待文件映射中所有的写操作完成

		cond_resched();	// 条件调度，允许其他任务运行

		spin_lock(&inode_lock);	// 重新锁定inode锁
	}
	spin_unlock(&inode_lock);	// 最终解锁inode锁
	iput(old_inode);	// 释放最后一个处理的inode
}

/**
 * writeback_inodes_sb	-	writeback dirty inodes from given super_block
 * @sb: the superblock
 *
 * Start writeback on some inodes on this super_block. No guarantees are made
 * on how many (if any) will be written, and this function does not wait
 * for IO completion of submitted IO. The number of pages submitted is
 * returned.
 */
/**
 * writeback_inodes_sb - 从给定的超级块中回写脏inode
 * @sb: 超级块
 *
 * 在此超级块上启动一些inode的回写。没有保证会写回多少个inode（如果有的话），
 * 并且此函数不会等待提交的IO完成。返回提交的页面数。
 */
/**
 * 用来启动超级块中一些inode的回写过程。它首先计算了文件系统中脏页的数量和NFS系统中不稳定页的数量，然后计算出需要写回的总页数。
 * 这个总页数是脏页数加上不稳定页数再加上总的inode数减去未使用的inode数。函数通过调用 bdi_start_writeback 启动写回过程，
 * 但不会等待IO完成，也没有保证完成的inode数量。这种设计可以灵活地处理大量的数据，适用于需要快速响应的场景，
 * 而不是确保所有数据都被写回。
 */
void writeback_inodes_sb(struct super_block *sb)
{
	// 计算文件系统中脏页的数量
	unsigned long nr_dirty = global_page_state(NR_FILE_DIRTY);
	// 计算NFS系统中不稳定页的数量
	unsigned long nr_unstable = global_page_state(NR_UNSTABLE_NFS);
	long nr_to_write;	// 将要写回的页数

	// 总的要写回的页数包括脏页、不稳定页和总inode的数量减去未使用的inode数量
	nr_to_write = nr_dirty + nr_unstable +
			(inodes_stat.nr_inodes - inodes_stat.nr_unused);

	// 从给定的超级块的后备设备开始写回
	bdi_start_writeback(sb->s_bdi, sb, nr_to_write);
}
EXPORT_SYMBOL(writeback_inodes_sb);

/**
 * writeback_inodes_sb_if_idle	-	start writeback if none underway
 * @sb: the superblock
 *
 * Invoke writeback_inodes_sb if no writeback is currently underway.
 * Returns 1 if writeback was started, 0 if not.
 */
/**
 * writeback_inodes_sb_if_idle - 如果当前没有进行中的回写，则开始回写
 * @sb: 超级块
 *
 * 如果当前没有回写正在进行，调用writeback_inodes_sb。
 * 如果启动了回写则返回1，如果没有则返回0。
 */
/*
 * 定义了一个函数 writeback_inodes_sb_if_idle，它用于在没有其他回写操作进行时启动超级块的回写操作。
 * 函数首先检查超级块关联的后备设备（通过 sb->s_bdi 访问）是否有回写正在进行，如果没有，则调用 writeback_inodes_sb 
 * 函数来开始回写过程，并返回1表示回写已经启动。如果有回写正在进行，则不进行操作并返回0。
 * 这种设计适用于避免在回写已经足够频繁的情况下再次触发回写，从而减少系统负载和潜在的写放大。
 */
int writeback_inodes_sb_if_idle(struct super_block *sb)
{
	// 检查指定的后备设备是否正在进行回写
	if (!writeback_in_progress(sb->s_bdi)) {
		// 如果没有回写正在进行，调用writeback_inodes_sb开始回写
		writeback_inodes_sb(sb);
		return 1;	// 回写启动，返回1
	} else
		return 0;	// 回写未启动，返回0
}
EXPORT_SYMBOL(writeback_inodes_sb_if_idle);

/**
 * sync_inodes_sb	-	sync sb inode pages
 * @sb: the superblock
 *
 * This function writes and waits on any dirty inode belonging to this
 * super_block. The number of pages synced is returned.
 */
/**
 * sync_inodes_sb - 同步超级块的inode页面
 * @sb: 超级块
 *
 * 这个函数写入并等待属于这个超级块的任何脏inode。返回同步的页面数。
 */
/*
 * 一个专门用于同步一个超级块中所有脏inode的函数 sync_inodes_sb。该函数首先使用 bdi_sync_writeback 函数发起超级块的脏
 * inode写回操作，该操作会将所有脏inode提交给底层的I/O系统进行异步处理，但此函数本身并不等待这些I/O操作完成。
 * 接着，通过 wait_sb_inodes 函数等待之前启动的所有I/O操作完成，确保所有的数据都被稳定地写到了存储设备上。
 */
void sync_inodes_sb(struct super_block *sb)
{
	// 调用bdi_sync_writeback同步写回超级块的脏inode，不等待I/O完成
	bdi_sync_writeback(sb->s_bdi, sb);
	// 调用wait_sb_inodes等待所有inode的I/O操作完成
	wait_sb_inodes(sb);
}
EXPORT_SYMBOL(sync_inodes_sb);

/**
 * write_inode_now	-	write an inode to disk
 * @inode: inode to write to disk
 * @sync: whether the write should be synchronous or not
 *
 * This function commits an inode to disk immediately if it is dirty. This is
 * primarily needed by knfsd.
 *
 * The caller must either have a ref on the inode or must have set I_WILL_FREE.
 */
/**
 * write_inode_now - 立即将一个inode写入磁盘
 * @inode: 要写入磁盘的inode
 * @sync: 写入是否应该是同步的
 *
 * 如果inode是脏的，这个函数会立即将其提交到磁盘。这主要是由knfsd所需。
 *
 * 调用者必须持有inode的引用或者必须已经设置了I_WILL_FREE。
 */
/*
 * 定义了 write_inode_now 函数，其作用是立即将一个脏的inode写入磁盘。该函数接受两个参数：一个 inode 对象和一个布尔值 sync，
 * 后者指示写入是否应该是同步的。函数首先配置一个 writeback_control 结构体，用以控制写回操作的行为，包括写回的数据量和范围。
 * 如果inode的映射不支持写回脏数据，则将写回的数据量设置为0，实际上不进行任何写回操作。
 */
int write_inode_now(struct inode *inode, int sync)
{
	int ret;  // 用于存储返回值
	struct writeback_control wbc = {
		.nr_to_write = LONG_MAX,  // 设置为尽可能多地写入
		.sync_mode = sync ? WB_SYNC_ALL : WB_SYNC_NONE,  // 根据sync参数设置同步模式
		.range_start = 0,  // 写回范围的起始
		.range_end = LLONG_MAX,  // 写回范围的结束
	};

	// 如果inode的映射不支持写回脏数据，则设置不写入任何数据
	if (!mapping_cap_writeback_dirty(inode->i_mapping))
		wbc.nr_to_write = 0;

	might_sleep();  // 在可能睡眠的上下文中调用
	spin_lock(&inode_lock);  // 获取inode锁
	ret = writeback_single_inode(inode, &wbc);  // 调用writeback_single_inode进行写回
	spin_unlock(&inode_lock);  // 释放inode锁
	if (sync)
		inode_sync_wait(inode);  // 如果是同步写入，则等待写入完成
	return ret;  // 返回操作结果
}
EXPORT_SYMBOL(write_inode_now);

/**
 * sync_inode - write an inode and its pages to disk.
 * @inode: the inode to sync
 * @wbc: controls the writeback mode
 *
 * sync_inode() will write an inode and its pages to disk.  It will also
 * correctly update the inode on its superblock's dirty inode lists and will
 * update inode->i_state.
 *
 * The caller must have a ref on the inode.
 */
/**
 * sync_inode - 将一个inode及其页写入磁盘。
 * @inode: 需要同步的inode
 * @wbc: 控制回写模式的控制结构
 *
 * sync_inode() 将把一个inode及其页写入磁盘。它还会正确地更新inode在其超级块的脏inode列表上的位置，
 * 并会更新 inode->i_state。
 *
 * 调用者必须持有一个对inode的引用。
 */
/*
 * 定义了 sync_inode 函数，该函数的目的是将指定的inode及其关联的页面写入磁盘。函数接收两个参数：一个 inode 指针和一个 
 * writeback_control 结构体指针，后者用于控制回写操作的具体行为。
 */
int sync_inode(struct inode *inode, struct writeback_control *wbc)
{
	int ret;	// 用于存储函数返回值

	// 锁定inode锁，防止其他进程同时修改
	spin_lock(&inode_lock);
	// 调用writeback_single_inode函数对单个inode进行写回
	ret = writeback_single_inode(inode, wbc);
	// 解锁inode锁
	spin_unlock(&inode_lock);
	return ret;	// 返回writeback_single_inode的结果
}
EXPORT_SYMBOL(sync_inode);
