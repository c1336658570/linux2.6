
#include <linux/wait.h>
#include <linux/backing-dev.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/writeback.h>
#include <linux/device.h>

// 初始化原子变量 bdi_seq，通常用于跟踪或版本控制
static atomic_long_t bdi_seq = ATOMIC_LONG_INIT(0);

// 默认的 IO 解除阻塞函数，不执行任何操作
void default_unplug_io_fn(struct backing_dev_info *bdi, struct page *page)
{
}
EXPORT_SYMBOL(default_unplug_io_fn);

// 定义默认的后台设备信息结构体
struct backing_dev_info default_backing_dev_info = {
	.name		= "default",  // 名称为"default"
	.ra_pages	= VM_MAX_READAHEAD * 1024 / PAGE_CACHE_SIZE,  // 预读页数，根据系统设置计算得出
	.state		= 0,  // 初始状态为0
	.capabilities	= BDI_CAP_MAP_COPY,  // 能力包括支持映射复制
	.unplug_io_fn	= default_unplug_io_fn,  // 使用默认的解除阻塞函数
};
EXPORT_SYMBOL_GPL(default_backing_dev_info);

// 定义一个不执行任何操作的后台设备信息结构体
struct backing_dev_info noop_backing_dev_info = {
	.name		= "noop",	// 名称为"noop"
};
EXPORT_SYMBOL_GPL(noop_backing_dev_info);

// 定义后台设备信息的类别
static struct class *bdi_class;

/*
 * bdi_lock protects updates to bdi_list and bdi_pending_list, as well as
 * reader side protection for bdi_pending_list. bdi_list has RCU reader side
 * locking.
 */
/*
 * bdi_lock 用于保护 bdi_list 和 bdi_pending_list 的更新，
 * 以及 bdi_pending_list 的读取端保护。bdi_list 使用 RCU 读取端锁定。
 */
// 定义一个自旋锁以保护对后台设备列表的操作
DEFINE_SPINLOCK(bdi_lock);
LIST_HEAD(bdi_list);	// 定义并初始化后台设备信息列表
// 定义并初始化等待处理的后台设备信息列表(有任务需要写会的链表)
LIST_HEAD(bdi_pending_list);

// 用于同步超级块的任务结构
static struct task_struct *sync_supers_tsk;
// 定时器，用于定时同步超级块
static struct timer_list sync_supers_timer;

// 声明同步超级块的函数
static int bdi_sync_supers(void *);
// 定时器函数，用于调用同步超级块的操作
static void sync_supers_timer_fn(unsigned long);
// 设置定时器的函数
static void arm_supers_timer(void);

// 向后台设备信息添加默认的刷新器任务
static void bdi_add_default_flusher_task(struct backing_dev_info *bdi);

// 在内核配置中启用了调试文件系统时
#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>

// 调试文件系统中后台设备信息的根目录入口
static struct dentry *bdi_debug_root;

static void bdi_debug_init(void)
{
	// 在debugfs中创建一个名为"bdi"的目录
	bdi_debug_root = debugfs_create_dir("bdi", NULL);
}

static int bdi_debug_stats_show(struct seq_file *m, void *v)
{
	struct backing_dev_info *bdi = m->private;  // 从私有数据获取backing_dev_info结构
	struct bdi_writeback *wb;  // 用于迭代writeback结构的指针
	unsigned long background_thresh;
	unsigned long dirty_thresh;
	unsigned long bdi_thresh;
	unsigned long nr_dirty, nr_io, nr_more_io, nr_wb;  // 用于统计的变量
	struct inode *inode;  // 用于迭代inode列表的指针

	/*
	 * inode lock is enough here, the bdi->wb_list is protected by
	 * RCU on the reader side
	 */
	// 使用inode锁进行保护，因为bdi->wb_list是在读取端用RCU保护的
	nr_wb = nr_dirty = nr_io = nr_more_io = 0;	// 初始化统计变量
	spin_lock(&inode_lock);		// 锁定inode锁
	// 遍历writeback列表
	list_for_each_entry(wb, &bdi->wb_list, list) {
		nr_wb++;
		// 遍历标记为脏的inode列表
		list_for_each_entry(inode, &wb->b_dirty, i_list)
			nr_dirty++;
		// 遍历I/O inode列表
		list_for_each_entry(inode, &wb->b_io, i_list)
			nr_io++;
		// 遍历更多I/O inode列表
		list_for_each_entry(inode, &wb->b_more_io, i_list)
			nr_more_io++;
	}
	// 解锁inode锁
	spin_unlock(&inode_lock);

	// 获取脏页阈值
	get_dirty_limits(&background_thresh, &dirty_thresh, &bdi_thresh, bdi);

// 定义宏用于转换页面数为KB
#define K(x) ((x) << (PAGE_SHIFT - 10))
	// 向seq文件输出统计数据
	seq_printf(m,
		   "BdiWriteback:     %8lu kB\n"
		   "BdiReclaimable:   %8lu kB\n"
		   "BdiDirtyThresh:   %8lu kB\n"
		   "DirtyThresh:      %8lu kB\n"
		   "BackgroundThresh: %8lu kB\n"
		   "WritebackThreads: %8lu\n"
		   "b_dirty:          %8lu\n"
		   "b_io:             %8lu\n"
		   "b_more_io:        %8lu\n"
		   "bdi_list:         %8u\n"
		   "state:            %8lx\n"
		   "wb_mask:          %8lx\n"
		   "wb_list:          %8u\n"
		   "wb_cnt:           %8u\n",
		   (unsigned long) K(bdi_stat(bdi, BDI_WRITEBACK)),
		   (unsigned long) K(bdi_stat(bdi, BDI_RECLAIMABLE)),
		   K(bdi_thresh), K(dirty_thresh),
		   K(background_thresh), nr_wb, nr_dirty, nr_io, nr_more_io,
		   !list_empty(&bdi->bdi_list), bdi->state, bdi->wb_mask,
		   !list_empty(&bdi->wb_list), bdi->wb_cnt);
#undef K	// 取消定义宏

	return 0;	// 返回成功
}

// 打开调试状态文件的函数，设置单个打开文件的处理方式
static int bdi_debug_stats_open(struct inode *inode, struct file *file)
{
	// 以单一打开模式打开文件
	return single_open(file, bdi_debug_stats_show, inode->i_private);
}

// 文件操作结构，定义了文件的打开、读取、定位和释放操作
static const struct file_operations bdi_debug_stats_fops = {
	.open		= bdi_debug_stats_open,  // 文件打开函数
	.read		= seq_read,              // 读操作
	.llseek		= seq_lseek,             // 文件定位操作
	.release	= single_release,        // 文件释放函数
};

// 注册后台设备信息的调试目录和文件
static void bdi_debug_register(struct backing_dev_info *bdi, const char *name)
{
	// 在调试文件系统中为bdi创建目录
	bdi->debug_dir = debugfs_create_dir(name, bdi_debug_root);
	// 在bdi目录下创建统计信息文件
	bdi->debug_stats = debugfs_create_file("stats", 0444, bdi->debug_dir,
					       bdi, &bdi_debug_stats_fops);
}

// 注销后台设备信息的调试目录和文件
static void bdi_debug_unregister(struct backing_dev_info *bdi)
{
	debugfs_remove(bdi->debug_stats);	// 移除统计信息文件
	debugfs_remove(bdi->debug_dir);		// 移除bdi目录
}
#else
// 如果没有启用调试文件系统，则定义空的初始化和注册/注销函数
static inline void bdi_debug_init(void)
{
}
static inline void bdi_debug_register(struct backing_dev_info *bdi,
				      const char *name)
{
}
static inline void bdi_debug_unregister(struct backing_dev_info *bdi)
{
}
#endif

// 用于存储 read_ahead_kb 属性的函数
static ssize_t read_ahead_kb_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	// 从设备中获取后台设备信息
	struct backing_dev_info *bdi = dev_get_drvdata(dev);
	char *end;
	unsigned long read_ahead_kb;
	// 初始化返回值为无效参数
	ssize_t ret = -EINVAL;

	// 将字符串转换为无符号长整数
	read_ahead_kb = simple_strtoul(buf, &end, 10);
	// 检查转换是否成功并确保字符串以空字符或换行符结束
	if (*buf && (end[0] == '\0' || (end[0] == '\n' && end[1] == '\0'))) {
		// 设置bdi的预读页数
		bdi->ra_pages = read_ahead_kb >> (PAGE_SHIFT - 10);
		ret = count;	// 成功时返回写入的字符数
	}
	return ret;	// 返回结果
}

// 定义一个辅助宏，将页数转换为千字节（K）
#define K(pages) ((pages) << (PAGE_SHIFT - 10))

// 生成一个展示函数，该函数返回设备的某个属性值
#define BDI_SHOW(name, expr)						\
static ssize_t name##_show(struct device *dev,				\
			   struct device_attribute *attr, char *page)	\
{									\
	struct backing_dev_info *bdi = dev_get_drvdata(dev);		\
									\
	return snprintf(page, PAGE_SIZE-1, "%lld\n", (long long)expr);	\
}

// 使用BDI_SHOW宏为read_ahead_kb属性生成显示函数
BDI_SHOW(read_ahead_kb, K(bdi->ra_pages))

// 用于存储 min_ratio 属性的函数
// 用于存储后台设备信息（backing_dev_info）中的min_ratio属性的函数。
// 这些属性控制后台设备（如文件系统）的某些操作参数，如缓存和写入的比例限制。
static ssize_t min_ratio_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	// 从设备中获取后台设备信息
	struct backing_dev_info *bdi = dev_get_drvdata(dev);
	char *end;
	unsigned int ratio;
	ssize_t ret = -EINVAL;	// 初始错误返回值设为无效参数

	ratio = simple_strtoul(buf, &end, 10);	// 将字符串转换为无符号整数
	// 检查转换是否成功并确保字符串以空字符或换行符结束
	if (*buf && (end[0] == '\0' || (end[0] == '\n' && end[1] == '\0'))) {
		ret = bdi_set_min_ratio(bdi, ratio);	// 尝试设置最小比率
		if (!ret)
			ret = count;	// 若设置成功，返回处理的字节数
	}
	return ret;	// 返回结果
}
// 使用BDI_SHOW宏定义一个显示 min_ratio 的函数
// 用于显示后台设备信息（backing_dev_info）中的min_ratio属性的函数。
// 这些属性控制后台设备（如文件系统）的某些操作参数，如缓存和写入的比例限制。
BDI_SHOW(min_ratio, bdi->min_ratio)

// 存储 max_ratio 属性的函数
// 用于存储后台设备信息（backing_dev_info）中的max_ratio属性的函数。
// 这些属性控制后台设备（如文件系统）的某些操作参数，如缓存和写入的比例限制。
static ssize_t max_ratio_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	// 从设备中获取后台设备信息
	struct backing_dev_info *bdi = dev_get_drvdata(dev);
	char *end;
	unsigned int ratio;
	ssize_t ret = -EINVAL;	// 初始错误返回值设为无效参数

	ratio = simple_strtoul(buf, &end, 10);	// 从buf字符串解析无符号整数
	// 检查转换是否成功且确保转换后的剩余字符串正确
	if (*buf && (end[0] == '\0' || (end[0] == '\n' && end[1] == '\0'))) {
		ret = bdi_set_max_ratio(bdi, ratio);	// 尝试设置最大比率
		if (!ret)
			ret = count;	// 若设置成功，返回处理的字节数
	}
	return ret;	// 返回结果
}
// 使用BDI_SHOW宏定义一个显示 max_ratio 的函数
// 用于显示后台设备信息（backing_dev_info）中的max_ratio属性的函数。
// 这些属性控制后台设备（如文件系统）的某些操作参数，如缓存和写入的比例限制。
BDI_SHOW(max_ratio, bdi->max_ratio)

// 定义宏用于创建设备属性，同时提供显示和存储函数。
#define __ATTR_RW(attr) __ATTR(attr, 0644, attr##_show, attr##_store)

// 定义设备属性数组，包括read_ahead_kb、min_ratio和max_ratio
static struct device_attribute bdi_dev_attrs[] = {
	__ATTR_RW(read_ahead_kb),
	__ATTR_RW(min_ratio),
	__ATTR_RW(max_ratio),
	__ATTR_NULL,	// 标记属性数组的结束
};

// 初始化BDI类的函数，这个类用于管理BDI相关的设备和属性
static __init int bdi_class_init(void)
{
	// 创建一个名为"bdi"的类
	bdi_class = class_create(THIS_MODULE, "bdi");
	// 检查是否创建成功
	if (IS_ERR(bdi_class))
		return PTR_ERR(bdi_class);

	// 将定义的设备属性关联到这个类
	bdi_class->dev_attrs = bdi_dev_attrs;
	bdi_debug_init();	// 调用另一个函数进行调试相关的初始化
	return 0;
}
// 内核启动时调用bdi_class_init()
postcore_initcall(bdi_class_init);

// 默认BDI初始化函数
static int __init default_bdi_init(void)
{
	int err;

	// 创建一个内核线程用于同步supers
	sync_supers_tsk = kthread_run(bdi_sync_supers, NULL, "sync_supers");
	// 检查线程创建是否成功
	BUG_ON(IS_ERR(sync_supers_tsk));

	// 初始化计时器
	init_timer(&sync_supers_timer);
	// 设置计时器功能
	setup_timer(&sync_supers_timer, sync_supers_timer_fn, 0);
	arm_supers_timer();	// 启动计时器

	// 初始化默认的后台设备信息
	err = bdi_init(&default_backing_dev_info);
	if (!err)
		// 注册默认的BDI
		bdi_register(&default_backing_dev_info, NULL, "default");

	return err;	// 返回操作结果
}
// 系统启动时调用default_bdi_init()
subsys_initcall(default_bdi_init);

// 初始化给定的写回结构
static void bdi_wb_init(struct bdi_writeback *wb, struct backing_dev_info *bdi)
{
	// 初始化给定的写回结构
	memset(wb, 0, sizeof(*wb));

	wb->bdi = bdi;	// 设置关联的背景设备信息
	// 设置最后一次老式刷新的时间为当前时间点
	wb->last_old_flush = jiffies;
	// 初始化脏页列表
	INIT_LIST_HEAD(&wb->b_dirty);
	// 初始化 I/O 页列表
	INIT_LIST_HEAD(&wb->b_io);
	// 初始化更多 I/O 页的列表
	INIT_LIST_HEAD(&wb->b_more_io);
}

// 初始化特定的写回任务
static void bdi_task_init(struct backing_dev_info *bdi,
			  struct bdi_writeback *wb)
{
	// 获取当前任务结构
	struct task_struct *tsk = current;

	spin_lock(&bdi->wb_lock);	// 对写回设备加锁
	// 将写回结构添加到设备的写回列表
	list_add_tail_rcu(&wb->list, &bdi->wb_list);
	spin_unlock(&bdi->wb_lock);	// 解锁

	tsk->flags |= PF_FLUSHER | PF_SWAPWRITE;
	set_freezable();	// 标记任务为可冻结

	/*
	 * Our parent may run at a different priority, just set us to normal
	 */
	/*
	 * 我们的父任务可能运行在不同的优先级，这里设置我们的优先级为普通
	 */
	set_user_nice(tsk, 0);	// 设置当前任务的优先级为 0
}

static int bdi_start_fn(void *ptr)
{
	// 从参数获取写回控制结构
	struct bdi_writeback *wb = ptr;
	// 从写回结构获取关联的后台设备信息
	struct backing_dev_info *bdi = wb->bdi;
	int ret;

	/*
	 * Add us to the active bdi_list
	 */
	/*
	 * 将我们添加到活跃的bdi_list中
	 */
	spin_lock_bh(&bdi_lock);	// 获取bdi锁，保护bdi_list
	// 将bdi添加到全局的bdi列表中
	list_add_rcu(&bdi->bdi_list, &bdi_list);
	// 释放锁
	spin_unlock_bh(&bdi_lock);

	bdi_task_init(bdi, wb);	 // 初始化相关的写回任务

	/*
	 * Clear pending bit and wakeup anybody waiting to tear us down
	 */
	/*
	 * 清除pending位并唤醒任何等待拆除我们的线程
	 */
	// 清除BDI_pending状态位
	clear_bit(BDI_pending, &bdi->state);
	smp_mb__after_clear_bit();	// 确保清除操作的内存屏障
	// 唤醒等待BDI_pending位的线程
	wake_up_bit(&bdi->state, BDI_pending);

	ret = bdi_writeback_task(wb);	// 执行写回任务，处理脏数据

	/*
	 * Remove us from the list
	 */
	/*
	 * 将我们从列表中移除
	 */
	spin_lock(&bdi->wb_lock);  // 加锁写回设备的锁
	list_del_rcu(&wb->list);  // 从写回设备列表中删除当前写回任务
	spin_unlock(&bdi->wb_lock);  // 释放锁

	/*
	 * Flush any work that raced with us exiting. No new work
	 * will be added, since this bdi isn't discoverable anymore.
	 */
	/*
	 * 清理与我们退出竞争的任何工作。由于此bdi不再可发现，不会添加新的工作。
	 */
	if (!list_empty(&bdi->work_list))	// 如果工作列表不为空
		wb_do_writeback(wb, 1);	// 执行写回工作

	wb->task = NULL;	// 清除任务指针，标识任务已结束
	return ret;	// 返回写回操作的结果
}

// 检查是否有脏数据IO
int bdi_has_dirty_io(struct backing_dev_info *bdi)
{
	// 调用wb_has_dirty_io检查写回（writeback）结构中是否有未写入磁盘的数据
	return wb_has_dirty_io(&bdi->wb);
}

// 清空BDI中的IO
static void bdi_flush_io(struct backing_dev_info *bdi)
{
	struct writeback_control wbc = {
		.bdi			= bdi,	// 当前BDI
		.sync_mode		= WB_SYNC_NONE,	// 同步模式设置为无
		.older_than_this	= NULL,	// 没有设置时间限制
		.range_cyclic		= 1,		// 循环范围
		.nr_to_write		= 1024,	// 设置单次可写的最大数量
	};

	// 调用writeback_inodes_wbc函数执行写回操作，将数据真正写入磁盘
	writeback_inodes_wbc(&wbc);
}

/*
 * kupdated() used to do this. We cannot do it from the bdi_forker_task()
 * or we risk deadlocking on ->s_umount. The longer term solution would be
 * to implement sync_supers_bdi() or similar and simply do it from the
 * bdi writeback tasks individually.
 */
/*
 * kupdated() 以前用于做这个。我们不能从 bdi_forker_task() 中做这个，
 * 否则我们会有在 ->s_umount 上死锁的风险。一个长期的解决方案是实现
 * sync_supers_bdi() 或类似功能，并单独从 bdi 写回任务中做这个。
 */
// 同步超级块
static int bdi_sync_supers(void *unused)
{
	// 设置当前线程的优先级
	set_user_nice(current, 0);

	// 当线程不需要停止时，持续执行
	while (!kthread_should_stop()) {
		// 设置线程为可中断的休眠状态
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();	// 调度执行

		/*
		 * Do this periodically, like kupdated() did before.
		 */
		/*
		 * 像以前kupdated()所做的那样定期执行
		 */
		// 同步超级块
		sync_supers();
	}

	// 返回0表示执行成功
	return 0;
}

// 设定并激活超级块同步的计时器
static void arm_supers_timer(void)
{
	unsigned long next;

	// 计算下一次激活时间，基于当前时间加上设定间隔
	next = msecs_to_jiffies(dirty_writeback_interval * 10) + jiffies;
	// 调整并更新计时器，使其在预定的时间点上触发
	mod_timer(&sync_supers_timer, round_jiffies_up(next));
}

// 超级块同步计时器的回调函数
static void sync_supers_timer_fn(unsigned long unused)
{
	// 唤醒处理同步超级块的任务
	wake_up_process(sync_supers_tsk);
	// 重新设置并激活计时器
	arm_supers_timer();
}

// 后台数据写入调度任务的主函数
static int bdi_forker_task(void *ptr)
{
	// 获取传入的写回结构体指针
	struct bdi_writeback *me = ptr;

	// 初始化当前任务
	bdi_task_init(me->bdi, me);

	for (;;) {	// 无限循环，持续处理写回任务
		struct backing_dev_info *bdi, *tmp;
		struct bdi_writeback *wb;

		/*
		 * Temporary measure, we want to make sure we don't see
		 * dirty data on the default backing_dev_info
		 */
		/*
		 * 暂时的措施，我们想确保我们不会在默认的backing_dev_info上看到脏数据
		 */
		// 如果有脏IO或工作列表不为空，执行写回
		if (wb_has_dirty_io(me) || !list_empty(&me->bdi->work_list))
			wb_do_writeback(me, 0);

		spin_lock_bh(&bdi_lock);	// 获取锁保护后续操作

		/*
		 * Check if any existing bdi's have dirty data without
		 * a thread registered. If so, set that up.
		 */
		/*
		 * 检查是否有现有的bdi含有脏数据且没有注册的线程。
		 * 如果有，设置一个新的线程。
		 */
		// 遍历所有注册的后台设备信息
		list_for_each_entry_safe(bdi, tmp, &bdi_list, bdi_list) {
			if (bdi->wb.task)	// 如果该设备已有处理任务，跳过
				continue;
			// 如果工作列表为空且没有脏IO，跳过
			if (list_empty(&bdi->work_list) &&
			    !bdi_has_dirty_io(bdi))
				continue;

			// 为该设备添加一个默认的刷新任务
			bdi_add_default_flusher_task(bdi);
		}

		// 将当前任务设置为可中断
		set_current_state(TASK_INTERRUPTIBLE);

		// 如果待处理列表为空
		if (list_empty(&bdi_pending_list)) {
			unsigned long wait;

			spin_unlock_bh(&bdi_lock);	// 释放锁
			// 计算等待时间
			wait = msecs_to_jiffies(dirty_writeback_interval * 10);
			schedule_timeout(wait);	// 调度超时等待
			try_to_freeze();	// 尝试处理冻结任务
			continue;
		}

		// 设置任务状态为运行
		__set_current_state(TASK_RUNNING);

		/*
		 * This is our real job - check for pending entries in
		 * bdi_pending_list, and create the tasks that got added
		 */
		/*
		 * 这是我们的真正任务 - 检查bdi_pending_list中的待处理条目，
		 * 并创建已经添加的任务。
		 */
		// 处理待处理列表中的第一个后台设备信息
		bdi = list_entry(bdi_pending_list.next, struct backing_dev_info,
				 bdi_list);
		list_del_init(&bdi->bdi_list);	// 从列表中移除
		spin_unlock_bh(&bdi_lock);			// 释放锁

		wb = &bdi->wb;
		// 创建新的线程来处理写回任务
		wb->task = kthread_run(bdi_start_fn, wb, "flush-%s",
					dev_name(bdi->dev));
		/*
		 * If task creation fails, then readd the bdi to
		 * the pending list and force writeout of the bdi
		 * from this forker thread. That will free some memory
		 * and we can try again.
		 */
		/*
		 * 如果任务创建失败，则重新添加bdi到待处理列表，并强制
		 * 从这个forker线程写出bdi数据。这将释放一些内存，
		 * 我们可以再次尝试。
		 */
		if (IS_ERR(wb->task)) {	// 如果任务创建失败
			wb->task = NULL;

			/*
			 * Add this 'bdi' to the back, so we get
			 * a chance to flush other bdi's to free
			 * memory.
			 */
			/*
			 * 将此'bdi'添加到末尾，以便我们有机会
			 * 刷新其他bdi来释放内存。
			 */
			spin_lock_bh(&bdi_lock);	// 再次获取锁
			// 将设备信息重新加入到待处理列表
			list_add_tail(&bdi->bdi_list, &bdi_pending_list);
			spin_unlock_bh(&bdi_lock);	// 释放锁

			bdi_flush_io(bdi);	// 强制刷新设备IO
		}
	}

	return 0;		// 返回0，正常退出
}

// 添加到待处理列表的函数
static void bdi_add_to_pending(struct rcu_head *head)
{
	struct backing_dev_info *bdi;

	 // 从 rcu_head 结构获取 bdi 指针
	bdi = container_of(head, struct backing_dev_info, rcu_head);
	INIT_LIST_HEAD(&bdi->bdi_list);	// 初始化 bdi_list 链表

	spin_lock(&bdi_lock);	// 加锁保护全局链表操作
	// 将 bdi 添加到待处理列表的尾部
	list_add_tail(&bdi->bdi_list, &bdi_pending_list);
	spin_unlock(&bdi_lock);	// 解锁

	/*
	 * We are now on the pending list, wake up bdi_forker_task()
	 * to finish the job and add us back to the active bdi_list
	 */
	/*
	 * 我们现在在待处理列表上，唤醒 bdi_forker_task() 来完成工作
	 * 并将我们添加回活跃的 bdi_list
	 */
	wake_up_process(default_backing_dev_info.wb.task);	// 唤醒处理任务
}

/*
 * Add the default flusher task that gets created for any bdi
 * that has dirty data pending writeout
 */
// 为有脏数据待写出的任何 bdi 添加默认的刷新任务
void static bdi_add_default_flusher_task(struct backing_dev_info *bdi)
{
	// 如果 bdi 没有写回脏数据的能力，则返回
	if (!bdi_cap_writeback_dirty(bdi))
		return;

	// 如果 bdi 没有注册，发出警告并返回
	if (WARN_ON(!test_bit(BDI_registered, &bdi->state))) {
		printk(KERN_ERR "bdi %p/%s is not registered!\n",
							bdi, bdi->name);
		return;
	}

	/*
	 * Check with the helper whether to proceed adding a task. Will only
	 * abort if we two or more simultanous calls to
	 * bdi_add_default_flusher_task() occured, further additions will block
	 * waiting for previous additions to finish.
	 */
	/*
	 * 检查是否继续添加任务的辅助函数。如果发生两次或多次同时调用
	 * bdi_add_default_flusher_task()，将终止；其他添加将阻塞，等待前一个添加完成。
	 */
	// 测试并设置 BDI_pending 状态位
	if (!test_and_set_bit(BDI_pending, &bdi->state)) {
		list_del_rcu(&bdi->bdi_list);	// 从当前列表中删除

		/*
		 * We must wait for the current RCU period to end before
		 * moving to the pending list. So schedule that operation
		 * from an RCU callback.
		 */
		/*
		 * 我们必须等待当前的 RCU 周期结束才能移动到待处理列表。
		 * 因此，从 RCU 回调中安排这个操作。
		 */
		// 调用 RCU 回调函数将其添加到待处理列表
		call_rcu(&bdi->rcu_head, bdi_add_to_pending);
	}
}

/*
 * Remove bdi from bdi_list, and ensure that it is no longer visible
 */
/*
 * 从 bdi_list 中移除 bdi，并确保它不再可见
 */
static void bdi_remove_from_list(struct backing_dev_info *bdi)
{
	spin_lock_bh(&bdi_lock);	// 锁定，保护全局列表的操作
	list_del_rcu(&bdi->bdi_list);	// 从 RCU 受保护的列表中删除 bdi
	spin_unlock_bh(&bdi_lock);	// 解锁

	synchronize_rcu();	// 等待 RCU 同步，确保所有的 CPU 都完成了对 bdi 的访问
}

int bdi_register(struct backing_dev_info *bdi, struct device *parent,
		const char *fmt, ...)
{
	va_list args;
	int ret = 0;
	struct device *dev;

	/* 如果驱动需要为每个设备使用单独的队列 */
	if (bdi->dev)	/* The driver needs to use separate queues per device */
		goto exit;

	va_start(args, fmt);	// 初始化变参列表
	// 根据给定的参数格式创建设备
	dev = device_create_vargs(bdi_class, parent, MKDEV(0, 0), bdi, fmt, args);
	va_end(args);	// 结束变参处理
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);	// 如果设备创建失败，获取错误码
		goto exit;
	}

	spin_lock_bh(&bdi_lock);	// 锁定
	// 将 bdi 添加到全局的 bdi_list
	list_add_tail_rcu(&bdi->bdi_list, &bdi_list);
	spin_unlock_bh(&bdi_lock);	// 解锁

	bdi->dev = dev;	// 设置 bdi 的设备指针

	/*
	 * Just start the forker thread for our default backing_dev_info,
	 * and add other bdi's to the list. They will get a thread created
	 * on-demand when they need it.
	 */
	/*
	 * 只为我们的默认 backing_dev_info 启动 forker 线程，
	 * 并将其他 bdi 添加到列表。它们将在需要时按需创建线程。
	 */
	// 如果 bdi 能够启动 flusher forker 线程
	if (bdi_cap_flush_forker(bdi)) {
		struct bdi_writeback *wb = &bdi->wb;

		// 启动 bdi_forker_task 线程
		wb->task = kthread_run(bdi_forker_task, wb, "bdi-%s",
						dev_name(dev));
		if (IS_ERR(wb->task)) {	// 如果线程启动失败
			wb->task = NULL;
			ret = -ENOMEM;	// 内存错误

			bdi_remove_from_list(bdi);	// 移除 bdi
			goto exit;
		}
	}

	bdi_debug_register(bdi, dev_name(dev));	// 注册 bdi 的调试信息
	set_bit(BDI_registered, &bdi->state);	// 设置 bdi 为已注册状态
exit:
	return ret;
}
EXPORT_SYMBOL(bdi_register);

int bdi_register_dev(struct backing_dev_info *bdi, dev_t dev)
{
	// 使用设备的主次号注册BDI
	return bdi_register(bdi, NULL, "%u:%u", MAJOR(dev), MINOR(dev));
}
EXPORT_SYMBOL(bdi_register_dev);

/*
 * Remove bdi from the global list and shutdown any threads we have running
 */
/*
 * 从全局列表中移除 bdi 并关闭任何我们正在运行的线程
 */
static void bdi_wb_shutdown(struct backing_dev_info *bdi)
{
	struct bdi_writeback *wb;

	// 如果 bdi 没有启用写回脏数据的能力，则直接返回
	if (!bdi_cap_writeback_dirty(bdi))
		return;

	/*
	 * If setup is pending, wait for that to complete first
	 */
	/*
	 * 如果设置正在进行中，首先等待其完成
	 */
	// 在 bdi 的状态位上等待，直到不再是 BDI_pending 状态
	wait_on_bit(&bdi->state, BDI_pending, bdi_sched_wait,
			TASK_UNINTERRUPTIBLE);

	/*
	 * Make sure nobody finds us on the bdi_list anymore
	 */
	/*
	 * 确保没有人在 bdi_list 上找到我们
	 */
	bdi_remove_from_list(bdi);	// 从全局的 bdi_list 中移除 bdi

	/*
	 * Finally, kill the kernel threads. We don't need to be RCU
	 * safe anymore, since the bdi is gone from visibility. Force
	 * unfreeze of the thread before calling kthread_stop(), otherwise
	 * it would never exet if it is currently stuck in the refrigerator.
	 */
	/*
	 * 最后，结束内核线程。此时不再需要保证 RCU 安全，因为 bdi 已经从可见性中移除。
	 * 在调用 kthread_stop() 之前强制解冻线程，否则如果当前线程被冻结，它将永远不会退出。
	 */
	// 遍历 bdi 关联的所有写回结构
	list_for_each_entry(wb, &bdi->wb_list, list) {
		thaw_process(wb->task);	// 解冻每个关联的写回任务
		kthread_stop(wb->task);	// 停止每个关联的写回任务
	}
}

/*
 * This bdi is going away now, make sure that no super_blocks point to it
 */
/*
 * 这个 bdi 现在将被移除，确保没有任何 super_blocks 指向它
 */
static void bdi_prune_sb(struct backing_dev_info *bdi)
{
	struct super_block *sb;

	spin_lock(&sb_lock);	// 加锁，保护对 super_blocks 链表的访问
	// 遍历所有的 super_block
	list_for_each_entry(sb, &super_blocks, s_list) {
		// 如果 super_block 的 bdi 指向当前的 bdi
		if (sb->s_bdi == bdi)
			sb->s_bdi = NULL;	// 将其置为 NULL，避免野指针
	}
	spin_unlock(&sb_lock);	// 解锁
}

/*
 * 注销 bdi
 */
void bdi_unregister(struct backing_dev_info *bdi)
{
	if (bdi->dev) {	// 如果 bdi 有关联的设备
		// 调用 bdi_prune_sb 以确保 no super_blocks 指向这个 bdi
		bdi_prune_sb(bdi);

		// 如果 bdi 没有派生刷新器的能力
		if (!bdi_cap_flush_forker(bdi))
			// 关闭与 bdi 关联的写回任务
			bdi_wb_shutdown(bdi);
		// 注销与 bdi 相关的调试信息
		bdi_debug_unregister(bdi);
		// 注销设备
		device_unregister(bdi->dev);
		bdi->dev = NULL;	// 将设备指针置为 NULL
	}
}
EXPORT_SYMBOL(bdi_unregister);

/*
 * 初始化 backing_dev_info 结构体
 */
int bdi_init(struct backing_dev_info *bdi)
{
	int i, err;

	bdi->dev = NULL;	// 初始化设备指针为空

	bdi->min_ratio = 0;		// 初始化最小比例为0
	bdi->max_ratio = 100;	// 初始化最大比例为100
	bdi->max_prop_frac = PROP_FRAC_BASE;	// 初始化最大比例因子
	spin_lock_init(&bdi->wb_lock);	// 初始化写回锁
	INIT_RCU_HEAD(&bdi->rcu_head);	// 初始化 RCU 头
	INIT_LIST_HEAD(&bdi->bdi_list);	// 初始化 BDI 列表
	INIT_LIST_HEAD(&bdi->wb_list);	// 初始化写回列表
	INIT_LIST_HEAD(&bdi->work_list);	// 初始化工作列表

	// 初始化写回结构
	bdi_wb_init(&bdi->wb, bdi);

	/*
	 * Just one thread support for now, hard code mask and count
	 */
	/*
	 * 当前仅支持一个线程，硬编码掩码和计数
	 */
	bdi->wb_mask = 1;	// 设置写回掩码
	bdi->wb_cnt = 1;	// 设置写回计数

	// 初始化 BDI 状态项的每个 CPU 计数器
	for (i = 0; i < NR_BDI_STAT_ITEMS; i++) {
		err = percpu_counter_init(&bdi->bdi_stat[i], 0);
		if (err)
			goto err;	// 初始化失败，跳转到错误处理
	}

	// 初始化脏页超限标志为0
	bdi->dirty_exceeded = 0;
	// 初始化局部完成度
	err = prop_local_init_percpu(&bdi->completions);

	if (err) {
err:
		while (i--)
			// 错误处理：销毁已初始化的计数器
			percpu_counter_destroy(&bdi->bdi_stat[i]);
	}

	return err;	// 返回错误代码
}	
EXPORT_SYMBOL(bdi_init);

/*
 * 销毁 backing_dev_info 结构体
 */
void bdi_destroy(struct backing_dev_info *bdi)
{
	int i;

	/*
	 * Splice our entries to the default_backing_dev_info, if this
	 * bdi disappears
	 */
	/*
	 * 如果这个 bdi 要消失，将我们的条目拼接到 default_backing_dev_info 上
	 */
	if (bdi_has_dirty_io(bdi)) {
		struct bdi_writeback *dst = &default_backing_dev_info.wb;

		spin_lock(&inode_lock);	 // 获取 inode 锁
		 // 将脏数据列表合并到默认的写回结构中
		list_splice(&bdi->wb.b_dirty, &dst->b_dirty);
		// 将 IO 列表合并
		list_splice(&bdi->wb.b_io, &dst->b_io);
		// 将更多的 IO 列表合并
		list_splice(&bdi->wb.b_more_io, &dst->b_more_io);
		// 释放 inode 锁
		spin_unlock(&inode_lock);
	}

	bdi_unregister(bdi);	// 注销 bdi

	// 销毁每个 CPU 的 BDI 状态计数器
	for (i = 0; i < NR_BDI_STAT_ITEMS; i++)
		percpu_counter_destroy(&bdi->bdi_stat[i]);

	// 销毁局部完成度
	prop_local_destroy_percpu(&bdi->completions);
}
EXPORT_SYMBOL(bdi_destroy);

/*
 * For use from filesystems to quickly init and register a bdi associated
 * with dirty writeback
 */
/*
 * 供文件系统使用，用于快速初始化和注册与脏写回相关的 bdi
 */
int bdi_setup_and_register(struct backing_dev_info *bdi, char *name,
			   unsigned int cap)
{
	char tmp[32];
	int err;

	bdi->name = name;  // 设置 bdi 的名称
	bdi->capabilities = cap;  // 设置 bdi 的功能
	err = bdi_init(bdi);  // 初始化 bdi
	if (err)
		return err;	// 如果初始化失败，返回错误

	// 生成 bdi 的注册名称
	sprintf(tmp, "%.28s%s", name, "-%d");
	// 注册 bdi，增加 bdi 序列号，并使用格式化的名称
	err = bdi_register(bdi, NULL, tmp, atomic_long_inc_return(&bdi_seq));
	if (err) {
		// 如果注册失败，销毁 bdi
		bdi_destroy(bdi);
		return err;	// 返回错误
	}

	return 0;	// 成功返回 0
}
EXPORT_SYMBOL(bdi_setup_and_register);

// 定义了两个等待队列头，用于处理同步和异步I/O的拥塞等待
static wait_queue_head_t congestion_wqh[2] = {
		__WAIT_QUEUE_HEAD_INITIALIZER(congestion_wqh[0]),
		__WAIT_QUEUE_HEAD_INITIALIZER(congestion_wqh[1])
	};

// 清除指定backing_dev_info的拥塞状态
void clear_bdi_congested(struct backing_dev_info *bdi, int sync)
{
	enum bdi_state bit;
	// 根据sync参数选择合适的等待队列
	wait_queue_head_t *wqh = &congestion_wqh[sync];

	// 根据sync决定是同步还是异步拥塞位
	bit = sync ? BDI_sync_congested : BDI_async_congested;
	// 清除拥塞位
	clear_bit(bit, &bdi->state);
	// 确保清除操作的内存屏障
	smp_mb__after_clear_bit();
	if (waitqueue_active(wqh))
		wake_up(wqh);	// 如果有进程在等待队列中，唤醒它们
}
EXPORT_SYMBOL(clear_bdi_congested);

// 设置指定backing_dev_info的拥塞状态
void set_bdi_congested(struct backing_dev_info *bdi, int sync)
{
	enum bdi_state bit;

	// 决定设置同步或异步拥塞位
	bit = sync ? BDI_sync_congested : BDI_async_congested;
	set_bit(bit, &bdi->state);	// 设置拥塞位
}
EXPORT_SYMBOL(set_bdi_congested);

/**
 * congestion_wait - wait for a backing_dev to become uncongested
 * @sync: SYNC or ASYNC IO
 * @timeout: timeout in jiffies
 *
 * Waits for up to @timeout jiffies for a backing_dev (any backing_dev) to exit
 * write congestion.  If no backing_devs are congested then just wait for the
 * next write to be completed.
 */
/**
 * congestion_wait - 等待一个backing_dev变得不拥堵
 * @sync: SYNC或ASYNC IO
 * @timeout: 超时时间，以jiffies为单位
 *
 * 等待最多@timeout jiffies，以便一个backing_dev（任何backing_dev）退出写拥塞状态。
 * 如果没有backing_devs处于拥塞状态，那么就等待下一次写操作完成。
 */
long congestion_wait(int sync, long timeout)
{
	long ret;
	DEFINE_WAIT(wait);
	// 根据sync选择等待队列
	wait_queue_head_t *wqh = &congestion_wqh[sync];

	// 准备等待
	prepare_to_wait(wqh, &wait, TASK_UNINTERRUPTIBLE);
	ret = io_schedule_timeout(timeout);	// 等待或直到超时
	finish_wait(wqh, &wait);	// 完成等待
	return ret;	// 返回经过的时间
}
EXPORT_SYMBOL(congestion_wait);

