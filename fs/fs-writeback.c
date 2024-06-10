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
struct bdi_work {
	/* 待处理工作列表 */
	struct list_head list;		/* pending work list */
	/* 用于RCU释放或清理工作项 */
	struct rcu_head rcu_head;	/* for RCU free/clear of work */

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
	WS_USED_B = 0,
	WS_ONSTACK_B,
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
	return !list_empty(&bdi->work_list);
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
	const enum writeback_sync_modes sync_mode = work->args.sync_mode;
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
		call_rcu(&work->rcu_head, bdi_work_free);
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
		    TASK_UNINTERRUPTIBLE);
}

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
static void bdi_sync_writeback(struct backing_dev_info *bdi,
			       struct super_block *sb)
{
	struct wb_writeback_args args = {
		.sb		= sb,
		// 设置同步模式为完全同步
		.sync_mode	= WB_SYNC_ALL,
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
static void redirty_tail(struct inode *inode)
{
	// 获取inode对应的后备设备的写回控制结构
	struct bdi_writeback *wb = &inode_to_bdi(inode)->wb;

	if (!list_empty(&wb->b_dirty)) {	// 如果脏inode列表不为空
		struct inode *tail;

		// 获取列表中最新的inode
		tail = list_entry(wb->b_dirty.next, struct inode, i_list);
		// 如果当前inode的脏时间早于列表中的inode
		if (time_before(inode->dirtied_when, tail->dirtied_when))
			// 更新当前inode的脏时间为当前时间
			inode->dirtied_when = jiffies;
	}
	// 将inode移动到脏列表的末尾
	list_move(&inode->i_list, &wb->b_dirty);
}

/*
 * requeue inode for re-scanning after bdi->b_io list is exhausted.
 */
/*
 * 在bdi->b_io列表用尽后，重新排队inode以重新扫描。
 */
static void requeue_io(struct inode *inode)
{
	// 获取inode对应的后备设备的写回控制结构
	struct bdi_writeback *wb = &inode_to_bdi(inode)->wb;

	// 将inode移动到更多IO处理列表
	list_move(&inode->i_list, &wb->b_more_io);
}

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
		if (wbc->sync_mode != WB_SYNC_ALL) {	// 如果不是为了数据完整性的同步写回
			// 重新排队此inode以后处理
			requeue_io(inode);
			return 0;	// 返回成功
		}

		/*
		 * It's a data-integrity sync.  We must wait.
		 */
		// 等待inode写回完成
		inode_wait_for_writeback(inode);
	}

	// 确保此时inode未被锁定
	BUG_ON(inode->i_state & I_SYNC);

	/* Set I_SYNC, reset I_DIRTY */
	// 设置I_SYNC标志，重置I_DIRTY
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
	// 如果是同步模式
	if (wbc->sync_mode == WB_SYNC_ALL) {
		// 等待文件数据写回完成
		int err = filemap_fdatawait(mapping);
		if (ret == 0)
			ret = err;
	}

	/* Don't write the inode if only I_DIRTY_PAGES was set */
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
			goto select_queue;
		} else if (inode->i_state & I_DIRTY) {
			// 如果inode被重新弄脏
			/*
			 * At least XFS will redirty the inode during the
			 * writeback (delalloc) and on io completion (isize).
			 */
			redirty_tail(inode);
		} else if (mapping_tagged(mapping, PAGECACHE_TAG_DIRTY)) {
			/*
			 * We didn't write back all the pages.  nfs_writepages()
			 * sometimes bales out without doing anything. Redirty
			 * the inode; Move it from b_io onto b_more_io/b_dirty.
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
			// 如果没有写回所有页面
			if (wbc->for_kupdate) {
				/*
				 * For the kupdate function we move the inode
				 * to b_more_io so it will get more writeout as
				 * soon as the queue becomes uncongested.
				 */
				inode->i_state |= I_DIRTY_PAGES;
select_queue:
				// 如果写回限额已用完，重新排队此inode
				if (wbc->nr_to_write <= 0) {
					/*
					 * slice used up: queue for next turn
					 */
					requeue_io(inode);
				} else {
					/*
					 * somehow blocked: retry later
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
				inode->i_state |= I_DIRTY_PAGES;
				redirty_tail(inode);
			}
		} else if (atomic_read(&inode->i_count)) {
			/*
			 * The inode is clean, inuse
			 */
			// 如果inode干净且正在使用中
			list_move(&inode->i_list, &inode_in_use);
		} else {
			/*
			 * The inode is clean, unused
			 */
			// 如果inode干净且未使用
			list_move(&inode->i_list, &inode_unused);
		}
	}
	inode_sync_complete(inode);	// 完成inode同步
	return ret;	// 返回结果
}

static void unpin_sb_for_writeback(struct super_block *sb)
{
	up_read(&sb->s_umount);
	put_super(sb);
}

enum sb_pin_state {
	SB_PINNED,
	SB_NOT_PINNED,
	SB_PIN_FAILED
};

/*
 * For WB_SYNC_NONE writeback, the caller does not have the sb pinned
 * before calling writeback. So make sure that we do pin it, so it doesn't
 * go away while we are writing inodes from it.
 */
static enum sb_pin_state pin_sb_for_writeback(struct writeback_control *wbc,
					      struct super_block *sb)
{
	/*
	 * Caller must already hold the ref for this
	 */
	if (wbc->sync_mode == WB_SYNC_ALL) {
		WARN_ON(!rwsem_is_locked(&sb->s_umount));
		return SB_NOT_PINNED;
	}
	spin_lock(&sb_lock);
	sb->s_count++;
	if (down_read_trylock(&sb->s_umount)) {
		if (sb->s_root) {
			spin_unlock(&sb_lock);
			return SB_PINNED;
		}
		/*
		 * umounted, drop rwsem again and fall through to failure
		 */
		up_read(&sb->s_umount);
	}
	sb->s_count--;
	spin_unlock(&sb_lock);
	return SB_PIN_FAILED;
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
			// 如果指定了超级块且当前inode的超级块不匹配，则跳过此inode
			redirty_tail(inode);
			continue;
		}
		if (sb != inode->i_sb)
			/* finish with this superblock */
			// 完成此超级块的处理
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
		// 检查此inode是否在sync_sb_inodes被调用后被弄脏
		if (inode_dirtied_after(inode, wbc->wb_start))
			return 1;

		// 检查inode状态的合理性
		BUG_ON(inode->i_state & (I_FREEING | I_CLEAR));
		__iget(inode);	// 增加inode的引用计数
		pages_skipped = wbc->pages_skipped;
		// 写回单个inode
		writeback_single_inode(inode, wbc);
		if (wbc->pages_skipped != pages_skipped) {
			/*
			 * writeback is not making progress due to locked
			 * buffers.  Skip this inode for now.
			 */
			// 如果写回没有取得进展，可能是因为缓冲区锁定。现在跳过此inode。
			redirty_tail(inode);
		}
		spin_unlock(&inode_lock);	// 解锁
		iput(inode);	// 减少inode的引用计数
		cond_resched();	// 条件调度
		spin_lock(&inode_lock);	// 加锁
		if (wbc->nr_to_write <= 0) {
			wbc->more_io = 1;
			return 1;
		}
		if (!list_empty(&wb->b_more_io))
			wbc->more_io = 1;
	}
	/* b_io is empty */
	// b_io列表为空
	return 1;
}

/*
 * writeback_inodes_wb - 处理写回操作的核心函数
 * @wb: 写回控制块，包含相关inode链表等信息
 * @wbc: 写回控制结构，包含写回操作的参数和状态
 *
 * 这个函数执行具体的写回操作，处理wb中的inode列表，根据条件选择合适的inode进行写回。
 * 这个过程包括对超级块的操作锁定，以确保文件系统的一致性。
 */
static void writeback_inodes_wb(struct bdi_writeback *wb,
				struct writeback_control *wbc)
{
	int ret = 0;

	/* 避免活锁 */
	wbc->wb_start = jiffies; /* livelock avoidance */
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
			// 将inode重新标记为dirty，放回列表末尾
			redirty_tail(inode);
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
#define MAX_WRITEBACK_PAGES     1024

static inline bool over_bground_thresh(void)
{
	unsigned long background_thresh, dirty_thresh;

	get_dirty_limits(&background_thresh, &dirty_thresh, NULL, NULL);

	return (global_page_state(NR_FILE_DIRTY) +
		global_page_state(NR_UNSTABLE_NFS) >= background_thresh);
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
static long wb_writeback(struct bdi_writeback *wb,
			 struct wb_writeback_args *args)
{
	struct writeback_control wbc = {
		.bdi			= wb->bdi,
		.sb			= args->sb,
		.sync_mode		= args->sync_mode,
		.older_than_this	= NULL,
		.for_kupdate		= args->for_kupdate,
		.for_background		= args->for_background,
		.range_cyclic		= args->range_cyclic,
	};
	unsigned long oldest_jif;
	long wrote = 0;
	struct inode *inode;

	if (wbc.for_kupdate) {
		wbc.older_than_this = &oldest_jif;
		oldest_jif = jiffies -
				msecs_to_jiffies(dirty_expire_interval * 10);
	}
	if (!wbc.range_cyclic) {
		wbc.range_start = 0;
		wbc.range_end = LLONG_MAX;
	}

	for (;;) {
		/*
		 * Stop writeback when nr_pages has been consumed
		 */
		if (args->nr_pages <= 0)
			break;

		/*
		 * For background writeout, stop when we are below the
		 * background dirty threshold
		 */
		if (args->for_background && !over_bground_thresh())
			break;

		wbc.more_io = 0;
		wbc.nr_to_write = MAX_WRITEBACK_PAGES;
		wbc.pages_skipped = 0;
		writeback_inodes_wb(wb, &wbc);
		args->nr_pages -= MAX_WRITEBACK_PAGES - wbc.nr_to_write;
		wrote += MAX_WRITEBACK_PAGES - wbc.nr_to_write;

		/*
		 * If we consumed everything, see if we have more
		 */
		if (wbc.nr_to_write <= 0)
			continue;
		/*
		 * Didn't write everything and we don't have more IO, bail
		 */
		if (!wbc.more_io)
			break;
		/*
		 * Did we write something? Try for more
		 */
		if (wbc.nr_to_write < MAX_WRITEBACK_PAGES)
			continue;
		/*
		 * Nothing written. Wait for some inode to
		 * become available for writeback. Otherwise
		 * we'll just busyloop.
		 */
		spin_lock(&inode_lock);
		if (!list_empty(&wb->b_more_io))  {
			inode = list_entry(wb->b_more_io.prev,
						struct inode, i_list);
			inode_wait_for_writeback(inode);
		}
		spin_unlock(&inode_lock);
	}

	return wrote;
}

/*
 * Return the next bdi_work struct that hasn't been processed by this
 * wb thread yet. ->seen is initially set for each thread that exists
 * for this device, when a thread first notices a piece of work it
 * clears its bit. Depending on writeback type, the thread will notify
 * completion on either receiving the work (WB_SYNC_NONE) or after
 * it is done (WB_SYNC_ALL).
 */
static struct bdi_work *get_next_work_item(struct backing_dev_info *bdi,
					   struct bdi_writeback *wb)
{
	struct bdi_work *work, *ret = NULL;

	rcu_read_lock();

	list_for_each_entry_rcu(work, &bdi->work_list, list) {
		if (!test_bit(wb->nr, &work->seen))
			continue;
		clear_bit(wb->nr, &work->seen);

		ret = work;
		break;
	}

	rcu_read_unlock();
	return ret;
}

static long wb_check_old_data_flush(struct bdi_writeback *wb)
{
	unsigned long expired;
	long nr_pages;

	expired = wb->last_old_flush +
			msecs_to_jiffies(dirty_writeback_interval * 10);
	if (time_before(jiffies, expired))
		return 0;

	wb->last_old_flush = jiffies;
	nr_pages = global_page_state(NR_FILE_DIRTY) +
			global_page_state(NR_UNSTABLE_NFS) +
			(inodes_stat.nr_inodes - inodes_stat.nr_unused);

	if (nr_pages) {
		struct wb_writeback_args args = {
			.nr_pages	= nr_pages,
			.sync_mode	= WB_SYNC_NONE,
			.for_kupdate	= 1,
			.range_cyclic	= 1,
		};

		return wb_writeback(wb, &args);
	}

	return 0;
}

/*
 * Retrieve work items and do the writeback they describe
 */
long wb_do_writeback(struct bdi_writeback *wb, int force_wait)
{
	struct backing_dev_info *bdi = wb->bdi;
	struct bdi_work *work;
	long wrote = 0;

	while ((work = get_next_work_item(bdi, wb)) != NULL) {
		struct wb_writeback_args args = work->args;

		/*
		 * Override sync mode, in case we must wait for completion
		 */
		if (force_wait)
			work->args.sync_mode = args.sync_mode = WB_SYNC_ALL;

		/*
		 * If this isn't a data integrity operation, just notify
		 * that we have seen this work and we are now starting it.
		 */
		if (args.sync_mode == WB_SYNC_NONE)
			wb_clear_pending(wb, work);

		wrote += wb_writeback(wb, &args);

		/*
		 * This is a data integrity writeback, so only do the
		 * notification when we have completed the work.
		 */
		if (args.sync_mode == WB_SYNC_ALL)
			wb_clear_pending(wb, work);
	}

	/*
	 * Check for periodic writeback, kupdated() style
	 */
	wrote += wb_check_old_data_flush(wb);

	return wrote;
}

/*
 * Handle writeback of dirty data for the device backed by this bdi. Also
 * wakes up periodically and does kupdated style flushing.
 */
int bdi_writeback_task(struct bdi_writeback *wb)
{
	unsigned long last_active = jiffies;
	unsigned long wait_jiffies = -1UL;
	long pages_written;

	while (!kthread_should_stop()) {
		pages_written = wb_do_writeback(wb, 0);

		if (pages_written)
			last_active = jiffies;
		else if (wait_jiffies != -1UL) {
			unsigned long max_idle;

			/*
			 * Longest period of inactivity that we tolerate. If we
			 * see dirty data again later, the task will get
			 * recreated automatically.
			 */
			max_idle = max(5UL * 60 * HZ, wait_jiffies);
			if (time_after(jiffies, max_idle + last_active))
				break;
		}

		wait_jiffies = msecs_to_jiffies(dirty_writeback_interval * 10);
		schedule_timeout_interruptible(wait_jiffies);
		try_to_freeze();
	}

	return 0;
}

/*
 * Schedule writeback for all backing devices. This does WB_SYNC_NONE
 * writeback, for integrity writeback see bdi_sync_writeback().
 */
static void bdi_writeback_all(struct super_block *sb, long nr_pages)
{
	struct wb_writeback_args args = {
		.sb		= sb,
		.nr_pages	= nr_pages,
		.sync_mode	= WB_SYNC_NONE,
	};
	struct backing_dev_info *bdi;

	rcu_read_lock();

	list_for_each_entry_rcu(bdi, &bdi_list, bdi_list) {
		if (!bdi_has_dirty_io(bdi))
			continue;

		bdi_alloc_queue_work(bdi, &args);
	}

	rcu_read_unlock();
}

/*
 * Start writeback of `nr_pages' pages.  If `nr_pages' is zero, write back
 * the whole world.
 */
void wakeup_flusher_threads(long nr_pages)
{
	if (nr_pages == 0)
		nr_pages = global_page_state(NR_FILE_DIRTY) +
				global_page_state(NR_UNSTABLE_NFS);
	bdi_writeback_all(NULL, nr_pages);
}

static noinline void block_dump___mark_inode_dirty(struct inode *inode)
{
	if (inode->i_ino || strcmp(inode->i_sb->s_id, "bdev")) {
		struct dentry *dentry;
		const char *name = "?";

		dentry = d_find_alias(inode);
		if (dentry) {
			spin_lock(&dentry->d_lock);
			name = (const char *) dentry->d_name.name;
		}
		printk(KERN_DEBUG
		       "%s(%d): dirtied inode %lu (%s) on %s\n",
		       current->comm, task_pid_nr(current), inode->i_ino,
		       name, inode->i_sb->s_id);
		if (dentry) {
			spin_unlock(&dentry->d_lock);
			dput(dentry);
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
void __mark_inode_dirty(struct inode *inode, int flags)
{
	struct super_block *sb = inode->i_sb;

	/*
	 * Don't do this for I_DIRTY_PAGES - that doesn't actually
	 * dirty the inode itself
	 */
	if (flags & (I_DIRTY_SYNC | I_DIRTY_DATASYNC)) {
		if (sb->s_op->dirty_inode)
			sb->s_op->dirty_inode(inode);
	}

	/*
	 * make sure that changes are seen by all cpus before we test i_state
	 * -- mikulas
	 */
	smp_mb();

	/* avoid the locking if we can */
	if ((inode->i_state & flags) == flags)
		return;

	if (unlikely(block_dump))
		block_dump___mark_inode_dirty(inode);

	spin_lock(&inode_lock);
	if ((inode->i_state & flags) != flags) {
		const int was_dirty = inode->i_state & I_DIRTY;

		inode->i_state |= flags;

		/*
		 * If the inode is being synced, just update its dirty state.
		 * The unlocker will place the inode on the appropriate
		 * superblock list, based upon its state.
		 */
		if (inode->i_state & I_SYNC)
			goto out;

		/*
		 * Only add valid (hashed) inodes to the superblock's
		 * dirty list.  Add blockdev inodes as well.
		 */
		if (!S_ISBLK(inode->i_mode)) {
			if (hlist_unhashed(&inode->i_hash))
				goto out;
		}
		if (inode->i_state & (I_FREEING|I_CLEAR))
			goto out;

		/*
		 * If the inode was already on b_dirty/b_io/b_more_io, don't
		 * reposition it (that would break b_dirty time-ordering).
		 */
		if (!was_dirty) {
			struct bdi_writeback *wb = &inode_to_bdi(inode)->wb;
			struct backing_dev_info *bdi = wb->bdi;

			if (bdi_cap_writeback_dirty(bdi) &&
			    !test_bit(BDI_registered, &bdi->state)) {
				WARN_ON(1);
				printk(KERN_ERR "bdi-%s not registered\n",
								bdi->name);
			}

			inode->dirtied_when = jiffies;
			list_move(&inode->i_list, &wb->b_dirty);
		}
	}
out:
	spin_unlock(&inode_lock);
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
static void wait_sb_inodes(struct super_block *sb)
{
	struct inode *inode, *old_inode = NULL;

	/*
	 * We need to be protected against the filesystem going from
	 * r/o to r/w or vice versa.
	 */
	WARN_ON(!rwsem_is_locked(&sb->s_umount));

	spin_lock(&inode_lock);

	/*
	 * Data integrity sync. Must wait for all pages under writeback,
	 * because there may have been pages dirtied before our sync
	 * call, but which had writeout started before we write it out.
	 * In which case, the inode may not be on the dirty list, but
	 * we still have to wait for that writeout.
	 */
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		struct address_space *mapping;

		if (inode->i_state & (I_FREEING|I_CLEAR|I_WILL_FREE|I_NEW))
			continue;
		mapping = inode->i_mapping;
		if (mapping->nrpages == 0)
			continue;
		__iget(inode);
		spin_unlock(&inode_lock);
		/*
		 * We hold a reference to 'inode' so it couldn't have
		 * been removed from s_inodes list while we dropped the
		 * inode_lock.  We cannot iput the inode now as we can
		 * be holding the last reference and we cannot iput it
		 * under inode_lock. So we keep the reference and iput
		 * it later.
		 */
		iput(old_inode);
		old_inode = inode;

		filemap_fdatawait(mapping);

		cond_resched();

		spin_lock(&inode_lock);
	}
	spin_unlock(&inode_lock);
	iput(old_inode);
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
void writeback_inodes_sb(struct super_block *sb)
{
	unsigned long nr_dirty = global_page_state(NR_FILE_DIRTY);
	unsigned long nr_unstable = global_page_state(NR_UNSTABLE_NFS);
	long nr_to_write;

	nr_to_write = nr_dirty + nr_unstable +
			(inodes_stat.nr_inodes - inodes_stat.nr_unused);

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
int writeback_inodes_sb_if_idle(struct super_block *sb)
{
	if (!writeback_in_progress(sb->s_bdi)) {
		writeback_inodes_sb(sb);
		return 1;
	} else
		return 0;
}
EXPORT_SYMBOL(writeback_inodes_sb_if_idle);

/**
 * sync_inodes_sb	-	sync sb inode pages
 * @sb: the superblock
 *
 * This function writes and waits on any dirty inode belonging to this
 * super_block. The number of pages synced is returned.
 */
void sync_inodes_sb(struct super_block *sb)
{
	bdi_sync_writeback(sb->s_bdi, sb);
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
int write_inode_now(struct inode *inode, int sync)
{
	int ret;
	struct writeback_control wbc = {
		.nr_to_write = LONG_MAX,
		.sync_mode = sync ? WB_SYNC_ALL : WB_SYNC_NONE,
		.range_start = 0,
		.range_end = LLONG_MAX,
	};

	if (!mapping_cap_writeback_dirty(inode->i_mapping))
		wbc.nr_to_write = 0;

	might_sleep();
	spin_lock(&inode_lock);
	ret = writeback_single_inode(inode, &wbc);
	spin_unlock(&inode_lock);
	if (sync)
		inode_sync_wait(inode);
	return ret;
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
int sync_inode(struct inode *inode, struct writeback_control *wbc)
{
	int ret;

	spin_lock(&inode_lock);
	ret = writeback_single_inode(inode, wbc);
	spin_unlock(&inode_lock);
	return ret;
}
EXPORT_SYMBOL(sync_inode);
