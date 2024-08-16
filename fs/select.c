/*
 * This file contains the procedures for the handling of select and poll
 *
 * Created for Linux based loosely upon Mathius Lattner's minix
 * patches by Peter MacDonald. Heavily edited by Linus.
 *
 *  4 February 1994
 *     COFF/ELF binary emulation. If the process has the STICKY_TIMEOUTS
 *     flag set in its personality we do *not* modify the given timeout
 *     parameter to reflect time remaining.
 *
 *  24 January 2000
 *     Changed sys_poll()/do_poll() to use PAGE_SIZE chunk-based allocation 
 *     of fds to overcome nfds < 16390 descriptors limit (Tigran Aivazian).
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/personality.h> /* for STICKY_TIMEOUTS */
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/rcupdate.h>
#include <linux/hrtimer.h>

#include <asm/uaccess.h>


/*
 * Estimate expected accuracy in ns from a timeval.
 *
 * After quite a bit of churning around, we've settled on
 * a simple thing of taking 0.1% of the timeout as the
 * slack, with a cap of 100 msec.
 * "nice" tasks get a 0.5% slack instead.
 *
 * Consider this comment an open invitation to come up with even
 * better solutions..
 */
/*
 * 从timeval估计预期的精确度（单位为纳秒）。
 *
 * 经过一番深思熟虑，我们决定采用简单的方法，即取超时时间的0.1%作为
 * 时间弹性（slack），上限为100毫秒。
 * "nice"（系统优先级较低的）任务则得到0.5%的时间弹性。
 *
 * 请将此评论视为开发更好解决方案的公开邀请。
 */

#define MAX_SLACK	(100 * NSEC_PER_MSEC)  // 定义最大的时间弹性为100毫秒（转换为纳秒）

// 用于估算基于时间的精度，它在系统中用于计算等待事件（如轮询或选择等待）的时间精度。
static long __estimate_accuracy(struct timespec *tv)
{
	long slack;  // 用来存储计算出的时间弹性
	int divfactor = 1000;  // 默认的除数因子，对应0.1%的时间弹性

	if (tv->tv_sec < 0)  // 如果传入的时间值为负，则直接返回0，意味着没有时间弹性
		return 0;

	if (task_nice(current) > 0)  // 如果当前任务的nice值大于0（优先级较低）
		divfactor = divfactor / 5;  // 调整除数因子为原来的1/5，即0.5%的时间弹性

	if (tv->tv_sec > MAX_SLACK / (NSEC_PER_SEC/divfactor))  // 如果时间秒数超过了最大弹性允许的秒数
		return MAX_SLACK;  // 返回最大弹性

	slack = tv->tv_nsec / divfactor;  // 计算纳秒部分的时间弹性
	slack += tv->tv_sec * (NSEC_PER_SEC/divfactor);  // 加上秒部分转换为纳秒后的时间弹性

	if (slack > MAX_SLACK)  // 如果计算的时间弹性超过了最大值
		return MAX_SLACK;  // 返回最大弹性

	return slack;  // 返回计算的时间弹性
}

// 实现了一个评估时间精度的函数，适用于非实时任务，并考虑了系统当前时间以及任务的定时器松弛时间（timer slack）
static long estimate_accuracy(struct timespec *tv)
{
	unsigned long ret;  // 用来存储计算出的时间弹性
	struct timespec now;  // 用来存储当前的时间戳

	/*
	 * Realtime tasks get a slack of 0 for obvious reasons.
	 */
	/*
	 * 实时任务的时间弹性为0，原因显而易见。
	 */

	if (rt_task(current))  // 如果当前任务是实时任务
		return 0;  // 直接返回0，实时任务需要严格的时间管理，不允许有弹性

	ktime_get_ts(&now);  // 获取当前的时间戳
	now = timespec_sub(*tv, now);  // 计算目标时间和当前时间的差值
	ret = __estimate_accuracy(&now);  // 使用先前定义的函数计算这个时间差的时间弹性

	/*
	 * 如果计算出的时间弹性小于当前任务的timer_slack_ns，
	 * 则使用timer_slack_ns作为时间弹性，否则使用计算出的时间弹性。
	 */
	if (ret < current->timer_slack_ns)  // 如果计算的时间弹性小于任务的定时器松弛值
		return current->timer_slack_ns;  // 返回任务的定时器松弛值
	return ret;  // 返回计算的时间弹性
}

struct poll_table_page {
	struct poll_table_page * next;  // 指向下一个 poll_table_page 的指针
	struct poll_table_entry * entry;  // 指向当前正在使用的 poll_table_entry 的指针
	struct poll_table_entry entries[0];  // 灵活数组，存储 poll_table_entry 结构
};

// 用于检查一个 poll_table_page 是否已满
#define POLL_TABLE_FULL(table) \
	((unsigned long)((table)->entry+1) > PAGE_SIZE + (unsigned long)(table))

/*
 * Ok, Peter made a complicated, but straightforward multiple_wait() function.
 * I have rewritten this, taking some shortcuts: This code may not be easy to
 * follow, but it should be free of race-conditions, and it's practical. If you
 * understand what I'm doing here, then you understand how the linux
 * sleep/wakeup mechanism works.
 *
 * Two very simple procedures, poll_wait() and poll_freewait() make all the
 * work.  poll_wait() is an inline-function defined in <linux/poll.h>,
 * as all select/poll functions have to call it to add an entry to the
 * poll table.
 */
/*
 * 好的，Peter 创建了一个复杂但直接的 multiple_wait() 函数。
 * 我重新写了这个，采取了一些简化的措施：这段代码可能不容易理解，
 * 但它应该是没有竞争条件的，并且是实用的。如果你理解我在这里做的事情，
 * 那么你就理解了 Linux 的睡眠/唤醒机制是如何工作的。
 *
 * 两个非常简单的过程，poll_wait() 和 poll_freewait() 完成了所有的工作。
 * poll_wait() 是一个在 <linux/poll.h> 中定义的内联函数，
 * 因为所有 select/poll 函数都必须调用它来向轮询表中添加一个条目。
 */
static void __pollwait(struct file *filp, wait_queue_head_t *wait_address,
		       poll_table *p);

// 于初始化 poll_wqueues 结构体，为使用该结构进行轮询操作做准备。初始化包括设置函数指针、当前任务、触发状态、错误状态等
void poll_initwait(struct poll_wqueues *pwq)
{
	init_poll_funcptr(&pwq->pt, __pollwait);  // 初始化轮询表结构的函数指针，设置为 __pollwait 函数
	pwq->polling_task = current;  // 设置当前的任务结构体为正在轮询的任务
	pwq->triggered = 0;  // 初始化触发状态为0
	pwq->error = 0;  // 初始化错误状态为0
	pwq->table = NULL;  // 初始化轮询表指针为NULL
	pwq->inline_index = 0;  // 初始化内联索引为0
}
EXPORT_SYMBOL(poll_initwait);

// 释放单个轮询表项的函数
static void free_poll_entry(struct poll_table_entry *entry)
{
	remove_wait_queue(entry->wait_address, &entry->wait);  // 从等待队列中移除
	fput(entry->filp);  // 减少文件描述符的引用计数，可能会导致文件关闭
}

// 清理所有轮询表项的函数
void poll_freewait(struct poll_wqueues *pwq)
{
	struct poll_table_page * p = pwq->table;  // 指向轮询表的首个页面
	int i;
	// 首先处理内联条目
	for (i = 0; i < pwq->inline_index; i++)
		free_poll_entry(pwq->inline_entries + i);  // 释放内联条目中的每个轮询表项
	
	// 处理通过动态内存分配的轮询表页
	while (p) {
		struct poll_table_entry * entry;
		struct poll_table_page *old;

		entry = p->entry;  // 指向当前页的最后一个轮询表项
		do {
			entry--;  // 向前移动到前一个轮询表项
			free_poll_entry(entry);  // 释放这个轮询表项
		} while (entry > p->entries);  // 检查是否到达了当前页的开始位置
		
		old = p;  // 保存当前页的指针
		p = p->next;  // 移动到下一页
		free_page((unsigned long) old);  // 释放当前页占用的内存
	}
}
EXPORT_SYMBOL(poll_freewait);

// 从给定的poll_wqueues结构中获取一个新的poll_table_entry
static struct poll_table_entry *poll_get_entry(struct poll_wqueues *p)
{
	struct poll_table_page *table = p->table;  // 指向当前轮询表页面

	// 如果内联数组未满，直接返回一个内联数组中的新条目
	if (p->inline_index < N_INLINE_POLL_ENTRIES)
		return p->inline_entries + p->inline_index++;  // 返回当前条目的指针后，索引加一

	// 如果当前页面已满或不存在，需要分配一个新的页面
	if (!table || POLL_TABLE_FULL(table)) {
		struct poll_table_page *new_table;  // 定义新表的指针

		// 尝试分配一个新的页面
		new_table = (struct poll_table_page *) __get_free_page(GFP_KERNEL);  // 分配一个内核内存页
		if (!new_table) {  // 如果内存分配失败
			p->error = -ENOMEM;  // 设置错误码为内存不足
			return NULL;  // 返回NULL
		}
		new_table->entry = new_table->entries;  // 初始化新表的entry指针到entries的开始位置
		new_table->next = table;  // 将新表的next指向当前表
		p->table = new_table;  // 更新poll_wqueues结构的table指针到新表
		table = new_table;  // 将table指针更新为新表
	}

	// 返回新表中的下一个可用条目，并递增entry指针
	return table->entry++;
}

// 自定义的唤醒函数，用于在轮询操作中唤醒等待队列
static int __pollwake(wait_queue_t *wait, unsigned mode, int sync, void *key)
{
	struct poll_wqueues *pwq = wait->private;	// 从等待队列项中获取私有数据，这里是poll_wqueues结构
	DECLARE_WAITQUEUE(dummy_wait, pwq->polling_task);	// 声明并初始化一个虚拟等待队列项，用于当前轮询任务

	/*
	 * Although this function is called under waitqueue lock, LOCK
	 * doesn't imply write barrier and the users expect write
	 * barrier semantics on wakeup functions.  The following
	 * smp_wmb() is equivalent to smp_wmb() in try_to_wake_up()
	 * and is paired with set_mb() in poll_schedule_timeout.
	 */
	/*
   * 尽管此函数在等待队列锁的保护下被调用，但锁操作不意味着写内存屏障，
   * 用户期望唤醒函数具有写内存屏障语义。以下的 smp_wmb() 等效于 try_to_wake_up()
   * 中的 smp_wmb()，并与 poll_schedule_timeout 中的 set_mb() 成对。
   */
	smp_wmb();  // 插入一个写内存屏障，确保之前的内存写入在此之前完成
	pwq->triggered = 1;  // 设置触发标志为1，表示已触发

	/*
	 * Perform the default wake up operation using a dummy
	 * waitqueue.
	 *
	 * TODO: This is hacky but there currently is no interface to
	 * pass in @sync.  @sync is scheduled to be removed and once
	 * that happens, wake_up_process() can be used directly.
	 */
	/*
   * 使用一个虚拟的等待队列执行默认的唤醒操作。
   *
   * 注意：这种做法有些hacky，但当前没有接口来传递 @sync 参数。
   * @sync 计划被移除，一旦发生，可以直接使用 wake_up_process()。
   */
	return default_wake_function(&dummy_wait, mode, sync, key);  // 使用默认的唤醒函数来唤醒进程
}

// 定义pollwake函数，此函数用作轮询操作中的唤醒回调
static int pollwake(wait_queue_t *wait, unsigned mode, int sync, void *key)
{
	struct poll_table_entry *entry;

	// 通过容器宏从wait队列项中获取到包含它的poll_table_entry结构
	entry = container_of(wait, struct poll_table_entry, wait);

	// 检查是否有事件键，并且这个键是否与等待条目的键匹配
	if (key && !((unsigned long)key & entry->key))
			return 0;  // 如果不匹配，直接返回0，不进行唤醒

	// 如果匹配，调用__pollwake进行实际的唤醒操作
	return __pollwake(wait, mode, sync, key);
}

/* Add a new entry */
// 添加一个新的等待队列条目
static void __pollwait(struct file *filp, wait_queue_head_t *wait_address,
				poll_table *p)
{
	// 从poll_table结构体中获取包含它的poll_wqueues结构
	struct poll_wqueues *pwq = container_of(p, struct poll_wqueues, pt);

	// 尝试获取一个新的poll_table_entry
	struct poll_table_entry *entry = poll_get_entry(pwq);
	if (!entry)
		return; // 如果无法获取新条目，则直接返回

	// 增加文件的引用计数
	get_file(filp);

	// 初始化poll_table_entry结构
	entry->filp = filp; // 关联文件
	entry->wait_address = wait_address; // 设置等待队列头
	entry->key = p->key; // 设置触发事件的关键字
	init_waitqueue_func_entry(&entry->wait, pollwake); // 初始化等待队列条目，并设置唤醒函数为pollwake
	entry->wait.private = pwq; // 设置等待队列条目的私有数据为当前的poll_wqueues

	// 将新的等待队列条目添加到等待队列中
	add_wait_queue(wait_address, &entry->wait);
}

// 调度一个轮询等待的超时
int poll_schedule_timeout(struct poll_wqueues *pwq, int state,
			  ktime_t *expires, unsigned long slack)
{
	int rc = -EINTR;

	set_current_state(state);	// 设置当前任务的状态
	if (!pwq->triggered)	// 如果没有被触发，则调度一个高分辨率的定时器超时
		rc = schedule_hrtimeout_range(expires, slack, HRTIMER_MODE_ABS);
	// 将当前任务的状态设置回运行状态
	__set_current_state(TASK_RUNNING);

	/*
	 * Prepare for the next iteration.
	 *
	 * The following set_mb() serves two purposes.  First, it's
	 * the counterpart rmb of the wmb in pollwake() such that data
	 * written before wake up is always visible after wake up.
	 * Second, the full barrier guarantees that triggered clearing
	 * doesn't pass event check of the next iteration.  Note that
	 * this problem doesn't exist for the first iteration as
	 * add_wait_queue() has full barrier semantics.
	 */
	 /*
   * 准备下一次迭代。
   *
   * 下面的 set_mb() 有两个目的。首先，它是 pollwake() 中写内存屏障的读取配对，
   * 保证在唤醒之前的数据写入在唤醒之后总是可见的。其次，完整的屏障确保触发清除操作
   * 不会在下一次迭代的事件检查之前发生。注意，对于第一次迭代，这个问题不存在，
   * 因为 add_wait_queue() 具有完整屏障语义。
   */
	set_mb(pwq->triggered, 0);

	return rc;	// 返回调度结果
}
EXPORT_SYMBOL(poll_schedule_timeout);

/**
 * poll_select_set_timeout - helper function to setup the timeout value
 * @to:		pointer to timespec variable for the final timeout
 * @sec:	seconds (from user space)
 * @nsec:	nanoseconds (from user space)
 *
 * Note, we do not use a timespec for the user space value here, That
 * way we can use the function for timeval and compat interfaces as well.
 *
 * Returns -EINVAL if sec/nsec are not normalized. Otherwise 0.
 */
/**
 * poll_select_set_timeout - 帮助函数设置超时值
 * @to:		指向最终超时的 timespec 变量的指针
 * @sec:	秒（来自用户空间）
 * @nsec:	纳秒（来自用户空间）
 *
 * 注意，我们这里没有使用 timespec 作为用户空间的值。这样我们可以将此函数用于 timeval 和兼容接口。
 *
 * 如果 sec/nsec 没有规范化，则返回 -EINVAL。否则返回 0。
 */
int poll_select_set_timeout(struct timespec *to, long sec, long nsec)
{
	struct timespec ts = {.tv_sec = sec, .tv_nsec = nsec};	// 从给定的秒和纳秒参数创建一个 timespec 结构

	if (!timespec_valid(&ts))	// 检查 timespec 结构是否有效
		return -EINVAL;	// 如果时间值不合法，返回错误

	/* Optimize for the zero timeout value here */
	/* 在这里优化零超时值 */
	if (!sec && !nsec) {
		to->tv_sec = to->tv_nsec = 0;	// 如果秒和纳秒都为零，设置超时为零
	} else {
		ktime_get_ts(to);	// 获取当前时间
		*to = timespec_add_safe(*to, ts);	// 安全地将当前时间和用户提供的延迟相加，设置超时时间
	}
	return 0;	// 返回成功
}

/**
 * poll_select_copy_remaining - 将剩余的超时时间复制回用户空间
 * @end_time: [in] 指向结构体 timespec 的指针，表示最初指定的结束时间
 * @p: [in] 用户空间指针，指向存放剩余时间的位置
 * @timeval: [in] 标志是否使用 timeval 结构而非 timespec 结构
 * @ret: [in] 原始的 select 或 poll 调用的返回值
 *
 * 当 select 或 poll 调用不是因为时间到了而返回时，该函数会计算剩余时间，
 * 并尝试将它复制回用户空间。如果因为权限问题无法写入用户空间，函数会更改返回值。
 * 
 * 返回更新后的结果代码。
 */
static int poll_select_copy_remaining(struct timespec *end_time, void __user *p,
				      int timeval, int ret)
{
	struct timespec rts;  // 用于存放剩余时间的 timespec 结构
	struct timeval rtv;   // 用于存放剩余时间的 timeval 结构，如果需要的话

	if (!p)  // 如果用户没有提供一个指针来接收剩余时间
		return ret;  // 直接返回原始的系统调用结果

	if (current->personality & STICKY_TIMEOUTS)  // 检查是否设置了粘滞超时，如果是，跳转到sticky标签
		goto sticky;

	/* No update for zero timeout */
	// 如果指定的结束时间为零，那么没有剩余时间需要更新
	if (!end_time->tv_sec && !end_time->tv_nsec)
		return ret;

	ktime_get_ts(&rts);  // 获取当前时间
	rts = timespec_sub(*end_time, rts);  // 计算剩余时间
	if (rts.tv_sec < 0)  // 如果计算的结果是负数，说明已经超时
		rts.tv_sec = rts.tv_nsec = 0;  // 设置剩余时间为零

	if (timeval) {  // 如果用户期望返回的是 timeval 结构
		rtv.tv_sec = rts.tv_sec;
		rtv.tv_usec = rts.tv_nsec / NSEC_PER_USEC;  // 将纳秒转换为微秒

		if (!copy_to_user(p, &rtv, sizeof(rtv)))  // 将 timeval 结构复制到用户空间
			return ret;
	} else if (!copy_to_user(p, &rts, sizeof(rts)))  // 如果用户期望返回的是 timespec 结构
		return ret;

	/*
	 * If an application puts its timeval in read-only memory, we
	 * don't want the Linux-specific update to the timeval to
	 * cause a fault after the select has completed
	 * successfully. However, because we're not updating the
	 * timeval, we can't restart the system call.
	 */
	/*
	 * 如果应用程序将其 timeval 放在只读内存中，在 select 成功完成后，我们不希望
	 * Linux 特定的 timeval 更新导致错误。但因为我们没有更新 timeval，我们不能重启系统调用。
	 */

sticky:
	if (ret == -ERESTARTNOHAND)	// 如果原始的系统调用需要重启，但没有处理
		ret = -EINTR;	// 将错误代码更改为中断
	return ret;	// 返回更新后的结果代码
}

// 定义宏以方便访问fd_set_bits结构中的输入、输出、异常文件描述符集。
#define FDS_IN(fds, n)		(fds->in + n)
#define FDS_OUT(fds, n)		(fds->out + n)
#define FDS_EX(fds, n)		(fds->ex + n)

// 定义宏以方便获取输入、输出、异常文件描述符集中的所有位。
#define BITS(fds, n)	(*FDS_IN(fds, n)|*FDS_OUT(fds, n)|*FDS_EX(fds, n))

/**
 * max_select_fd - 确定最大有效的文件描述符索引
 * @n: 文件描述符数量
 * @fds: 指向fd_set_bits结构的指针，包含输入、输出和异常文件描述符集
 * 
 * 此函数用于计算有效文件描述符的最大索引值。
 * 它会检查指定的文件描述符集中设置的文件描述符是否有效（即在进程的打开文件表中）。
 * 
 * 返回值：如果成功，返回最大的文件描述符索引；如果有无效的文件描述符，返回-EBADF。
 */
static int max_select_fd(unsigned long n, fd_set_bits *fds)
{
	unsigned long *open_fds;  // 指向进程打开的文件描述符位图的指针
	unsigned long set;        // 临时变量，用于存放文件描述符集中的位
	int max;                  // 存放计算得出的最大文件描述符索引
	struct fdtable *fdt;      // 指向文件描述符表的指针

	/* handle last in-complete long-word first */
	// 处理最后一个不完整的长字（long word）
	set = ~(~0UL << (n & (__NFDBITS-1)));  // 计算需要检查的最后一部分文件描述符集的掩码
	n /= __NFDBITS;                         // 文件描述符总数转换为以长字为单位
	fdt = files_fdtable(current->files);    // 获取当前进程的文件描述符表
	open_fds = fdt->open_fds->fds_bits+n;  // 获取指向最后一组文件描述符集的指针
	max = 0;                                // 初始化最大文件描述符索引为0
	if (set) {  // 如果最后一组文件描述符集非空
		set &= BITS(fds, n);  // 获取当前组中所有被设置的文件描述符
		if (set) {  // 如果有文件描述符被设置
			if (!(set & ~*open_fds))  // 检查这些文件描述符是否都有效
				goto get_max;         // 如果有效，计算最大文件描述符索引
			return -EBADF;  // 如果有无效的文件描述符，返回错误
		}
	}
	while (n) {  // 逐个检查剩余的文件描述符集
		open_fds--;  // 移动到前一个文件描述符集
		n--;
		set = BITS(fds, n);  // 获取当前文件描述符集中所有被设置的文件描述符
		if (!set)  // 如果没有文件描述符被设置，继续下一组
			continue;
		if (set & ~*open_fds)  // 检查所有被设置的文件描述符是否有效
			return -EBADF;  // 如果有无效的文件描述符，返回错误
		if (max)  // 如果已经找到了最大文件描述符索引，继续检查剩余的文件描述符集
			continue;
get_max:
		do {  // 计算最大文件描述符索引
			max++;
			set >>= 1;
		} while (set);
		max += n * __NFDBITS;  // 计算总的最大文件描述符索引
	}

	return max;  // 返回最大文件描述符索引
}

// 定义输入相关事件的集合
#define POLLIN_SET (POLLRDNORM | POLLRDBAND | POLLIN | POLLHUP | POLLERR)
// 定义输出相关事件的集合
#define POLLOUT_SET (POLLWRBAND | POLLWRNORM | POLLOUT | POLLERR)
// 定义异常相关事件的集合
#define POLLEX_SET (POLLPRI)

/**
 * wait_key_set - 设置poll_table的key字段以反映感兴趣的事件
 * @wait: 指向poll_table的指针，用于存储等待事件信息
 * @in: 表示要检测输入相关事件的位掩码
 * @out: 表示要检测输出相关事件的位掩码
 * @bit: 当前处理的特定事件位
 * 
 * 此函数根据输入和输出位掩码以及当前事件位更新poll_table结构的key字段。
 * 如果指定事件位在输入或输出掩码中设置，则相应的事件集合会被加入到key中。
 */
static inline void wait_key_set(poll_table *wait, unsigned long in,
				unsigned long out, unsigned long bit)
{
	if (wait) { // 如果提供了有效的poll_table结构指针
		wait->key = POLLEX_SET; // 首先设置异常事件集
		if (in & bit) // 如果输入掩码中包含当前事件位
			wait->key |= POLLIN_SET; // 则添加输入相关的事件集到key
		if (out & bit) // 如果输出掩码中包含当前事件位
			wait->key |= POLLOUT_SET; // 则添加输出相关的事件集到key
	}
}

// 核心select函数实现
int do_select(int n, fd_set_bits *fds, struct timespec *end_time)
{
	ktime_t expire, *to = NULL;  // 定义超时时间结构
	struct poll_wqueues table;   // 定义等待队列结构
	poll_table *wait;            // 定义poll表指针
	int retval, i, timed_out = 0; // 定义返回值、循环变量和超时标志
	unsigned long slack = 0;      // 定义时间精度变量

	rcu_read_lock(); // 读取锁定，防止数据竞争
	retval = max_select_fd(n, fds); // 获取最大文件描述符数
	rcu_read_unlock(); // 读取解锁

	if (retval < 0) // 如果获取文件描述符失败，则返回错误
		return retval;
	n = retval;	// 设置有效的文件描述符数

	poll_initwait(&table);	// 初始化poll等待队列
	wait = &table.pt;	// 获取等待队列的poll表指针
	if (end_time && !end_time->tv_sec && !end_time->tv_nsec) {
		wait = NULL;	// 如果超时时间为0，则不设置等待
		timed_out = 1;	// 设置超时标志
	}

	if (end_time && !timed_out)	// 如果设置了超时时间并且未超时
		slack = estimate_accuracy(end_time);	// 估算时间精度

	retval = 0;	// 初始化返回值
	for (;;) {	// 无限循环，直到遇到break语句
		unsigned long *rinp, *routp, *rexp, *inp, *outp, *exp;
		inp = fds->in; outp = fds->out; exp = fds->ex;  // 指向用户提供的输入、输出和异常文件描述符集合
		rinp = fds->res_in; routp = fds->res_out; rexp = fds->res_ex;  // 指向结果集合，将在此存储有事件的文件描述符

		for (i = 0; i < n; ++rinp, ++routp, ++rexp) {  // 遍历所有文件描述符
			unsigned long in, out, ex, all_bits, bit = 1, mask, j;
			unsigned long res_in = 0, res_out = 0, res_ex = 0;  // 初始化结果集的位掩码
			const struct file_operations *f_op = NULL;
			struct file *file = NULL;

			in = *inp++; out = *outp++; ex = *exp++;  // 获取当前描述符的输入、输出、异常请求
			all_bits = in | out | ex;  // 合并所有请求，用于快速判断是否有任何请求
			if (all_bits == 0) {  // 如果当前位没有任何请求
				i += __NFDBITS;  // 跳过一个字的处理，优化性能
				continue;
			}

			for (j = 0; j < __NFDBITS; ++j, ++i, bit <<= 1) {  // 循环遍历每个位
				int fput_needed;  // 用于跟踪文件引用计数的变量
				if (i >= n)  // 如果索引超出文件描述符的数量，终止循环
					break;
				if (!(bit & all_bits))  // 如果当前位没有设置任何事件，继续下一位
					continue;
				file = fget_light(i, &fput_needed);  // 获取文件描述符对应的文件结构
				if (file) {  // 如果文件存在
					f_op = file->f_op;  // 获取文件操作指针
					mask = DEFAULT_POLLMASK;  // 设置默认的事件掩码
					if (f_op && f_op->poll) {  // 如果定义了poll方法
						wait_key_set(wait, in, out, bit);  // 设置poll表中的键值
						mask = (*f_op->poll)(file, wait);  // 调用poll方法
					}
				fput_light(file, fput_needed);  // 释放文件结构
					if ((mask & POLLIN_SET) && (in & bit)) {  // 检查是否有读事件
						res_in |= bit;  // 设置结果中的读位
						retval++;  // 增加返回的计数
						wait = NULL;  // 清除等待指针
					}
					if ((mask & POLLOUT_SET) && (out & bit)) {  // 检查是否有写事件
						res_out |= bit;  // 设置结果中的写位
						retval++;  // 增加返回的计数
						wait = NULL;  // 清除等待指针
					}
					if ((mask & POLLEX_SET) && (ex & bit)) {  // 检查是否有异常事件
						res_ex |= bit;  // 设置结果中的异常位
						retval++;  // 增加返回的计数
						wait = NULL;  // 清除等待指针
					}
				}
			}
			if (res_in)
				*rinp = res_in;	// 如果有读事件发生，更新结果集中的读位
			if (res_out)
				*routp = res_out;	// 如果有写事件发生，更新结果集中的写位
			if (res_ex)
				*rexp = res_ex;		// 如果有异常事件发生，更新结果集中的异常位
			cond_resched();		// 让出CPU，给其他进程运行机会，以避免长时间占用CPU
		}
		wait = NULL;	// 清空等待队列指针
		if (retval || timed_out || signal_pending(current))	// 检查退出条件
			break;	// 如果已有事件返回，或超时，或有信号等待处理，则终止循环
		if (table.error) {
			retval = table.error;	// 如果在处理过程中出现错误，设置返回值
			break;	// 并终止循环
		}

		/*
		 * If this is the first loop and we have a timeout
		 * given, then we convert to ktime_t and set the to
		 * pointer to the expiry value.
		 */
		// 如果这是第一次循环，并且指定了超时时间
		if (end_time && !to) {
			expire = timespec_to_ktime(*end_time);	// 将 timespec 结构转换为 ktime_t 结构
			to = &expire;	// 设置超时指针
		}

		// 如果尚未超时，调度一个超时，状态为可中断
		if (!poll_schedule_timeout(&table, TASK_INTERRUPTIBLE,
					   to, slack))
			timed_out = 1;	// 如果返回0，表示已超时
	}

	poll_freewait(&table);	// 清理等待队列，释放资源

	return retval;		// 返回结果
}

/*
 * We can actually return ERESTARTSYS instead of EINTR, but I'd
 * like to be certain this leads to no problems. So I return
 * EINTR just for safety.
 *
 * Update: ERESTARTSYS breaks at least the xview clock binary, so
 * I'm trying ERESTARTNOHAND which restart only when you want to.
 */
/*
 * 我们实际上可以返回 ERESTARTSYS 而不是 EINTR，但我想确保这不会导致问题。
 * 因此，为了安全起见，我返回 EINTR。
 *
 * 更新：ERESTARTSYS 至少会破坏 xview 时钟二进制文件，
 * 所以我尝试使用 ERESTARTNOHAND，它只在你希望的时候重启。
 */
// 定义最大 select 等待秒数。
// MAX_SCHEDULE_TIMEOUT 是调度器的最大超时时间，通常为无限。
// HZ 表示每秒的时钟滴答数。此宏的计算结果是将最大调度超时时间转换为秒，
// 然后减去一秒，以防止整数溢出或其他边界条件问题。
#define MAX_SELECT_SECONDS \
	((unsigned long) (MAX_SCHEDULE_TIMEOUT / HZ)-1)

int core_sys_select(int n, fd_set __user *inp, fd_set __user *outp,
			   fd_set __user *exp, struct timespec *end_time)
{
	fd_set_bits fds; // 定义用于存储文件描述符状态的结构体
	void *bits; // 通用指针，用于指向文件描述符状态数据
	int ret, max_fds; // ret用于存储函数返回值，max_fds用于存储最大文件描述符数
	unsigned int size; // 用于计算需要的内存大小
	struct fdtable *fdt; // 文件描述符表的指针
	/* Allocate small arguments on the stack to save memory and be faster */
	/* 在栈上分配小型参数数组，以节省内存并提高速度 */
	long stack_fds[SELECT_STACK_ALLOC/sizeof(long)];	// 在栈上分配小的参数空间以提高效率和节省内存

	ret = -EINVAL;	// 初始化返回值为无效参数错误
	if (n < 0)
		goto out_nofds;	// 如果传入的文件描述符数量小于0，跳转到错误处理

	/* max_fds can increase, so grab it once to avoid race */
	/* 为避免竞态条件，获取一次最大文件描述符数 */
	rcu_read_lock();  // 读取锁定，以保护下面对文件描述符表的访问
	fdt = files_fdtable(current->files);  // 获取当前进程的文件描述符表
	max_fds = fdt->max_fds;  // 获取文件描述符的最大值
	rcu_read_unlock();  // 释放读取锁
	if (n > max_fds)
		n = max_fds;	// 如果传入的文件描述符数量超过最大值，调整为最大值

	/*
	 * We need 6 bitmaps (in/out/ex for both incoming and outgoing),
	 * since we used fdset we need to allocate memory in units of
	 * long-words. 
	 */
	/*
	 * 需要六个位图（输入/输出/异常的传入和传出），
	 * 由于我们使用fdset，需要按long-word的单位分配内存。
	 */
	size = FDS_BYTES(n);	// 计算需要的字节数
	bits = stack_fds;	// 默认使用栈上的空间
	if (size > sizeof(stack_fds) / 6) {	// 如果栈空间不够
		/* Not enough space in on-stack array; must use kmalloc */
		/* 栈上数组空间不足，必须使用kmalloc分配 */
		ret = -ENOMEM;	// 设置返回值为内存不足
		bits = kmalloc(6 * size, GFP_KERNEL);	// 使用内核内存分配方式分配空间
		if (!bits)
			goto out_nofds;	// 如果分配失败，处理错误
	}
	// 为文件描述符的输入、输出和异常设置位图数组指针
	fds.in      = bits;
	fds.out     = bits +   size;
	fds.ex      = bits + 2*size;
	fds.res_in  = bits + 3*size;
	fds.res_out = bits + 4*size;
	fds.res_ex  = bits + 5*size;

	// 从用户空间获取文件描述符集，如果出错则跳转到 out 标签
	if ((ret = get_fd_set(n, inp, fds.in)) ||
	    (ret = get_fd_set(n, outp, fds.out)) ||
	    (ret = get_fd_set(n, exp, fds.ex)))
		goto out;
	// 清零结果文件描述符集
	zero_fd_set(n, fds.res_in);
	zero_fd_set(n, fds.res_out);
	zero_fd_set(n, fds.res_ex);

	// 调用 do_select 来实际处理 select 操作
	ret = do_select(n, &fds, end_time);

	// 如果 do_select 返回错误，则跳转到 out 标签处理
	if (ret < 0)
		goto out;
	// 如果没有文件描述符准备好，处理潜在的信号和错误情况
	if (!ret) {
		// 表示不自动重启系统调用
		ret = -ERESTARTNOHAND;	// 如果没有事件发生，可能需要重新启动系统调用
		if (signal_pending(current))	// 检查是否有待处理信号
			goto out;
		ret = 0;
	}

	// 将结果文件描述符集复制回用户空间，如果出错设置返回值为 -EFAULT
	if (set_fd_set(n, inp, fds.res_in) ||
	    set_fd_set(n, outp, fds.res_out) ||
	    set_fd_set(n, exp, fds.res_ex))
		ret = -EFAULT;	// 如果复制回用户空间失败，则返回错误

out:
	if (bits != stack_fds)
		kfree(bits);	// 如果是从堆上分配的空间，需要释放
out_nofds:
	return ret;		// 返回结果
}

// 定义 select 系统调用，参数分别是文件描述符数量，以及指向读、写、异常描述符集的指针和一个指向时间间隔的指针
SYSCALL_DEFINE5(select, int, n, fd_set __user *, inp, fd_set __user *, outp,
		fd_set __user *, exp, struct timeval __user *, tvp)
{
	struct timespec end_time, *to = NULL;  // 定义 timespec 结构体用于精确的时间操作
	struct timeval tv;  // 用于从用户空间获取的时间间隔
	int ret;

	// 如果提供了时间间隔参数
	if (tvp) {
		if (copy_from_user(&tv, tvp, sizeof(tv)))  // 从用户空间复制 timeval 结构到内核空间
			return -EFAULT;  // 如果复制失败，返回错误

		to = &end_time;  // 设置指向 timespec 结构体的指针
		// 设置超时，将 timeval 转换为 timespec，并处理秒和纳秒
		if (poll_select_set_timeout(to,
				tv.tv_sec + (tv.tv_usec / USEC_PER_SEC),
				(tv.tv_usec % USEC_PER_SEC) * NSEC_PER_USEC))
			return -EINVAL;  // 如果设置超时失败，返回错误
	}

	// 调用核心的 select 处理函数
	ret = core_sys_select(n, inp, outp, exp, to);
	// 尝试更新用户空间的时间结构，以表明剩余的时间（如果 select 提前返回）
	ret = poll_select_copy_remaining(&end_time, tvp, 1, ret);

	return ret;  // 返回结果
}

#ifdef HAVE_SET_RESTORE_SIGMASK
static long do_pselect(int n, fd_set __user *inp, fd_set __user *outp,
		       fd_set __user *exp, struct timespec __user *tsp,
		       const sigset_t __user *sigmask, size_t sigsetsize)
{
	sigset_t ksigmask, sigsaved;  // 定义内核空间的信号集合和保存的信号集合
	struct timespec ts, end_time, *to = NULL;  // 时间结构体，用于处理超时
	int ret;

	if (tsp) {
		if (copy_from_user(&ts, tsp, sizeof(ts)))  // 从用户空间复制时间结构到内核空间
			return -EFAULT;  // 复制失败返回错误

		to = &end_time;  // 设置超时时间指针
		if (poll_select_set_timeout(to, ts.tv_sec, ts.tv_nsec))  // 设置超时时间
			return -EINVAL;  // 设置失败返回错误
	}

	if (sigmask) {	// 如果提供了信号掩码
		/* XXX: Don't preclude handling different sized sigset_t's.  */
		if (sigsetsize != sizeof(sigset_t))	// 检查信号集大小是否正确
			return -EINVAL;	// 返回错误
		if (copy_from_user(&ksigmask, sigmask, sizeof(ksigmask)))	// 从用户空间复制信号掩码到内核空间
			return -EFAULT;	// 复制失败返回错误

		sigdelsetmask(&ksigmask, sigmask(SIGKILL)|sigmask(SIGSTOP));	// 从信号掩码中删除SIGKILL和SIGSTOP	
		sigprocmask(SIG_SETMASK, &ksigmask, &sigsaved);	// 设置新的信号掩码，并保存当前信号掩码
	}

	ret = core_sys_select(n, inp, outp, exp, to);	// 调用核心 select 函数
	ret = poll_select_copy_remaining(&end_time, tsp, 0, ret);	// 更新用户空间的时间结构体，如果需要的话

	if (ret == -ERESTARTNOHAND) {	// 如果需要重新启动，但不处理信号
		/*
		 * Don't restore the signal mask yet. Let do_signal() deliver
		 * the signal on the way back to userspace, before the signal
		 * mask is restored.
		 */
		if (sigmask) {
			memcpy(&current->saved_sigmask, &sigsaved,
					sizeof(sigsaved));	// 保存新的信号掩码，用于在返回用户空间前恢复
			set_restore_sigmask();	// 标记在返回用户空间时需要恢复信号掩码
		}
	} else if (sigmask)
		sigprocmask(SIG_SETMASK, &sigsaved, NULL);	// 恢复之前的信号掩码

	return ret;	// 返回结果
}

/*
 * Most architectures can't handle 7-argument syscalls. So we provide a
 * 6-argument version where the sixth argument is a pointer to a structure
 * which has a pointer to the sigset_t itself followed by a size_t containing
 * the sigset size.
 */
/*
 * 大多数架构无法处理7个参数的系统调用。因此，我们提供了一个6个参数的版本，
 * 其中第六个参数是一个指向结构体的指针，该结构体包含了指向 sigset_t 本身的指针
 * 以及一个包含 sigset 大小的 size_t。
 */
SYSCALL_DEFINE6(pselect6, int, n, fd_set __user *, inp, fd_set __user *, outp,
		fd_set __user *, exp, struct timespec __user *, tsp,
		void __user *, sig)
{
	size_t sigsetsize = 0;  // 信号集大小
	sigset_t __user *up = NULL;  // 用户空间的信号集指针

	if (sig) {  // 如果提供了sig参数
		if (!access_ok(VERIFY_READ, sig, sizeof(void *)+sizeof(size_t))
		    || __get_user(up, (sigset_t __user * __user *)sig)  // 从用户空间获取信号集指针
		    || __get_user(sigsetsize,  // 获取信号集大小
				(size_t __user *)(sig+sizeof(void *))))
			return -EFAULT;  // 如果访问出错，返回错误
	}

	return do_pselect(n, inp, outp, exp, tsp, up, sigsetsize);  // 调用do_pselect处理具体逻辑
}
#endif /* HAVE_SET_RESTORE_SIGMASK */

#ifdef __ARCH_WANT_SYS_OLD_SELECT
// 定义一个结构体，用于传递参数到旧的 select 系统调用
struct sel_arg_struct {
	unsigned long n;  // 要监视的文件描述符数量
	fd_set __user *inp, *outp, *exp;  // 指向输入、输出和异常监视文件描述符集的指针
	struct timeval __user *tvp;  // 指向时间间隔结构体的指针
};

// 定义 old_select 系统调用，参数是指向 sel_arg_struct 结构的指针
SYSCALL_DEFINE1(old_select, struct sel_arg_struct __user *, arg)
{
	struct sel_arg_struct a;  // 在栈上分配一个 sel_arg_struct 结构体变量

	if (copy_from_user(&a, arg, sizeof(a)))  // 从用户空间复制数据到内核空间
		return -EFAULT;  // 如果复制失败，返回错误码 EFAULT

	return sys_select(a.n, a.inp, a.outp, a.exp, a.tvp);  // 调用 sys_select 函数处理 select 逻辑
}
#endif

// 定义 poll_list 结构，用于维护一个链表，链表中的每个节点可以存储一定数量的 pollfd 结构。
struct poll_list {
	struct poll_list *next;  // 指向下一个 poll_list 的指针
	int len;  // 当前 poll_list 中的 pollfd 数量
	struct pollfd entries[0];  // 柔性数组，实际存储 pollfd 结构
};


#define POLLFD_PER_PAGE  ((PAGE_SIZE-sizeof(struct poll_list)) / sizeof(struct pollfd))

/*
 * Fish for pollable events on the pollfd->fd file descriptor. We're only
 * interested in events matching the pollfd->events mask, and the result
 * matching that mask is both recorded in pollfd->revents and returned. The
 * pwait poll_table will be used by the fd-provided poll handler for waiting,
 * if non-NULL.
 */
/*
 * 处理 pollfd->fd 文件描述符上的可轮询事件。我们只对匹配 pollfd->events 掩码的事件感兴趣，
 * 匹配该掩码的结果将被记录在 pollfd->revents 中并返回。如果 pwait 非空，则 poll_table 将被
 * 用于等待，由文件描述符提供的 poll 处理程序。
 */
static inline unsigned int do_pollfd(struct pollfd *pollfd, poll_table *pwait)
{
	unsigned int mask;  // 用于存储事件掩码
	int fd;  // 文件描述符

	mask = 0;
	fd = pollfd->fd;
	if (fd >= 0) {  // 检查文件描述符是否有效
		int fput_needed;  // 用于检查是否需要执行 fput 操作
		struct file * file;  // 文件指针

		file = fget_light(fd, &fput_needed);  // 尝试获取文件描述符指向的文件
		mask = POLLNVAL;  // 默认将事件掩码设置为无效（POLLNVAL）
		if (file != NULL) {  // 如果成功获取文件
			mask = DEFAULT_POLLMASK;  // 重置掩码为默认的事件掩码
			if (file->f_op && file->f_op->poll) {  // 检查文件操作是否存在且包含 poll 方法
				if (pwait)  // 如果提供了等待队列
					pwait->key = pollfd->events | POLLERR | POLLHUP;  // 设置等待队列的事件掩码
				mask = file->f_op->poll(file, pwait);  // 调用 poll 方法获取事件掩码
			}
			/* Mask out unneeded events. */
			/* 掩蔽不需要的事件 */
			mask &= pollfd->events | POLLERR | POLLHUP;  // 与用户请求的事件进行与操作，只保留感兴趣的事件
			fput_light(file, fput_needed);  // 如果需要，释放文件
		}
	}
	pollfd->revents = mask;  // 设置 pollfd 的返回事件

	return mask;  // 返回事件掩码
}

static int do_poll(unsigned int nfds,  struct poll_list *list,
		   struct poll_wqueues *wait, struct timespec *end_time)
{
	poll_table* pt = &wait->pt; // 指向等待队列的 poll_table 结构
	ktime_t expire, *to = NULL; // 用于处理超时
	int timed_out = 0, count = 0; // timed_out 指示是否超时，count 记录检测到的事件数量
	unsigned long slack = 0; // 用于调整超时精度

	/* Optimise the no-wait case */
	/* 优化无等待情况 */
	if (end_time && !end_time->tv_sec && !end_time->tv_nsec) {
		pt = NULL; // 如果指定的超时时间为零，不需要等待队列
		timed_out = 1; // 直接设置为超时
	}

	if (end_time && !timed_out)
		slack = estimate_accuracy(end_time); // 计算超时的精度

	for (;;) {	// 循环处理所有 pollfd 结构
		struct poll_list *walk;

		for (walk = list; walk != NULL; walk = walk->next) {	// 遍历 poll_list 链表
			struct pollfd * pfd, * pfd_end;

			pfd = walk->entries;	// 当前 poll_list 的起始 pollfd
			pfd_end = pfd + walk->len;	// 当前 poll_list 的结束 pollfd
			for (; pfd != pfd_end; pfd++) {
				/*
				 * Fish for events. If we found one, record it
				 * and kill the poll_table, so we don't
				 * needlessly register any other waiters after
				 * this. They'll get immediately deregistered
				 * when we break out and return.
				 */
				/*
				 * 查找事件。如果找到事件，记录它，并且清除 poll_table，
				 * 这样之后不再无谓地注册其他等待者。一旦返回，会立即注销所有等待者。
				 */
				if (do_pollfd(pfd, pt)) {
					count++;	// 增加找到的事件数量
					pt = NULL;	// 清除 poll_table，避免再次注册
				}
			}
		}
		/*
		 * All waiters have already been registered, so don't provide
		 * a poll_table to them on the next loop iteration.
		 */
		// 所有等待者已经注册完毕，下次循环不再提供 poll_table
		pt = NULL;
		if (!count) { // 如果没有检测到事件
			count = wait->error; // 检查是否有错误发生
			if (signal_pending(current))
				count = -EINTR; // 如果有信号发生，返回 -EINTR
		}
		if (count || timed_out) // 如果检测到事件或超时，结束循环
			break;

		/*
		 * If this is the first loop and we have a timeout
		 * given, then we convert to ktime_t and set the to
		 * pointer to the expiry value.
		 */
		/*
		 * 如果这是第一次循环并且有超时设置，那么转换为 ktime_t 并设置超时指针。
		 */
		if (end_time && !to) {
			expire = timespec_to_ktime(*end_time); // 将 timespec 转为 ktime_t
			to = &expire; // 设置超时指针
		}

		if (!poll_schedule_timeout(wait, TASK_INTERRUPTIBLE, to, slack))
			timed_out = 1; // 调度超时，如果超时，设置 timed_out 标志
	}
	return count; // 返回检测到的事件数量
}

// 定义在栈上能够存放的pollfd数量
#define N_STACK_PPS ((sizeof(stack_pps) - sizeof(struct poll_list))  / \
			sizeof(struct pollfd))

// 定义系统调用do_sys_poll
int do_sys_poll(struct pollfd __user *ufds, unsigned int nfds,
		struct timespec *end_time)
{
	struct poll_wqueues table;	// 定义等待队列表
 	int err = -EFAULT, fdcount, len, size;	// 初始化错误代码为-EFAULT，定义文件描述符计数、长度和大小变量
	/* Allocate small arguments on the stack to save memory and be
	   faster - use long to make sure the buffer is aligned properly
	   on 64 bit archs to avoid unaligned access */
	// 在栈上分配临时存储空间以节省内存并提高性能，使用长整型确保在64位架构上缓冲区对齐，避免未对齐访问
	long stack_pps[POLL_STACK_ALLOC/sizeof(long)];	// 在栈上分配临时空间，以便快速访问，避免使用kmalloc
	struct poll_list *const head = (struct poll_list *)stack_pps;	// 使用栈空间的开始部分作为poll_list的头部
 	struct poll_list *walk = head;	// walk用于遍历或构建poll_list链表
 	unsigned long todo = nfds;	// 还需处理的文件描述符数量

	if (nfds > rlimit(RLIMIT_NOFILE))	// 如果文件描述符数量超过了系统限制
		return -EINVAL;	// 返回无效参数错误

	len = min_t(unsigned int, nfds, N_STACK_PPS);	// 计算一次性可以处理的最大文件描述符数量，取nfds和N_STACK_PPS的较小值
	for (;;) {	// 不断循环直到所有的文件描述符都被处理
		walk->next = NULL;	// 初始化当前poll_list节点的next指针为NULL
		walk->len = len;	// 设置当前节点的长度为len
		if (!len)	// 如果长度为0，即没有文件描述符需要处理，则跳出循环
			break;	// 如果没有需要处理的文件描述符，则终止循环

		if (copy_from_user(walk->entries, ufds + nfds-todo,
					sizeof(struct pollfd) * walk->len))
			goto out_fds;	// 从用户空间复制pollfd到内核空间，如果失败则跳转到错误处理

		todo -= walk->len;	// 从待处理的文件描述符数量中减去当前已处理的数量
		if (!todo)
			break;		// 如果所有文件描述符都已处理，则退出循环

		len = min(todo, POLLFD_PER_PAGE); // 计算下一个节点可以处理的文件描述符数量
		size = sizeof(struct poll_list) + sizeof(struct pollfd) * len; // 计算需要分配的空间大小
		walk = walk->next = kmalloc(size, GFP_KERNEL); // 为下一个节点分配内存，并将当前节点的next指针指向它
		if (!walk) {	// 如果内存分配失败
			err = -ENOMEM;	// 设置错误码为-ENOMEM
			goto out_fds;	// 跳转到错误处理代码
		}
	}

	poll_initwait(&table);	// 初始化轮询队列，准备接收等待事件
	fdcount = do_poll(nfds, head, &table, end_time);	// 执行poll操作，检查文件描述符状态
	poll_freewait(&table);	// 清理和释放轮询队列中的资源

	for (walk = head; walk; walk = walk->next) {	// 遍历所有的poll_list，这些poll_list包含了文件描述符的信息
		struct pollfd *fds = walk->entries;	// 获取当前poll_list节点中的文件描述符数组
		int j;

		for (j = 0; j < walk->len; j++, ufds++)	// 遍历当前节点的每一个文件描述符
			if (__put_user(fds[j].revents, &ufds->revents))	// 将结果写回用户空间，如果写入失败则跳转到错误处理
				goto out_fds;	// 将结果复制回用户空间，如果失败则处理错误
  	}

	err = fdcount;	// 返回轮询结果，即有多少文件描述符上发生了预期的事件
out_fds:
	walk = head->next;	// 开始清理分配的poll_list链表
	while (walk) {	// 清理分配的内存
		struct poll_list *pos = walk;
		walk = walk->next;	// 遍历并释放所有除头节点外的poll_list节点
		kfree(pos);
	}

	return err;	// 返回轮询结果或错误代码
}

static long do_restart_poll(struct restart_block *restart_block)
{
	struct pollfd __user *ufds = restart_block->poll.ufds;  // 用户空间的文件描述符数组
	int nfds = restart_block->poll.nfds;  // 文件描述符数量
	struct timespec *to = NULL, end_time;  // 指向时间结构的指针和时间结构，用于设定超时
	int ret;

	if (restart_block->poll.has_timeout) {  // 检查是否设置了超时
		end_time.tv_sec = restart_block->poll.tv_sec;  // 设置超时秒
		end_time.tv_nsec = restart_block->poll.tv_nsec;  // 设置超时纳秒
		to = &end_time;  // 将超时时间结构的指针赋给to
	}

	ret = do_sys_poll(ufds, nfds, to);  // 执行系统轮询调用

	if (ret == -EINTR) {  // 如果返回值是因为中断（信号）
		restart_block->fn = do_restart_poll;  // 设置重启块的函数指针为do_restart_poll
		ret = -ERESTART_RESTARTBLOCK;  // 设置返回值为重启块重新启动
	}
	return ret;  // 返回结果
}

SYSCALL_DEFINE3(poll, struct pollfd __user *, ufds, unsigned int, nfds,
		long, timeout_msecs)
{
	struct timespec end_time, *to = NULL;  // 结束时间和一个可能为空的指针to，用于指向结束时间
	int ret;  // 用于存储函数返回值

	// 如果timeout_msecs非负，则设置超时时间
	if (timeout_msecs >= 0) {
		to = &end_time;  // 设置to指针指向end_time
		// 计算超时的秒数和纳秒数，并设置到to指向的timespec结构中
		poll_select_set_timeout(to, timeout_msecs / MSEC_PER_SEC,
			NSEC_PER_MSEC * (timeout_msecs % MSEC_PER_SEC));
	}

	// 执行轮询操作，根据文件描述符数组ufds，数量nfds，以及可能的超时时间to
	ret = do_sys_poll(ufds, nfds, to);

	// 如果返回值表示被中断（即返回-EINTR）
	if (ret == -EINTR) {
		struct restart_block *restart_block;

		// 获取当前线程的restart_block结构
		restart_block = &current_thread_info()->restart_block;
		// 设置重启操作的函数为do_restart_poll
		restart_block->fn = do_restart_poll;
		// 设置重启操作中的文件描述符数组和数量
		restart_block->poll.ufds = ufds;
		restart_block->poll.nfds = nfds;

		// 如果设置了超时，也将超时值存入restart_block
		if (timeout_msecs >= 0) {
			restart_block->poll.tv_sec = end_time.tv_sec;
			restart_block->poll.tv_nsec = end_time.tv_nsec;
			restart_block->poll.has_timeout = 1;
		} else {
			restart_block->poll.has_timeout = 0;
		}

		// 设置返回值为-ERESTART_RESTARTBLOCK，通知内核需要重新执行系统调用
		ret = -ERESTART_RESTARTBLOCK;
	}
	return ret;  // 返回最终结果
}

#ifdef HAVE_SET_RESTORE_SIGMASK
SYSCALL_DEFINE5(ppoll, struct pollfd __user *, ufds, unsigned int, nfds,
		struct timespec __user *, tsp, const sigset_t __user *, sigmask,
		size_t, sigsetsize)
{
	sigset_t ksigmask, sigsaved;  // 内核中的信号集和保存的旧信号集
	struct timespec ts, end_time, *to = NULL;  // 用于处理超时
	int ret;  // 函数返回值

	if (tsp) {
		// 如果提供了时间戳，从用户空间复制时间戳到内核空间
		if (copy_from_user(&ts, tsp, sizeof(ts)))
			return -EFAULT;  // 复制失败返回错误

		to = &end_time;  // 设置超时时间的指针
		if (poll_select_set_timeout(to, ts.tv_sec, ts.tv_nsec))  // 设置超时
			return -EINVAL;  // 如果时间设置不合法，则返回错误
	}

	// 如果提供了信号掩码
	if (sigmask) {
		/* XXX: Don't preclude handling different sized sigset_t's.  */
		/* XXX: 不要排除处理不同大小的 sigset_t。 */
		if (sigsetsize != sizeof(sigset_t))
			return -EINVAL;	// 信号集大小不正确，返回错误
		if (copy_from_user(&ksigmask, sigmask, sizeof(ksigmask)))
			return -EFAULT;	// 从用户空间复制信号掩码失败，返回错误

		// 删除SIGKILL和SIGSTOP信号	
		sigdelsetmask(&ksigmask, sigmask(SIGKILL)|sigmask(SIGSTOP));
		sigprocmask(SIG_SETMASK, &ksigmask, &sigsaved);	// 设置新的信号掩码，并保存旧的信号掩码
	}

	ret = do_sys_poll(ufds, nfds, to);	// 执行系统轮询调用

	/* We can restart this syscall, usually */
	/* 通常，我们可以重启这个系统调用。 */
	if (ret == -EINTR) {
		/*
		 * Don't restore the signal mask yet. Let do_signal() deliver
		 * the signal on the way back to userspace, before the signal
		 * mask is restored.
		 */
		/*
		 * 不要立即恢复信号掩码。让 do_signal() 在返回用户空间的过程中传递信号，
		 * 然后再恢复信号掩码。
		 */
		// 如果系统调用因为中断被打断
		if (sigmask) {
			memcpy(&current->saved_sigmask, &sigsaved,
					sizeof(sigsaved));	// 保存当前的信号掩码
			set_restore_sigmask();	// 设置恢复信号掩码的标志
		}
		ret = -ERESTARTNOHAND;	// 设置系统调用重启标志
	} else if (sigmask)
		sigprocmask(SIG_SETMASK, &sigsaved, NULL);	// 恢复旧的信号掩码

	ret = poll_select_copy_remaining(&end_time, tsp, 0, ret);	// 处理剩余的超时时间

	return ret;	// 返回结果
}
#endif /* HAVE_SET_RESTORE_SIGMASK */
