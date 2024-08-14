/*
 *  fs/eventpoll.c (Efficient event retrieval implementation)
 *  Copyright (C) 2001,...,2009	 Davide Libenzi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Davide Libenzi <davidel@xmailserver.org>
 *
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/hash.h>
#include <linux/spinlock.h>
#include <linux/syscalls.h>
#include <linux/rbtree.h>
#include <linux/wait.h>
#include <linux/eventpoll.h>
#include <linux/mount.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/anon_inodes.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/mman.h>
#include <asm/atomic.h>

/*
 * LOCKING:
 * There are three level of locking required by epoll :
 *
 * 1) epmutex (mutex)
 * 2) ep->mtx (mutex)
 * 3) ep->lock (spinlock)
 *
 * The acquire order is the one listed above, from 1 to 3.
 * We need a spinlock (ep->lock) because we manipulate objects
 * from inside the poll callback, that might be triggered from
 * a wake_up() that in turn might be called from IRQ context.
 * So we can't sleep inside the poll callback and hence we need
 * a spinlock. During the event transfer loop (from kernel to
 * user space) we could end up sleeping due a copy_to_user(), so
 * we need a lock that will allow us to sleep. This lock is a
 * mutex (ep->mtx). It is acquired during the event transfer loop,
 * during epoll_ctl(EPOLL_CTL_DEL) and during eventpoll_release_file().
 * Then we also need a global mutex to serialize eventpoll_release_file()
 * and ep_free().
 * This mutex is acquired by ep_free() during the epoll file
 * cleanup path and it is also acquired by eventpoll_release_file()
 * if a file has been pushed inside an epoll set and it is then
 * close()d without a previous call toepoll_ctl(EPOLL_CTL_DEL).
 * It is possible to drop the "ep->mtx" and to use the global
 * mutex "epmutex" (together with "ep->lock") to have it working,
 * but having "ep->mtx" will make the interface more scalable.
 * Events that require holding "epmutex" are very rare, while for
 * normal operations the epoll private "ep->mtx" will guarantee
 * a better scalability.
 */
/* 锁定：
 * epoll需要三级锁定：
 *
 * 1) epmutex (互斥锁)
 * 2) ep->mtx (互斥锁)
 * 3) ep->lock (自旋锁)
 *
 * 锁的获取顺序是上面列出的顺序，从1到3。
 * 我们需要一个自旋锁(ep->lock)，因为我们在poll回调内部操作对象，
 * 该回调可能由wake_up()触发，而wake_up()可能从IRQ上下文中调用。
 * 因此我们不能在poll回调中睡眠，所以需要一个自旋锁。在事件传输循环（从内核到用户空间）中，
 * 我们可能因为copy_to_user()而睡眠，因此我们需要一个允许我们睡眠的锁。这个锁是一个互斥锁(ep->mtx)。
 * 它在事件传输循环、在epoll_ctl(EPOLL_CTL_DEL)和在eventpoll_release_file()时获取。
 * 然后我们还需要一个全局互斥锁来序列化eventpoll_release_file()和ep_free()。
 * 此互斥锁由ep_free()在epoll文件清理路径期间获取，也由eventpoll_release_file()获取，
 * 如果一个文件被推送进一个epoll集合并随后被关闭，而没有先前调用epoll_ctl(EPOLL_CTL_DEL)。
 * 可以丢弃"ep->mtx"并使用全局互斥锁"epmutex"（与"ep->lock"一起）来工作，
 * 但是拥有"ep->mtx"将使接口更具可扩展性。
 * 需要持有"epmutex"的事件非常罕见，而对于正常操作，epoll私有的"ep->mtx"将保证更好的可扩展性。
 */


/* Epoll private bits inside the event mask */
/* 事件掩码中的epoll私有位 */
#define EP_PRIVATE_BITS (EPOLLONESHOT | EPOLLET)	// EPOLLONESHOT和EPOLLET为epoll特定的操作标志，合并定义为私有位

/* Maximum number of nesting allowed inside epoll sets */
/* epoll集合内允许的最大嵌套数量 */
#define EP_MAX_NESTS 4	// 定义最大嵌套为4层

/* Maximum msec timeout value storeable in a long int */
/* 长整型中可存储的最大毫秒超时值 */
#define EP_MAX_MSTIMEO min(1000ULL * MAX_SCHEDULE_TIMEOUT / HZ, (LONG_MAX - 999ULL) / HZ)	// 计算最大毫秒超时值

#define EP_MAX_EVENTS (INT_MAX / sizeof(struct epoll_event))	// 定义单次epoll_wait调用可以返回的最大事件数

#define EP_UNACTIVE_PTR ((void *) -1L)	// 定义一个特殊的指针值，用于标识非活跃的epoll项

#define EP_ITEM_COST (sizeof(struct epitem) + sizeof(struct eppoll_entry))	// 计算一个epoll项的内存开销

/* 定义一个结构体，用于关联文件结构体和文件描述符 */
// 将文件描述符与对应的文件结构体关联起来，用于epoll操作中方便地处理文件事件。
struct epoll_filefd {
	struct file *file;  // 指向文件结构体的指针
	int fd;             // 文件描述符
};

/*
 * Structure used to track possible nested calls, for too deep recursions
 * and loop cycles.
 */
/* 用于追踪可能的嵌套调用，以检测过深的递归和循环 */
// 为了防止在epoll操作中发生过深的递归调用和潜在的循环。每个节点通过链表链接到其他节点，提供了一种机制来追踪和管理嵌套调用的历史和上下文。
struct nested_call_node {
	struct list_head llink;  // 链表链接，用于连接多个嵌套调用节点
	void *cookie;            // 用户自定义的标识或数据，用于区分或标记调用
	void *ctx;               // 上下文信息，可用于存储调用时的状态或其他相关数据
};

/*
 * This structure is used as collector for nested calls, to check for
 * maximum recursion dept and loop cycles.
 */
/* 这个结构体被用作嵌套调用的收集器，用于检查最大递归深度和循环 */
// 用作整体的嵌套调用管理器，包含一个链表来追踪所有活跃的嵌套调用和一个自旋锁以保证线程安全。这个结构体关键用于控制和限制epoll中可能发生的递归调用，防止系统资源耗尽和保护内核稳定运行。
struct nested_calls {
	struct list_head tasks_call_list;  // 任务调用链表，存储所有嵌套调用节点
	spinlock_t lock;                  // 自旋锁，用于同步对嵌套调用列表的访问
};

/*
 * Each file descriptor added to the eventpoll interface will
 * have an entry of this type linked to the "rbr" RB tree.
 */
/* 添加到 eventpoll 接口的每个文件描述符都会在“rbr”红黑树中有一个此类型的条目。 */
struct epitem {
	/* RB tree node used to link this structure to the eventpoll RB tree */
	/* 红黑树节点，用于将此结构链接到 eventpoll 红黑树 */
	struct rb_node rbn;

	/* List header used to link this structure to the eventpoll ready list */
	/* 列表头，用于将此结构链接到 eventpoll 准备就绪列表 */
	struct list_head rdllink;

	/*
	 * Works together "struct eventpoll"->ovflist in keeping the
	 * single linked chain of items.
	 */
	/* 与“struct eventpoll”->ovflist 协作，保持项的单链表链 */
	struct epitem *next;

	/* The file descriptor information this item refers to */
	/* 此项所引用的文件描述符信息 */
	struct epoll_filefd ffd;

	/* Number of active wait queue attached to poll operations */
	/* 附加到 poll 操作的活跃等待队列数 */
	int nwait;

	/* List containing poll wait queues */
	/* 包含 poll 等待队列的列表 */
	struct list_head pwqlist;

	/* The "container" of this item */
	/* 此项的“容器” */
	struct eventpoll *ep;

	/* List header used to link this item to the "struct file" items list */
	/* 列表头，用于将此项链接到“struct file”项列表 */
	struct list_head fllink;

	/* The structure that describe the interested events and the source fd */
	/* 描述感兴趣事件和源文件描述符的结构 */
	struct epoll_event event;
};

/*
 * This structure is stored inside the "private_data" member of the file
 * structure and rapresent the main data sructure for the eventpoll
 * interface.
 */
/* 此结构体存储在文件结构的 "private_data" 成员中，代表 eventpoll 接口的主要数据结构。 */
struct eventpoll {
	/* Protect the this structure access */
	/* 保护此结构体访问的自旋锁 */
	spinlock_t lock;

	/*
	 * This mutex is used to ensure that files are not removed
	 * while epoll is using them. This is held during the event
	 * collection loop, the file cleanup path, the epoll file exit
	 * code and the ctl operations.
	 */
	/* 
	 * 此互斥锁用于确保在 epoll 使用文件时文件不会被移除。
	 * 它在事件收集循环、文件清理路径、epoll 文件退出代码和控制操作期间持有。
	 */
	struct mutex mtx;

	/* Wait queue used by sys_epoll_wait() */
	/* sys_epoll_wait() 使用的等待队列 */
	wait_queue_head_t wq;

	/* Wait queue used by file->poll() */
	/* file->poll() 使用的等待队列 */
	wait_queue_head_t poll_wait;

	/* List of ready file descriptors */
	/* 准备就绪的文件描述符列表 */
	struct list_head rdllist;

	/* RB tree root used to store monitored fd structs */
	/* 用于存储被监控的文件描述符结构的红黑树根 */
	struct rb_root rbr;

	/*
	 * This is a single linked list that chains all the "struct epitem" that
	 * happened while transfering ready events to userspace w/out
	 * holding ->lock.
	 */
	/* 
	 * 这是一个单链表，链接所有在将就绪事件传输到用户空间时发生的 "struct epitem"，
	 * 在此过程中不持有 ->lock。
	 */
	struct epitem *ovflist;

	/* The user that created the eventpoll descriptor */
	/* 创建 eventpoll 描述符的用户 */
	struct user_struct *user;
};

/* Wait structure used by the poll hooks */
/* 由 poll 钩子使用的等待结构 */
struct eppoll_entry {
	/* List header used to link this structure to the "struct epitem" */
	/* 列表头，用于将此结构链接到 "struct epitem" */
	struct list_head llink;

	/* The "base" pointer is set to the container "struct epitem" */
	/* "base" 指针设置为容器 "struct epitem" */
	struct epitem *base;

	/*
	 * Wait queue item that will be linked to the target file wait
	 * queue head.
	 */
	/* 将被链接到目标文件等待队列头的等待队列项 */
	wait_queue_t wait;

	/* The wait queue head that linked the "wait" wait queue item */
	/* 链接 "wait" 等待队列项的等待队列头 */
	wait_queue_head_t *whead;
};

/* Wrapper struct used by poll queueing */
/* 用于 poll 队列的封装结构 */
struct ep_pqueue {
	poll_table pt;	/* poll_table 结构，通常用于存储 poll 调用期间的信息 */
	struct epitem *epi;	/* 指向关联的 epitem 结构的指针，epitem 存储了特定文件描述符的事件信息 */
};

/* Used by the ep_send_events() function as callback private data */
/* 用作 ep_send_events() 函数回调的私有数据 */
struct ep_send_events_data {
	int maxevents;                       /* 可以返回给用户空间的最大事件数量 */
	struct epoll_event __user *events;  /* 指向用户空间的 epoll_event 数组，用于将事件数据从内核传输到用户空间 */
};

/*
 * Configuration options available inside /proc/sys/fs/epoll/
 */
/* 在 /proc/sys/fs/epoll/ 内可用的配置选项 */
/* Maximum number of epoll watched descriptors, per user */
/* 每个用户监视的 epoll 文件描述符的最大数量 */
static int max_user_watches __read_mostly;

/*
 * This mutex is used to serialize ep_free() and eventpoll_release_file().
 */
/* 此互斥锁用于序列化 ep_free() 和 eventpoll_release_file() 的调用。 */
static DEFINE_MUTEX(epmutex);

/* Used for safe wake up implementation */
/* 用于安全唤醒的实现 */
static struct nested_calls poll_safewake_ncalls;

/* Used to call file's f_op->poll() under the nested calls boundaries */
/* 用于在嵌套调用边界下调用文件的 f_op->poll() */
static struct nested_calls poll_readywalk_ncalls;

/* Slab cache used to allocate "struct epitem" */
/* 用于分配 "struct epitem" 的 Slab 缓存 */
static struct kmem_cache *epi_cache __read_mostly;

/* Slab cache used to allocate "struct eppoll_entry" */
/* 用于分配 "struct eppoll_entry" 的 Slab 缓存 */
static struct kmem_cache *pwq_cache __read_mostly;

#ifdef CONFIG_SYSCTL	// 如果启用了 SYSCTL 配置

#include <linux/sysctl.h>

static int zero;	// 一个用作最小值限制的静态变量，初始化为0

/* 定义一个 sysctl 表，通过 /proc 文件系统暴露一些 epoll 的配置 */
ctl_table epoll_table[] = {
	{
		.procname	= "max_user_watches",  // /proc/sys/fs/epoll/max_user_watches 的名称
		.data		= &max_user_watches,   // 指向 max_user_watches 变量的指针
		.maxlen		= sizeof(int),         // 这个变量的大小
		.mode		= 0644,                // 文件的权限模式（允许用户读写，组读取）
		.proc_handler	= proc_dointvec_minmax,// 处理函数，用于读写 int 类型的数据
		.extra1		= &zero,               // 指向定义的最小值限制
	},
	{ }  // 空条目，表示表的结束
};
#endif /* CONFIG_SYSCTL */


/* Setup the structure that is used as key for the RB tree */
/* 设置用作红黑树键的结构体 */
static inline void ep_set_ffd(struct epoll_filefd *ffd,
			      struct file *file, int fd)
{
	ffd->file = file;  // 将结构体中的文件指针设置为提供的文件指针
	ffd->fd = fd;      // 将结构体中的文件描述符设置为提供的文件描述符
}

/* Compare RB tree keys */
/* 比较红黑树的键 */
static inline int ep_cmp_ffd(struct epoll_filefd *p1,
			     struct epoll_filefd *p2)
{
	// 对两个文件指针进行比较，如果不相等，根据其内存地址返回 +1 或 -1
	return (p1->file > p2->file ? +1:
	        (p1->file < p2->file ? -1 : p1->fd - p2->fd));
	// 如果文件指针相同，则比较文件描述符并返回差值
}

/* Tells us if the item is currently linked */
/* 判断项是否当前已链接 */
static inline int ep_is_linked(struct list_head *p)
{
	return !list_empty(p);  // 如果列表不为空，返回 true，表示项已链接
}

/* Get the "struct epitem" from a wait queue pointer */
/* 从等待队列指针获取 "struct epitem" */
static inline struct epitem *ep_item_from_wait(wait_queue_t *p)
{
	return container_of(p, struct eppoll_entry, wait)->base;  // 使用 container_of 宏，从等待队列项中获取包含它的 eppoll_entry 结构，然后返回其 base 成员，即对应的 epitem
}

/* Get the "struct epitem" from an epoll queue wrapper */
/* 从 epoll 队列包装器获取 "struct epitem" */
static inline struct epitem *ep_item_from_epqueue(poll_table *p)
{
	return container_of(p, struct ep_pqueue, pt)->epi;  // 使用 container_of 宏，从 poll_table 中获取包含它的 ep_pqueue 结构，然后返回其 epi 成员，即对应的 epitem
}

/* Tells if the epoll_ctl(2) operation needs an event copy from userspace */
/* 判断 epoll_ctl(2) 操作是否需要从用户空间复制事件 */
static inline int ep_op_has_event(int op)
{
	// 如果操作不是 EPOLL_CTL_DEL（删除操作），则返回 true，表示该操作需要从用户空间复制事件
	return op != EPOLL_CTL_DEL;
}

/* Initialize the poll safe wake up structure */
/* 初始化用于安全唤醒的嵌套调用结构 */
static void ep_nested_calls_init(struct nested_calls *ncalls)
{
	// 初始化 tasks_call_list 链表头，表示此时链表为空
	INIT_LIST_HEAD(&ncalls->tasks_call_list);
	// 初始化嵌套调用结构中的自旋锁，用于保护该结构的并发访问
	spin_lock_init(&ncalls->lock);
}

/**
 * ep_call_nested - Perform a bound (possibly) nested call, by checking
 *                  that the recursion limit is not exceeded, and that
 *                  the same nested call (by the meaning of same cookie) is
 *                  no re-entered.
 *
 * @ncalls: Pointer to the nested_calls structure to be used for this call.
 * @max_nests: Maximum number of allowed nesting calls.
 * @nproc: Nested call core function pointer.
 * @priv: Opaque data to be passed to the @nproc callback.
 * @cookie: Cookie to be used to identify this nested call.
 * @ctx: This instance context.
 *
 * Returns: Returns the code returned by the @nproc callback, or -1 if
 *          the maximum recursion limit has been exceeded.
 */
/* ep_call_nested - 通过检查递归限制未被超过，并确保相同的嵌套调用（通过 cookie 标识）没有被重入，
 *                   执行一个有界（可能是嵌套的）调用。
 *
 * @ncalls: 指向此次调用要使用的 nested_calls 结构的指针。
 * @max_nests: 允许的最大嵌套调用数。
 * @nproc: 嵌套调用核心函数指针。
 * @priv: 传递给 @nproc 回调的不透明数据。
 * @cookie: 用于标识此嵌套调用的 cookie。
 * @ctx: 此实例上下文。
 *
 * 返回：返回 @nproc 回调返回的代码，如果超过最大递归限制，则返回 -1。
 */
/* ep_call_nested - 执行一个受限（可能嵌套的）调用，通过检查未超过递归限制，并且同一个嵌套调用（通过cookie标识）未被重入。*/
static int ep_call_nested(struct nested_calls *ncalls, int max_nests,
			  int (*nproc)(void *, void *, int), void *priv,
			  void *cookie, void *ctx)
{
	int error, call_nests = 0;  // 定义错误码和当前嵌套调用的层数
	unsigned long flags;  // 定义用于保存中断状态的变量
	struct list_head *lsthead = &ncalls->tasks_call_list;  // 获取任务链表头部
	struct nested_call_node *tncur;  // 用于遍历链表的临时节点指针
	struct nested_call_node tnode;  // 定义当前任务的节点

	spin_lock_irqsave(&ncalls->lock, flags);	// 加锁，保护对列表的操作，并保存中断状态

	/*
	 * Try to see if the current task is already inside this wakeup call.
	 * We use a list here, since the population inside this set is always
	 * very much limited.
	 */
	/* 尝试查看当前任务是否已经在此唤醒调用中。我们在这里使用一个列表，因为这个集合内的元素总是非常有限的。 */
	list_for_each_entry(tncur, lsthead, llink) {
		if (tncur->ctx == ctx &&
		    (tncur->cookie == cookie || ++call_nests > max_nests)) {
			/*
			 * Ops ... loop detected or maximum nest level reached.
			 * We abort this wake by breaking the cycle itself.
			 */
			/* 发现循环或达到最大嵌套层级。我们通过打破循环本身来中止这次唤醒。 */
			error = -1;
			goto out_unlock;
		}
	}

	/* Add the current task and cookie to the list */
	/* 将当前任务和 cookie 添加到列表中 */
	tnode.ctx = ctx;
	tnode.cookie = cookie;
	list_add(&tnode.llink, lsthead);

	spin_unlock_irqrestore(&ncalls->lock, flags);	// 解锁，并恢复之前保存的中断状态

	/* Call the nested function */
	/* 调用嵌套函数 */
	error = (*nproc)(priv, cookie, call_nests);

	/* Remove the current task from the list */
	/* 从列表中移除当前任务 */
	spin_lock_irqsave(&ncalls->lock, flags);
	list_del(&tnode.llink);
out_unlock:
	spin_unlock_irqrestore(&ncalls->lock, flags);	// 解锁，并恢复中断状态

	return error;	// 返回嵌套调用的结果或错误码
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/* 当定义了CONFIG_DEBUG_LOCK_ALLOC时，使用更复杂的锁定机制以支持锁依赖性检查 */
static inline void ep_wake_up_nested(wait_queue_head_t *wqueue,
				     unsigned long events, int subclass)
{
	unsigned long flags;

	// 上锁，保护等待队列，并保存中断状态，使用嵌套锁的子类来支持锁依赖性检查
	spin_lock_irqsave_nested(&wqueue->lock, flags, subclass);
	// 唤醒锁定的等待队列，传递指定的事件
	wake_up_locked_poll(wqueue, events);
	// 解锁，恢复之前保存的中断状态
	spin_unlock_irqrestore(&wqueue->lock, flags);
}
#else
// 当未定义CONFIG_DEBUG_LOCK_ALLOC时，使用简化的唤醒机制
static inline void ep_wake_up_nested(wait_queue_head_t *wqueue,
				     unsigned long events, int subclass)
{
	wake_up_poll(wqueue, events);	// 直接唤醒等待队列，传递指定的事件，不涉及锁操作
}
#endif

/* ep_poll_wakeup_proc - 用于唤醒封装在 cookie 中的 poll 等待队列的回调函数。*/
static int ep_poll_wakeup_proc(void *priv, void *cookie, int call_nests)
{
	/* 使用 cookie 作为 wait_queue_head_t 指针，调用 ep_wake_up_nested 唤醒等待队列。
	   cookie 在这里被转换回原本的 wait_queue_head_t 类型指针。
	   POLLIN 事件被传递，表示有数据可读。 */
	ep_wake_up_nested((wait_queue_head_t *) cookie, POLLIN,
			  1 + call_nests); // 唤醒过程中添加了 1 + call_nests，用于确定锁的子类。
	
	return 0; // 总是返回 0，表示成功。
}

/*
 * Perform a safe wake up of the poll wait list. The problem is that
 * with the new callback'd wake up system, it is possible that the
 * poll callback is reentered from inside the call to wake_up() done
 * on the poll wait queue head. The rule is that we cannot reenter the
 * wake up code from the same task more than EP_MAX_NESTS times,
 * and we cannot reenter the same wait queue head at all. This will
 * enable to have a hierarchy of epoll file descriptor of no more than
 * EP_MAX_NESTS deep.
 */
/* 
 * 执行对轮询等待列表的安全唤醒。问题在于，在新的回调唤醒系统中，可能会从对轮询等待队列头的 wake_up() 调用中
 * 重入轮询回调。规则是我们不能从同一任务中重入唤醒代码超过 EP_MAX_NESTS 次，并且我们根本不能重入同一个等待队列头。
 * 这将允许我们拥有不超过 EP_MAX_NESTS 深度的 epoll 文件描述符的层次结构。
 */
// 此函数用于安全地唤醒等待队列，以避免重入和递归调用的问题，特别是在使用事件轮询（epoll）机制时。
static void ep_poll_safewake(wait_queue_head_t *wq)
{
	int this_cpu = get_cpu();	// 获取当前 CPU 的 ID，防止在唤醒过程中发生 CPU 切换

	/* 
	 * 通过 ep_call_nested 调用 ep_poll_wakeup_proc 来安全地执行唤醒，
	 * 传递 NULL 作为私有数据，wq 作为 cookie，以及当前 CPU 作为上下文 
	 */
	ep_call_nested(&poll_safewake_ncalls, EP_MAX_NESTS,
		       ep_poll_wakeup_proc, NULL, wq, (void *) (long) this_cpu);

	put_cpu();	// 释放先前通过 get_cpu 获得的当前 CPU
}

/*
 * This function unregisters poll callbacks from the associated file
 * descriptor.  Must be called with "mtx" held (or "epmutex" if called from
 * ep_free).
 */
/* 
 * 该函数从关联的文件描述符中注销 poll 回调。调用时必须持有 "mtx" 锁
 * （如果从 ep_free 调用，则必须持有 "epmutex"）。
 */
// 从文件描述符的等待队列中移除并释放所有与特定 epoll 监视项 (epitem) 关联的 poll 回调。
static void ep_unregister_pollwait(struct eventpoll *ep, struct epitem *epi)
{
	struct list_head *lsthead = &epi->pwqlist;  // 获取与 epoll 项关联的 poll 等待队列列表头
	struct eppoll_entry *pwq;

	while (!list_empty(lsthead)) {  // 循环，直到关联的列表为空
		pwq = list_first_entry(lsthead, struct eppoll_entry, llink);  // 获取列表中的第一个等待队列项

		list_del(&pwq->llink);  // 从列表中删除该等待队列项
		remove_wait_queue(pwq->whead, &pwq->wait);  // 从文件的等待队列中移除这个等待队列项
		kmem_cache_free(pwq_cache, pwq);  // 释放等待队列项所使用的内存
	}
}

/**
 * ep_scan_ready_list - Scans the ready list in a way that makes possible for
 *                      the scan code, to call f_op->poll(). Also allows for
 *                      O(NumReady) performance.
 *
 * @ep: Pointer to the epoll private data structure.
 * @sproc: Pointer to the scan callback.
 * @priv: Private opaque data passed to the @sproc callback.
 *
 * Returns: The same integer error code returned by the @sproc callback.
 */
/* ep_scan_ready_list - 以一种能够调用 f_op->poll() 的方式扫描已准备好的列表。
 *                      同时允许 O(NumReady) 的性能。
 *
 * @ep: 指向 epoll 私有数据结构的指针。
 * @sproc: 扫描回调函数的指针。
 * @priv: 传递给 @sproc 回调的私有不透明数据。
 *
 * 返回: 返回由 @sproc 回调返回的相同整数错误代码。
 */
static int ep_scan_ready_list(struct eventpoll *ep,
			      int (*sproc)(struct eventpoll *,
					   struct list_head *, void *),
			      void *priv)
{
	int error, pwake = 0;  // 错误代码变量和用于记录是否需要唤醒的变量
	unsigned long flags;  // 用于保存中断状态的变量
	struct epitem *epi, *nepi;  // 用于遍历事件项的指针
	LIST_HEAD(txlist);  // 初始化一个临时列表头

	/*
	 * We need to lock this because we could be hit by
	 * eventpoll_release_file() and epoll_ctl().
	 */
	/* 锁定 epoll 实例的互斥锁，因为可能同时受到 eventpoll_release_file() 和 epoll_ctl() 的影响。 */
	mutex_lock(&ep->mtx);

	/*
	 * Steal the ready list, and re-init the original one to the
	 * empty list. Also, set ep->ovflist to NULL so that events
	 * happening while looping w/out locks, are not lost. We cannot
	 * have the poll callback to queue directly on ep->rdllist,
	 * because we want the "sproc" callback to be able to do it
	 * in a lockless way.
	 */
	/*
	 * “窃取”就绪列表，并重新初始化原列表为一个空列表。同时，将 ep->ovflist 设置为 NULL，以便在无锁循环中发生的事件不会丢失。
	 * 不能让 poll 回调直接在 ep->rdllist 上排队，因为我们希望 "sproc" 回调能以无锁的方式执行。
	 */
	/* 
	 * 为防止在锁外循环时事件丢失，暂时将就绪列表移到临时列表 txlist，并重置原列表为新的空列表。同时设置 ep->ovflist 为 NULL，
   * 以避免在无锁状态下事件的处理直接影响到 ep->rdllist，因为我们希望 "sproc" 回调能够以无锁的方式操作。 
	 */
	spin_lock_irqsave(&ep->lock, flags);			// 加锁并保存当前中断状态
	list_splice_init(&ep->rdllist, &txlist);	// 将就绪列表移动到临时列表 txlist，并重新初始化就绪列表
	ep->ovflist = NULL;	// 将溢出列表设置为 NULL，确保在处理时不会错过新事件
	spin_unlock_irqrestore(&ep->lock, flags);	// 解锁，并恢复中断状态

	/*
	 * Now call the callback function.
	 */
	/* 调用提供的回调函数 "sproc"，传递 epoll 实例、被转移的就绪列表 txlist 和私有数据 priv，处理就绪事件。 */
	error = (*sproc)(ep, &txlist, priv);

	/* 再次锁定，以处理在 "sproc" 回调期间可能被 poll 回调排队的其他事件。 */
	spin_lock_irqsave(&ep->lock, flags);
	/*
	 * During the time we spent inside the "sproc" callback, some
	 * other events might have been queued by the poll callback.
	 * We re-insert them inside the main ready-list here.
	 */
	/* 
	 * 在 "sproc" 回调执行期间，一些其他事件可能已经被 poll 回调排队。
	 * 我们在这里将它们重新插入主就绪列表。 
	 */
	/* 遍历由 "sproc" 回调期间可能产生的溢出列表 ep->ovflist。此列表包含在回调执行期间进入的新事件。 */
	for (nepi = ep->ovflist; (epi = nepi) != NULL;	// 遍历在回调执行期间可能添加到溢出列表的事件
	     nepi = epi->next, epi->next = EP_UNACTIVE_PTR) {	// 遍历并重置每个事件的 next 指针，防止循环引用
		/*
		 * We need to check if the item is already in the list.
		 * During the "sproc" callback execution time, items are
		 * queued into ->ovflist but the "txlist" might already
		 * contain them, and the list_splice() below takes care of them.
		 */
		/*
     * 检查每个事件项是否已在列表中。在 "sproc" 回调执行期间，事件项可能已被排队到 ->ovflist，
     * 但 "txlist" 可能已包含它们，接下来的 list_splice() 将处理这些事件项。
     */
		if (!ep_is_linked(&epi->rdllink))	// 检查该事件项是否已经链接到就绪列表中
			list_add_tail(&epi->rdllink, &ep->rdllist);	// 如果没有，将其添加到主就绪列表的尾部
	}
	/*
	 * We need to set back ep->ovflist to EP_UNACTIVE_PTR, so that after
	 * releasing the lock, events will be queued in the normal way inside
	 * ep->rdllist.
	 */
	/* 需要将 ep->ovflist 设置回 EP_UNACTIVE_PTR，以便在释放锁后，事件将以正常方式排队到 ep->rdllist 中。 */
	ep->ovflist = EP_UNACTIVE_PTR;

	/*
	 * Quickly re-inject items left on "txlist".
	 */
	/* 快速地将留在 "txlist" 上的项目重新注入。 */
	list_splice(&txlist, &ep->rdllist);

	if (!list_empty(&ep->rdllist)) {
		/*
		 * Wake up (if active) both the eventpoll wait list and
		 * the ->poll() wait list (delayed after we release the lock).
		 */
		/* 如果活跃，则唤醒 eventpoll 等待列表和 ->poll() 等待列表（在释放锁后延迟执行）。 */
		if (waitqueue_active(&ep->wq))
			wake_up_locked(&ep->wq);	// 如果 ep->wq 队列中有活跃的等待者，则唤醒它们
		if (waitqueue_active(&ep->poll_wait))
			pwake++;	// 如果 ep->poll_wait 队列中有活跃的等待者，增加 pwake 计数器
	}
	spin_unlock_irqrestore(&ep->lock, flags);	// 解锁并恢复之前保存的中断状态

	mutex_unlock(&ep->mtx);	// 释放之前获得的互斥锁

	/* We have to call this outside the lock */
	/* 我们必须在锁外调用此操作 */
	if (pwake)
		ep_poll_safewake(&ep->poll_wait);	// 如果有需要唤醒的等待队列，调用 ep_poll_safewake 安全唤醒它们

	return error;	// 返回回调函数传递回来的错误码
}

/*
 * Removes a "struct epitem" from the eventpoll RB tree and deallocates
 * all the associated resources. Must be called with "mtx" held.
 */
/*
 * 从事件轮询红黑树中移除一个 "struct epitem" 并释放所有相关资源。
 * 必须在持有 "mtx" 的情况下调用。 
 */
static int ep_remove(struct eventpoll *ep, struct epitem *epi)
{
	unsigned long flags;		// 用于保存中断状态
	struct file *file = epi->ffd.file;	// 从 epitem 结构中获取关联的文件指针

	/*
	 * Removes poll wait queue hooks. We _have_ to do this without holding
	 * the "ep->lock" otherwise a deadlock might occur. This because of the
	 * sequence of the lock acquisition. Here we do "ep->lock" then the wait
	 * queue head lock when unregistering the wait queue. The wakeup callback
	 * will run by holding the wait queue head lock and will call our callback
	 * that will try to get "ep->lock".
	 */
	/* 
	 * 移除轮询等待队列钩子。这必须在不持有 "ep->lock" 的情况下进行，否则可能发生死锁。
	 * 这是由于锁获取的顺序。在这里，我们在注销等待队列时首先获取 "ep->lock"，然后是等待队列头锁。
	 * 唤醒回调将在持有等待队列头锁的情况下运行，并将调用我们的回调，该回调将尝试获取 "ep->lock"。
	 */
	ep_unregister_pollwait(ep, epi);	// 调用函数以注销等待队列

	/* Remove the current item from the list of epoll hooks */
	/* 从 epoll 钩子列表中移除当前项 */
	spin_lock(&file->f_lock);  // 获取文件的锁
	if (ep_is_linked(&epi->fllink))
		list_del_init(&epi->fllink);  // 如果该项在 fllink 列表中，则从列表中删除
	spin_unlock(&file->f_lock);  // 释放文件的锁

	rb_erase(&epi->rbn, &ep->rbr);  // 从红黑树中删除该项

	spin_lock_irqsave(&ep->lock, flags);  // 获取 epoll 结构的锁，并保存中断状态
	if (ep_is_linked(&epi->rdllink))
		list_del_init(&epi->rdllink);  // 如果该项在 rdllink 列表中，则从列表中删除
	spin_unlock_irqrestore(&ep->lock, flags);  // 释放 epoll 结构的锁，并恢复中断状态

	/* At this point it is safe to free the eventpoll item */
	/* 在这一点上，释放事件轮询项是安全的 */
	kmem_cache_free(epi_cache, epi);	// 释放 epitem 占用的缓存

	atomic_dec(&ep->user->epoll_watches);	// 减少 epoll 监视计数

	return 0;	// 返回 0 表示成功
}

static void ep_free(struct eventpoll *ep)
{
	struct rb_node *rbp;
	struct epitem *epi;

	/* We need to release all tasks waiting for these file */
	/* 我们需要释放所有等待这些文件的任务 */
	if (waitqueue_active(&ep->poll_wait))
		ep_poll_safewake(&ep->poll_wait);

	/*
	 * We need to lock this because we could be hit by
	 * eventpoll_release_file() while we're freeing the "struct eventpoll".
	 * We do not need to hold "ep->mtx" here because the epoll file
	 * is on the way to be removed and no one has references to it
	 * anymore. The only hit might come from eventpoll_release_file() but
	 * holding "epmutex" is sufficent here.
	 */
	/*
	 * 我们需要加锁，因为在释放 "struct eventpoll" 的过程中，可能会受到
	 * eventpoll_release_file() 的影响。这里不需要持有 "ep->mtx"，因为 epoll 文件
	 * 正在被移除，已经没有任何引用了。唯一可能的影响可能来自 eventpoll_release_file()，
	 * 但在这里持有 "epmutex" 就足够了。
	 */
	mutex_lock(&epmutex);

	/*
	 * Walks through the whole tree by unregistering poll callbacks.
	 */
	/* 遍历整个树，注销 poll 回调。 */
	for (rbp = rb_first(&ep->rbr); rbp; rbp = rb_next(rbp)) {
		epi = rb_entry(rbp, struct epitem, rbn);

		ep_unregister_pollwait(ep, epi);
	}

	/*
	 * Walks through the whole tree by freeing each "struct epitem". At this
	 * point we are sure no poll callbacks will be lingering around, and also by
	 * holding "epmutex" we can be sure that no file cleanup code will hit
	 * us during this operation. So we can avoid the lock on "ep->lock".
	 */
	/*
	 * 遍历整个树，释放每个 "struct epitem"。此时，我们确信不会有 poll 回调遗留，
	 * 并且通过持有 "epmutex"，我们可以确保在此操作期间没有文件清理代码会影响我们。
	 * 因此，我们可以避免在 "ep->lock" 上加锁。
	 */
	while ((rbp = rb_first(&ep->rbr)) != NULL) {
		epi = rb_entry(rbp, struct epitem, rbn);
		ep_remove(ep, epi);
	}

	mutex_unlock(&epmutex);  // 解锁
	mutex_destroy(&ep->mtx);  // 销毁互斥锁
	free_uid(ep->user);  // 释放用户 ID
	kfree(ep);  // 释放 epoll 结构本身
}

// 当一个 epoll 文件描述符被关闭时，这个函数会被调用。它负责释放关联的 "struct eventpoll" 结构及其资源。
static int ep_eventpoll_release(struct inode *inode, struct file *file)
{
	struct eventpoll *ep = file->private_data;  // 从文件结构中获取私有数据，即指向 eventpoll 结构的指针

	/* 
	 * 如果私有数据不为空，释放 "eventpoll" 结构。
	 * 这包括从红黑树中移除所有文件描述符并释放所有资源。
	 */
	if (ep)
		ep_free(ep);  // 调用 ep_free 函数来释放 eventpoll 结构

	return 0;  // 返回 0 表示成功
}

/* 这个函数处理已就绪的事件列表，并确定这些事件是否真正符合调用者感兴趣的事件。 */
static int ep_read_events_proc(struct eventpoll *ep, struct list_head *head,
			       void *priv)
{
	struct epitem *epi, *tmp;

	/* 遍历已就绪事件的列表。 */
	list_for_each_entry_safe(epi, tmp, head, rdllink) {
		/* 检查事件文件的 poll 回调是否指示用户请求的事件类型。 */
		if (epi->ffd.file->f_op->poll(epi->ffd.file, NULL) &
		    epi->event.events)
			return POLLIN | POLLRDNORM;	// 如果事件匹配，则返回标准输入和读取正常的事件掩码
		else {
			/*
			 * Item has been dropped into the ready list by the poll
			 * callback, but it's not actually ready, as far as
			 * caller requested events goes. We can remove it here.
			 */
			/* 
			 * 项目已经被 poll 回调放入就绪列表，但就调用者请求的事件而言，
			 * 它实际上并未就绪。我们可以在这里将其移除。
			 */
			list_del_init(&epi->rdllink);  // 从就绪列表中移除该事件
		}
	}

	return 0;	// 如果没有事件匹配，返回0
}

/* 这个函数基于提供的私有数据和回调，处理就绪事件列表 */
static int ep_poll_readyevents_proc(void *priv, void *cookie, int call_nests)
{
	return ep_scan_ready_list(priv, ep_read_events_proc, NULL);
}

/* 这个函数实现了 epoll 文件的 poll 操作。 */
static unsigned int ep_eventpoll_poll(struct file *file, poll_table *wait)
{
	int pollflags;
	struct eventpoll *ep = file->private_data;

	/* Insert inside our poll wait queue */
	/* 将自己插入我们的 poll 等待队列中 */
	poll_wait(file, &ep->poll_wait, wait);

	/*
	 * Proceed to find out if wanted events are really available inside
	 * the ready list. This need to be done under ep_call_nested()
	 * supervision, since the call to f_op->poll() done on listed files
	 * could re-enter here.
	 */
	/*
	 * 继续查找就绪列表中是否真正有所需的事件。
	 * 这需要在 ep_call_nested() 的监督下完成，因为对列出文件的 f_op->poll() 的调用可能会在此重新进入。
	 */
	pollflags = ep_call_nested(&poll_readywalk_ncalls, EP_MAX_NESTS,
				   ep_poll_readyevents_proc, ep, ep, current);

	return pollflags != -1 ? pollflags : 0;
}

/* File callbacks that implement the eventpoll file behaviour */
/* 文件操作回调，实现了 eventpoll 文件的行为 */
static const struct file_operations eventpoll_fops = {
	.release	= ep_eventpoll_release,  // 关闭 epoll 文件时的回调
	.poll		= ep_eventpoll_poll     // 执行 poll 操作时的回调
};

/* Fast test to see if the file is an evenpoll file */
/* 快速检测一个文件是否是 epoll 文件 */
static inline int is_file_epoll(struct file *f)
{
	return f->f_op == &eventpoll_fops;
}

/*
 * This is called from eventpoll_release() to unlink files from the eventpoll
 * interface. We need to have this facility to cleanup correctly files that are
 * closed without being removed from the eventpoll interface.
 */
/* 
 * 这个函数由 eventpoll_release() 调用，用于将文件从 eventpoll 接口中断开链接。
 * 我们需要这个机制来正确清理那些已关闭但未从 eventpoll 接口中移除的文件。
 */
void eventpoll_release_file(struct file *file)
{
	struct list_head *lsthead = &file->f_ep_links;	// 获取与文件相关联的 epoll 链表头
	struct eventpoll *ep;
	struct epitem *epi;

	/*
	 * We don't want to get "file->f_lock" because it is not
	 * necessary. It is not necessary because we're in the "struct file"
	 * cleanup path, and this means that noone is using this file anymore.
	 * So, for example, epoll_ctl() cannot hit here since if we reach this
	 * point, the file counter already went to zero and fget() would fail.
	 * The only hit might come from ep_free() but by holding the mutex
	 * will correctly serialize the operation. We do need to acquire
	 * "ep->mtx" after "epmutex" because ep_remove() requires it when called
	 * from anywhere but ep_free().
	 *
	 * Besides, ep_remove() acquires the lock, so we can't hold it here.
	 */
	/*
	 * 我们不需要获取 "file->f_lock"，因为这是不必要的。这是因为我们处在 "struct file" 的清理路径中，
	 * 这意味着没有人再使用这个文件了。例如，epoll_ctl() 不能在这里操作，因为一旦到达这点，
	 * 文件计数器已经变为零且 fget() 会失败。唯一可能的干扰可能来自 ep_free()，但持有互斥锁
	 * 将正确地序列化操作。我们确实需要在获取 "epmutex" 之后再获取 "ep->mtx"，因为从 ep_free()
	 * 以外的任何地方调用 ep_remove() 时都需要它。
	   
	 * 此外，ep_remove() 需要获取锁，所以我们不能在这里持有它。
	 */
	mutex_lock(&epmutex);  // 锁定全局 epoll 互斥锁

	while (!list_empty(lsthead)) {  // 当列表不为空时循环
		epi = list_first_entry(lsthead, struct epitem, fllink);  // 获取列表中的第一个 epoll 项

		ep = epi->ep;  // 获取该 epoll 项关联的 epoll 实例
		list_del_init(&epi->fllink);  // 从文件的 epoll 链表中移除该 epoll 项，并重置链表节点
		mutex_lock(&ep->mtx);  // 锁定该 epoll 实例的互斥锁
		ep_remove(ep, epi);  // 移除该 epoll 项并清理资源
		mutex_unlock(&ep->mtx);  // 解锁该 epoll 实例的互斥锁
	}

	mutex_unlock(&epmutex);  // 解锁全局 epoll 互斥锁
}

/* 分配并初始化一个新的 eventpoll 结构。 */
static int ep_alloc(struct eventpoll **pep)
{
	int error;  // 用于存储错误代码
	struct user_struct *user;  // 指向当前用户信息的指针
	struct eventpoll *ep;  // 指向新分配的 eventpoll 结构的指针

	/* 获取对当前用户的引用。 */
	user = get_current_user();
	error = -ENOMEM;  // 默认错误为内存不足

	/* 为新的 eventpoll 结构分配内存。 */
	ep = kzalloc(sizeof(*ep), GFP_KERNEL);
	if (unlikely(!ep))
		goto free_uid;  // 如果内存分配失败，则跳转到错误处理部分

	/* 初始化结构的字段。 */
	spin_lock_init(&ep->lock);  // 初始化自旋锁
	mutex_init(&ep->mtx);  // 初始化互斥锁
	init_waitqueue_head(&ep->wq);  // 初始化等待队列头
	init_waitqueue_head(&ep->poll_wait);  // 初始化 poll 等待队列头
	INIT_LIST_HEAD(&ep->rdllist);  // 初始化就绪列表
	ep->rbr = RB_ROOT;  // 初始化红黑树根
	ep->ovflist = EP_UNACTIVE_PTR;  // 初始化溢出列表指针
	ep->user = user;  // 将当前用户信息关联到 eventpoll 结构

	*pep = ep;  // 将新创建的 eventpoll 结构赋值给输出参数

	return 0;  // 返回成功

free_uid:
	/* 如果在引用用户之后出现错误，释放用户引用。 */
	free_uid(user);
	return error;  // 返回错误代码
}

/*
 * Search the file inside the eventpoll tree. The RB tree operations
 * are protected by the "mtx" mutex, and ep_find() must be called with
 * "mtx" held.
 */
/*
 * 在 eventpoll 树中搜索文件。红黑树操作受到 "mtx" 互斥锁的保护，
 * 并且调用 ep_find() 时必须持有 "mtx"。
 */
static struct epitem *ep_find(struct eventpoll *ep, struct file *file, int fd)
{
	int kcmp;  // 用于比较文件描述符的结果
	struct rb_node *rbp;  // 当前红黑树节点指针
	struct epitem *epi, *epir = NULL;  // epitem 指针和结果指针
	struct epoll_filefd ffd;  // 封装文件描述符和文件指针的结构

	/* 设置 ffd 结构，包含文件指针和文件描述符 */
	ep_set_ffd(&ffd, file, fd);

	/* 遍历红黑树，寻找匹配的文件描述符 */
	for (rbp = ep->rbr.rb_node; rbp; ) {
		epi = rb_entry(rbp, struct epitem, rbn);  // 从树节点获取 epitem
		kcmp = ep_cmp_ffd(&ffd, &epi->ffd);  // 比较当前 epitem 的文件描述符与目标描述符

		if (kcmp > 0)  // 如果目标大于当前节点
			rbp = rbp->rb_right;  // 向右子树移动
		else if (kcmp < 0)  // 如果目标小于当前节点
			rbp = rbp->rb_left;  // 向左子树移动
		else {
			epir = epi;  // 找到匹配项，设置返回值
			break;  // 跳出循环
		}
	}

	return epir;  // 返回找到的 epitem，如果未找到则为 NULL
}

/*
 * This is the callback that is passed to the wait queue wakeup
 * machanism. It is called by the stored file descriptors when they
 * have events to report.
 */
/* 这是传递给等待队列唤醒机制的回调函数。当存储的文件描述符有事件报告时会被调用。 */
static int ep_poll_callback(wait_queue_t *wait, unsigned mode, int sync, void *key)
{
	int pwake = 0;  // 用于记录是否需要唤醒的标志
	unsigned long flags;  // 用于保存中断状态
	struct epitem *epi = ep_item_from_wait(wait);  // 从等待队列项获取对应的 epitem
	struct eventpoll *ep = epi->ep;  // 获取 epitem 所属的 eventpoll 结构

	spin_lock_irqsave(&ep->lock, flags);  // 加锁并保存中断状态

	/*
	 * If the event mask does not contain any poll(2) event, we consider the
	 * descriptor to be disabled. This condition is likely the effect of the
	 * EPOLLONESHOT bit that disables the descriptor when an event is received,
	 * until the next EPOLL_CTL_MOD will be issued.
	 */
	/* 
	 * 如果事件掩码不包含任何 poll(2) 事件，我们认为描述符被禁用。
	 * 这种情况很可能是 EPOLLONESHOT 位的效果，该位在接收到事件时禁用描述符，
	 * 直到下一个 EPOLL_CTL_MOD 被发出。
	 */
	if (!(epi->event.events & ~EP_PRIVATE_BITS))
		goto out_unlock;	// 如果没有有效的事件，直接跳转到解锁部分

	/*
	 * Check the events coming with the callback. At this stage, not
	 * every device reports the events in the "key" parameter of the
	 * callback. We need to be able to handle both cases here, hence the
	 * test for "key" != NULL before the event match test.
	 */
	/*
	 * 检查随回调到来的事件。在这个阶段，并非每个设备都在回调的 "key" 参数中报告事件。
	 * 我们需要在这里处理两种情况，因此在事件匹配测试之前测试 "key" != NULL。
	 */
	if (key && !((unsigned long) key & epi->event.events))
		goto out_unlock;	// 如果提供的 key 与事件不匹配，跳转到解锁部分

	/*
	 * If we are trasfering events to userspace, we can hold no locks
	 * (because we're accessing user memory, and because of linux f_op->poll()
	 * semantics). All the events that happens during that period of time are
	 * chained in ep->ovflist and requeued later on.
	 */
	/*
	 * 如果我们正在将事件传输到用户空间，我们不能持有锁
	 * （因为我们正在访问用户内存，并且由于 Linux f_op->poll() 的语义）。
	 * 在那段时间发生的所有事件都被链在 ep->ovflist 中，并稍后重新排队。
	 */
	if (unlikely(ep->ovflist != EP_UNACTIVE_PTR)) {
		if (epi->next == EP_UNACTIVE_PTR) {
			epi->next = ep->ovflist;
			ep->ovflist = epi;
		}
		goto out_unlock;
	}

	/* If this file is already in the ready list we exit soon */
	/* 如果这个文件已经在就绪列表中，我们很快就退出 */
	if (!ep_is_linked(&epi->rdllink))
		list_add_tail(&epi->rdllink, &ep->rdllist);

	/*
	 * Wake up ( if active ) both the eventpoll wait list and the ->poll()
	 * wait list.
	 */
	/* 如果活跃，唤醒 eventpoll 等待列表和 ->poll() 等待列表。 */
	if (waitqueue_active(&ep->wq))
		wake_up_locked(&ep->wq);
	if (waitqueue_active(&ep->poll_wait))
		pwake++;

out_unlock:
	spin_unlock_irqrestore(&ep->lock, flags);	// 解锁并恢复中断状态

	/* We have to call this outside the lock */
	/* 我们必须在锁外调用这个 */
	if (pwake)
		ep_poll_safewake(&ep->poll_wait);

	return 1;	// 回调总是返回 1，表示处理成功
}

/*
 * This is the callback that is used to add our wait queue to the
 * target file wakeup lists.
 */
/* 这是一个回调函数，用于将我们的等待队列添加到目标文件的唤醒列表中。 */
static void ep_ptable_queue_proc(struct file *file, wait_queue_head_t *whead,
				 poll_table *pt)
{
	struct epitem *epi = ep_item_from_epqueue(pt);  // 从 poll_table 获取关联的 epitem
	struct eppoll_entry *pwq;  // 用于创建新的等待队列项

	if (epi->nwait >= 0 && (pwq = kmem_cache_alloc(pwq_cache, GFP_KERNEL))) {
		// 如果还没有错误，并且能够从内存缓存中分配一个新的等待队列项
		init_waitqueue_func_entry(&pwq->wait, ep_poll_callback);  // 初始化等待队列项，并设置回调函数
		pwq->whead = whead;  // 设置等待队列头
		pwq->base = epi;  // 设置等待队列项基础指向当前的 epitem
		add_wait_queue(whead, &pwq->wait);  // 将新的等待队列项添加到目标文件的等待队列中
		list_add_tail(&pwq->llink, &epi->pwqlist);  // 将新的等待队列项添加到 epitem 的等待队列列表中
		epi->nwait++;  // 增加 epitem 上的等待计数
	} else {
		/* We have to signal that an error occurred */
		/* 我们需要标记发生了错误 */
		epi->nwait = -1;  // 设置 nwait 为 -1，表示发生错误
	}
}

static void ep_rbtree_insert(struct eventpoll *ep, struct epitem *epi)
{
	int kcmp;  // 用于存储键比较结果
	struct rb_node **p = &ep->rbr.rb_node, *parent = NULL;  // 指针p用于遍历树，parent用于记录p的父节点
	struct epitem *epic;  // 用来引用当前比较的树中的事件项

	/* 遍历树以找到新节点的正确位置。 */
	while (*p) {
		parent = *p;  // 记录当前节点作为后续插入操作的父节点
		epic = rb_entry(parent, struct epitem, rbn);  // 从红黑树节点获取事件项结构
		kcmp = ep_cmp_ffd(&epi->ffd, &epic->ffd);  // 比较文件描述符，确定插入位置

		if (kcmp > 0)  // 如果新节点键值大于当前节点键值
			p = &parent->rb_right;  // 向右子树移动
		else  // 如果新节点键值小于或等于当前节点键值
			p = &parent->rb_left;  // 向左子树移动
	}

	/* 将新节点插入到树中。 */
	rb_link_node(&epi->rbn, parent, p);  // 将新节点与父节点链接
	rb_insert_color(&epi->rbn, &ep->rbr);  // 调整红黑树的颜色属性，维持树平衡
}

/*
 * Must be called with "mtx" held.
 */
/* 必须在持有 "mtx" 的情况下调用。 */
// 将新的事件项添加到 epoll 集合中。
static int ep_insert(struct eventpoll *ep, struct epoll_event *event,
		     struct file *tfile, int fd)
{
	int error, revents, pwake = 0; // 定义错误代码、事件结果和唤醒标志变量
	unsigned long flags; // 用于后续的锁操作
	struct epitem *epi; // 指向新创建的事件项的指针
	struct ep_pqueue epq; // 用于注册的 poll 回调队列结构

	/* 检查 epoll 监视的数量是否超过用户允许的最大值。 */
	if (unlikely(atomic_read(&ep->user->epoll_watches) >=
		     max_user_watches))
		return -ENOSPC;	// 如果超过了，返回没有空间的错误
	/* 从缓存中分配一个新的 epitem。 */
	if (!(epi = kmem_cache_alloc(epi_cache, GFP_KERNEL)))
		return -ENOMEM;	// 分配失败返回内存不足的错误

	/* Item initialization follow here ... */
	/* 初始化新的 epitem。 */
	INIT_LIST_HEAD(&epi->rdllink); // 初始化事件项的就绪链表头
	INIT_LIST_HEAD(&epi->fllink); // 初始化事件项的文件链表头
	INIT_LIST_HEAD(&epi->pwqlist); // 初始化事件项的等待队列链表头
	epi->ep = ep; // 将事件项的父 epoll 实例指向当前 epoll 实例
	/* 设置与 epoll 事件相关联的文件描述符和文件指针。 */
	ep_set_ffd(&epi->ffd, tfile, fd); // 设置事件项的文件描述符
	epi->event = *event; // 复制传入的事件结构到事件项
	epi->nwait = 0; // 初始化等待计数为0
	epi->next = EP_UNACTIVE_PTR; // 设置事件项的下一个指针为非激活指针

	/* Initialize the poll table using the queue callback */
	/* 使用队列回调初始化 poll 表。 */
	epq.epi = epi;	// 设置 poll 队列的事件项指针为当前事件项
	init_poll_funcptr(&epq.pt, ep_ptable_queue_proc);	// 初始化 poll 表的函数指针为 epoll 的队列处理回调

	/*
	 * Attach the item to the poll hooks and get current event bits.
	 * We can safely use the file* here because its usage count has
	 * been increased by the caller of this function. Note that after
	 * this operation completes, the poll callback can start hitting
	 * the new item.
	 */
	/* 
	 * 将项目挂接到 poll 钩子上并获取当前事件位。
	 * 我们可以安全地使用这里的文件指针，因为它的使用计数已经由这个函数的调用者增加。
	 * 注意，这个操作完成后，poll 回调可以开始响应新的项目。
	 */
	revents = tfile->f_op->poll(tfile, &epq.pt);	// 将项挂接到 poll 钩子并获取当前事件位。

	/*
	 * We have to check if something went wrong during the poll wait queue
	 * install process. Namely an allocation for a wait queue failed due
	 * high memory pressure.
	 */
	/*
	 * 我们必须检查在 poll 等待队列安装过程中是否出了问题。
	 * 即由于高内存压力导致等待队列的分配失败。
	 */
	error = -ENOMEM;
	if (epi->nwait < 0)	// 检查在 poll 等待队列安装过程中是否发生错误。
		goto error_unregister;

	/* Add the current item to the list of active epoll hook for this file */
	/* 将新的 epitem 链接到文件的 epoll 钩子列表中。 */
	spin_lock(&tfile->f_lock);
	list_add_tail(&epi->fllink, &tfile->f_ep_links);
	spin_unlock(&tfile->f_lock);

	/*
	 * Add the current item to the RB tree. All RB tree operations are
	 * protected by "mtx", and ep_insert() is called with "mtx" held.
	 */
	/*
	 * 将当前项添加到红黑树中。所有红黑树操作都受到 "mtx" 保护，
	 * 并且调用 ep_insert() 时持有 "mtx"。
	 */
	ep_rbtree_insert(ep, epi);	// 将新的 epitem 插入到红黑树中。

	/* We have to drop the new item inside our item list to keep track of it */
	/* 将新的 epitem 插入我们的列表中以跟踪它。 */
	spin_lock_irqsave(&ep->lock, flags);

	/* If the file is already "ready" we drop it inside the ready list */
	/* 如果文件已经“就绪”，我们将其放入就绪列表中 */
	if ((revents & event->events) && !ep_is_linked(&epi->rdllink)) {
		list_add_tail(&epi->rdllink, &ep->rdllist);	// 将事件项添加到就绪列表的尾部

		/* Notify waiting tasks that events are available */
		/* 通知等待任务事件已可用 */
		if (waitqueue_active(&ep->wq))
			wake_up_locked(&ep->wq);  // 唤醒在 ep->wq 上等待的任务
		if (waitqueue_active(&ep->poll_wait))
			pwake++;  // 如果在 poll_wait 队列上有活动，增加唤醒计数
	}

	spin_unlock_irqrestore(&ep->lock, flags);	// 解锁并恢复之前保存的中断状态

	atomic_inc(&ep->user->epoll_watches);			// 增加 epoll 监视计数器

	/* We have to call this outside the lock */
	/* 我们必须在锁外调用这个 */
	if (pwake)
		ep_poll_safewake(&ep->poll_wait);	// 如果需要唤醒，调用 ep_poll_safewake

	return 0;	// 成功执行，返回 0

error_unregister:
	/* 如果出现错误，取消注册 poll 等待并进行清理。 */
	ep_unregister_pollwait(ep, epi);	// 注销与事件项相关联的 poll 等待队列

	/*
	 * We need to do this because an event could have been arrived on some
	 * allocated wait queue. Note that we don't care about the ep->ovflist
	 * list, since that is used/cleaned only inside a section bound by "mtx".
	 * And ep_insert() is called with "mtx" held.
	 */
	/*
	 * 我们需要这样做，因为可能有事件到达了某个已分配的等待队列。
	 * 注意，我们不关心 ep->ovflist 列表，因为这个列表仅在持有 "mtx" 的部分中使用和清理。
	 * 而 ep_insert() 是在持有 "mtx" 的情况下调用的。
	 */
	spin_lock_irqsave(&ep->lock, flags);  // 再次锁定并保存中断状态
	if (ep_is_linked(&epi->rdllink))
		list_del_init(&epi->rdllink);  // 如果事件项仍在就绪列表中，将其删除
	spin_unlock_irqrestore(&ep->lock, flags);  // 解锁并恢复中断状态

	kmem_cache_free(epi_cache, epi);  // 释放事件项所占用的内存

	return error;  // 返回错误代码
}

/*
 * Modify the interest event mask by dropping an event if the new mask
 * has a match in the current file status. Must be called with "mtx" held.
 */
/*
 * 修改感兴趣的事件掩码，如果新掩码与当前文件状态匹配，则丢弃一个事件。
 * 必须在持有 "mtx" 的情况下调用。
 */
static int ep_modify(struct eventpoll *ep, struct epitem *epi, struct epoll_event *event)
{
	int pwake = 0;	// 用于记录是否需要唤醒的标志
	unsigned int revents;	// 当前事件状态

	/*
	 * Set the new event interest mask before calling f_op->poll();
	 * otherwise we might miss an event that happens between the
	 * f_op->poll() call and the new event set registering.
	 */
	 /* 
	 * 在调用 f_op->poll() 之前设置新的事件兴趣掩码；
   * 否则我们可能会错过在 f_op->poll() 调用和新事件集注册之间发生的事件。
	 */
	epi->event.events = event->events;  // 更新事件项的事件掩码
	// 更新事件数据，受 mtx 保护
	epi->event.data = event->data; /* protected by mtx */

	/*
	 * Get current event bits. We can safely use the file* here because
	 * its usage count has been increased by the caller of this function.
	 */
	/*
	 * 获取当前事件位。我们可以在这里安全地使用文件指针，
   * 因为它的使用计数已由此函数的调用者增加。
	 */
	revents = epi->ffd.file->f_op->poll(epi->ffd.file, NULL);

	/*
	 * If the item is "hot" and it is not registered inside the ready
	 * list, push it inside.
	 */
	/* 如果项是“热”的，并且尚未注册在就绪列表中，将其推入列表。 */
	if (revents & event->events) {
		spin_lock_irq(&ep->lock);	// 加锁
		if (!ep_is_linked(&epi->rdllink)) {
			list_add_tail(&epi->rdllink, &ep->rdllist);	 // 将事件项添加到就绪列表

			/* Notify waiting tasks that events are available */
			/* 通知等待任务事件已可用 */
			if (waitqueue_active(&ep->wq))
				wake_up_locked(&ep->wq);	// 唤醒等待的任务
			if (waitqueue_active(&ep->poll_wait))
				pwake++;	// 增加唤醒计数
		}
		spin_unlock_irq(&ep->lock);	// 解锁
	}

	/* We have to call this outside the lock */
	/* 我们必须在锁外调用这个 */
	if (pwake)
		ep_poll_safewake(&ep->poll_wait);	// 如果需要唤醒，调用 ep_poll_safewake

	return 0;	// 返回成功
}

static int ep_send_events_proc(struct eventpoll *ep, struct list_head *head,
			       void *priv)
{
	struct ep_send_events_data *esed = priv;  // 引用传递给函数的私有数据结构
	int eventcnt;  // 已处理的事件计数
	unsigned int revents;  // 存储单个事件项的结果事件掩码
	struct epitem *epi;  // 指向事件项的指针
	struct epoll_event __user *uevent;  // 指向用户空间的事件数据指针

	/*
	 * We can loop without lock because we are passed a task private list.
	 * Items cannot vanish during the loop because ep_scan_ready_list() is
	 * holding "mtx" during this call.
	 */
	/*
	 * 我们可以在没有锁的情况下循环，因为我们传递了一个任务私有列表。
	 * 由于 ep_scan_ready_list() 在此调用期间持有 "mtx"，因此项在循环中不会消失。
	 */
	for (eventcnt = 0, uevent = esed->events;
	     !list_empty(head) && eventcnt < esed->maxevents;) {
		epi = list_first_entry(head, struct epitem, rdllink);	// 获取列表中的第一个事件项

		list_del_init(&epi->rdllink);	// 从就绪列表中删除该事件项

		revents = epi->ffd.file->f_op->poll(epi->ffd.file, NULL) &
			epi->event.events;	// 调用文件的 poll 方法重新检测事件

		/*
		 * If the event mask intersect the caller-requested one,
		 * deliver the event to userspace. Again, ep_scan_ready_list()
		 * is holding "mtx", so no operations coming from userspace
		 * can change the item.
		 */
		/* 
		 * 如果事件掩码与调用者请求的掩码相交，将事件传递到用户空间。
		 * 再次强调，ep_scan_ready_list() 正在持有 "mtx"，
		 * 因此任何来自用户空间的操作都无法更改该项。
		 */
		if (revents) {
			if (__put_user(revents, &uevent->events) ||
			    __put_user(epi->event.data, &uevent->data)) {
				list_add(&epi->rdllink, head);	// 如果复制到用户空间失败，将事件项重新添加到列表
				return eventcnt ? eventcnt : -EFAULT;	// 返回已处理的事件计数或错误
			}
			eventcnt++;	// 增加已处理的事件计数
			uevent++;	// 移动用户空间的事件指针
			if (epi->event.events & EPOLLONESHOT)
				epi->event.events &= EP_PRIVATE_BITS;	// 处理一次性事件
			else if (!(epi->event.events & EPOLLET)) {
				/*
				 * If this file has been added with Level
				 * Trigger mode, we need to insert back inside
				 * the ready list, so that the next call to
				 * epoll_wait() will check again the events
				 * availability. At this point, noone can insert
				 * into ep->rdllist besides us. The epoll_ctl()
				 * callers are locked out by
				 * ep_scan_ready_list() holding "mtx" and the
				 * poll callback will queue them in ep->ovflist.
				 */
				/*
				 * 如果这个文件是以水平触发模式添加的，
				 * 我们需要将其再次插回就绪列表中，以便下一次调用 epoll_wait() 时再次检查事件可用性。
				 * 在这一点上，除我们之外没有人可以插入到 ep->rdllist 中。
				 * epoll_ctl() 的调用者通过持有 "mtx" 而被锁定，poll 回调会将它们排队到 ep->ovflist 中。
				 */
				list_add_tail(&epi->rdllink, &ep->rdllist);
			}
		}
	}

	return eventcnt;	// 返回已处理的事件总数
}

/* 这个函数从 eventpoll 结构中获取准备好的事件，并将它们传递给调用者。 */
static int ep_send_events(struct eventpoll *ep,
			  struct epoll_event __user *events, int maxevents)
{
	struct ep_send_events_data esed;	// 定义一个结构体用于传递给回调函数的数据

	/* 设置用户希望接收的最大事件数量和事件应该存储的用户空间缓冲区的指针。 */
	esed.maxevents = maxevents;  // 设置用户期望接收的最大事件数
	esed.events = events;  // 设置指向用户空间事件缓冲区的指针

	/* 扫描就绪列表并将事件发送到用户空间。 */
	return ep_scan_ready_list(ep, ep_send_events_proc, &esed);  // 调用 ep_scan_ready_list 函数，传递回调函数和数据结构
}

static int ep_poll(struct eventpoll *ep, struct epoll_event __user *events,
		   int maxevents, long timeout)
{
	int res, eavail;  // res 用于存储结果，eavail 用于标识事件可用性
	unsigned long flags;  // 用于保护临界区的旗标
	long jtimeout;  // 转换后的超时值，以 jiffies 计
	wait_queue_t wait;  // 等待队列项

	/*
	 * Calculate the timeout by checking for the "infinite" value (-1)
	 * and the overflow condition. The passed timeout is in milliseconds,
	 * that why (t * HZ) / 1000.
	 */
	/*
	 * 通过检查“无限”值 (-1) 和溢出条件来计算超时。
	 * 传入的超时值以毫秒为单位，这就是为什么使用 (timeout * HZ) / 1000 的原因。
	 */
	jtimeout = (timeout < 0 || timeout >= EP_MAX_MSTIMEO) ?
		MAX_SCHEDULE_TIMEOUT : (timeout * HZ + 999) / 1000;	// 将毫秒转换为系统节拍

retry:
	spin_lock_irqsave(&ep->lock, flags);	// 加锁，保护临界区

	res = 0;	// 初始化结果为 0
	if (list_empty(&ep->rdllist)) {
		/*
		 * We don't have any available event to return to the caller.
		 * We need to sleep here, and we will be wake up by
		 * ep_poll_callback() when events will become available.
		 */
		/*
		 * 我们没有任何可用的事件返回给调用者。
		 * 我们需要在这里休眠，当事件变得可用时，ep_poll_callback() 将唤醒我们。
		 */
		init_waitqueue_entry(&wait, current);
		wait.flags |= WQ_FLAG_EXCLUSIVE;
		__add_wait_queue(&ep->wq, &wait);	// 将当前进程添加到等待队列

		for (;;) {
			/*
			 * We don't want to sleep if the ep_poll_callback() sends us
			 * a wakeup in between. That's why we set the task state
			 * to TASK_INTERRUPTIBLE before doing the checks.
			 */
			/*
			 * 如果 ep_poll_callback() 在此期间发送唤醒信号，我们不想睡眠。
			 * 这就是为什么在进行检查之前将任务状态设置为 TASK_INTERRUPTIBLE。
			 */
			set_current_state(TASK_INTERRUPTIBLE);
			if (!list_empty(&ep->rdllist) || !jtimeout)
				break;
			if (signal_pending(current)) {
				res = -EINTR;
				break;
			}

			spin_unlock_irqrestore(&ep->lock, flags);	// 释放锁并恢复之前的中断状态
			jtimeout = schedule_timeout(jtimeout);		// 调度休眠直到指定的超时或事件发生
			spin_lock_irqsave(&ep->lock, flags);			// 重新获取锁并保存当前中断状态
		}
		__remove_wait_queue(&ep->wq, &wait);				// 移除等待队列

		set_current_state(TASK_RUNNING);						// 将任务状态设置回 TASK_RUNNING
	}
	/* Is it worth to try to dig for events ? */
	/* 值得尝试寻找事件吗？ */
	eavail = !list_empty(&ep->rdllist) || ep->ovflist != EP_UNACTIVE_PTR;

	spin_unlock_irqrestore(&ep->lock, flags);	// 释放锁并恢复之前的中断状态

	/*
	 * Try to transfer events to user space. In case we get 0 events and
	 * there's still timeout left over, we go trying again in search of
	 * more luck.
	 */
	/*
	 * 尝试将事件传输到用户空间。如果我们得到 0 个事件并且还有剩余的超时时间，
	 * 我们将再次尝试以寻找更多的事件。
	 */
	if (!res && eavail &&
	    !(res = ep_send_events(ep, events, maxevents)) && jtimeout)
		goto retry;

	return res;	// 返回结果
}

/*
 * Open an eventpoll file descriptor.
 */
/* 打开一个 eventpoll 文件描述符 */
SYSCALL_DEFINE1(epoll_create1, int, flags)
{
	int error;  // 用于存储错误代码
	struct eventpoll *ep = NULL;  // 定义一个指向 eventpoll 结构的指针，初始化为空

	/* Check the EPOLL_* constant for consistency.  */
	/* 检查 EPOLL_* 常量的一致性 */
	BUILD_BUG_ON(EPOLL_CLOEXEC != O_CLOEXEC);	// 编译时检查 EPOLL_CLOEXEC 是否等于 O_CLOEXEC

	if (flags & ~EPOLL_CLOEXEC)
		return -EINVAL;	// 如果传入的标志位包含无效标志，返回 -EINVAL
	/*
	 * Create the internal data structure ("struct eventpoll").
	 */
	/* 创建内部数据结构（"struct eventpoll"）。 */
	error = ep_alloc(&ep);	// 调用 ep_alloc 函数分配 eventpoll 结构
	if (error < 0)
		return error;	// 如果分配失败，返回错误代码
	/*
	 * Creates all the items needed to setup an eventpoll file. That is,
	 * a file structure and a free file descriptor.
	 */
	/* 创建设置 eventpoll 文件所需的所有项目。这包括一个文件结构和一个空闲文件描述符。 */
	error = anon_inode_getfd("[eventpoll]", &eventpoll_fops, ep,
				 O_RDWR | (flags & O_CLOEXEC));	// 获取匿名 inode 文件描述符并绑定到 eventpoll 实例
	if (error < 0)
		ep_free(ep);	// 如果获取文件描述符失败，释放已分配的 eventpoll 结构

	return error;		// 返回文件描述符或错误代码
}

SYSCALL_DEFINE1(epoll_create, int, size)
{
	// 检查 size 参数是否有效。如果 size 小于等于 0，则返回错误代码 -EINVAL（无效参数）。
	if (size <= 0)
		return -EINVAL;

	// 创建 epoll 实例的实际工作交由 sys_epoll_create1 完成。在较新的内核中，'size' 参数已被忽略，因此可以安全地将 0 作为标志参数传递。
	return sys_epoll_create1(0);
}

/*
 * The following function implements the controller interface for
 * the eventpoll file that enables the insertion/removal/change of
 * file descriptors inside the interest set.
 */
/*
 * 下面的函数实现了事件轮询文件的控制器接口，
 * 使得可以在关注的文件描述符集合中插入、移除或修改文件描述符。
 */
SYSCALL_DEFINE4(epoll_ctl, int, epfd, int, op, int, fd,
		struct epoll_event __user *, event)
{
	int error;  // 错误码变量
	struct file *file, *tfile;  // 指向文件的指针
	struct eventpoll *ep;  // 指向 eventpoll 结构的指针
	struct epitem *epi;  // 指向 epoll 项的指针
	struct epoll_event epds;  // 用于存储从用户空间拷贝的 epoll 事件数据

	error = -EFAULT;  // 默认错误码为 -EFAULT（错误的地址）
	if (ep_op_has_event(op) &&
	    copy_from_user(&epds, event, sizeof(struct epoll_event)))
		goto error_return;  // 从用户空间复制 epoll_event 数据，出错则跳转到错误处理

	/* Get the "struct file *" for the eventpoll file */
	/* 获取 eventpoll 文件的 "struct file *" */
	error = -EBADF;  // 文件描述符无效的错误码
	file = fget(epfd);  // 根据文件描述符获取文件结构
	if (!file)
		goto error_return;  // 文件结构获取失败，跳转到错误返回

	/* Get the "struct file *" for the target file */
	/* 获取目标文件的 "struct file *" */
	tfile = fget(fd);  // 获取目标文件的文件结构
	if (!tfile)
		goto error_fput;  // 获取失败，跳转到释放文件描述符的错误处理

	/* The target file descriptor must support poll */
	/* 目标文件描述符必须支持 poll 操作 */
	error = -EPERM;  // 不允许的操作错误码
	if (!tfile->f_op || !tfile->f_op->poll)	// 如果目标文件不支持 poll 操作
		goto error_tgt_fput;  // 操作或poll为空，跳转到目标文件错误处理

	/*
	 * We have to check that the file structure underneath the file descriptor
	 * the user passed to us _is_ an eventpoll file. And also we do not permit
	 * adding an epoll file descriptor inside itself.
	 */
	/*
	 * 我们必须检查用户传递给我们的文件描述符下的文件结构是否是一个 eventpoll 文件。
	 * 并且我们不允许在 epoll 文件描述符内部添加自己。
	 */
	error = -EINVAL;  // 无效参数错误码
	if (file == tfile || !is_file_epoll(file))
		goto error_tgt_fput;  // 文件相同或不是 epoll 文件，跳转到目标文件错误处理

	/*
	 * At this point it is safe to assume that the "private_data" contains
	 * our own data structure.
	 */
	/* 在这一点上，可以安全地假设 "private_data" 包含我们自己的数据结构。 */
	ep = file->private_data;	// 获取私有数据，即 epoll 实例

	mutex_lock(&ep->mtx);	// 锁定互斥锁

	/*
	 * Try to lookup the file inside our RB tree, Since we grabbed "mtx"
	 * above, we can be sure to be able to use the item looked up by
	 * ep_find() till we release the mutex.
	 */
	/*
	 * 尝试在我们的红黑树中查找文件，由于我们上面获取了 "mtx"，
	 * 我们可以确保在释放互斥锁之前能够使用 ep_find() 查找到的项。
	 */
	epi = ep_find(ep, tfile, fd);	// 在红黑树中查找文件描述符

	error = -EINVAL;  // 重新设置无效参数错误码
	switch (op) {	// 根据操作类型分支处理
	case EPOLL_CTL_ADD:  // 添加操作
		if (!epi) {	// 如果文件描述符不在红黑树中
			epds.events |= POLLERR | POLLHUP;	// 设置错误和挂起状态
			error = ep_insert(ep, &epds, tfile, fd);	// 尝试插入文件描述符
		} else
			error = -EEXIST;  // 已存在错误
		break;
	case EPOLL_CTL_DEL:  // 删除操作
		if (epi)	// 如果找到了文件描述符
			error = ep_remove(ep, epi);	// 移除文件描述符
		else
			error = -ENOENT;  // 未找到错误
		break;
	case EPOLL_CTL_MOD:  // 修改操作
		if (epi) {	// 如果找到了文件描述符
			epds.events |= POLLERR | POLLHUP;	// 设置错误和挂起状态
			error = ep_modify(ep, epi, &epds);	// 修改文件描述符
		} else
			error = -ENOENT;  // 未找到错误
		break;
	}
	mutex_unlock(&ep->mtx);  // 解锁互斥锁

error_tgt_fput:
	fput(tfile);	// 释放目标文件的引用
error_fput:
	fput(file);		// 释放 epoll 文件的引用
error_return:

	return error;	// 返回错误码
}

/*
 * Implement the event wait interface for the eventpoll file. It is the kernel
 * part of the user space epoll_wait(2).
 */
/* 实现事件轮询文件的事件等待接口。这是用户空间 epoll_wait(2) 的内核部分。 */
SYSCALL_DEFINE4(epoll_wait, int, epfd, struct epoll_event __user *, events,
		int, maxevents, int, timeout)
{
	int error;  // 错误码变量
	struct file *file;  // 指向文件的指针
	struct eventpoll *ep;  // 指向事件轮询结构的指针

	/* The maximum number of event must be greater than zero */
	/* 最大事件数必须大于零 */
	if (maxevents <= 0 || maxevents > EP_MAX_EVENTS)
		return -EINVAL;	// 如果最大事件数非法，则返回 -EINVAL

	/* Verify that the area passed by the user is writeable */
	/* 验证用户传递的区域是否可写 */
	if (!access_ok(VERIFY_WRITE, events, maxevents * sizeof(struct epoll_event))) {
		error = -EFAULT;
		goto error_return;	// 如果无法写入事件数组，返回 -EFAULT
	}

	/* Get the "struct file *" for the eventpoll file */
	/* 获取事件轮询文件的 "struct file *" */
	error = -EBADF;  // 初始化错误代码为 -EBADF
	file = fget(epfd);  // 根据文件描述符获取文件结构
	if (!file)  // 如果文件结构获取失败
		goto error_return;  // 跳转到错误返回

	/*
	 * We have to check that the file structure underneath the fd
	 * the user passed to us _is_ an eventpoll file.
	 */
	/* 我们必须检查用户传递给我们的文件描述符下的文件结构是否是一个事件轮询文件。 */
	error = -EINVAL;  // 初始化错误代码为 -EINVAL
	if (!is_file_epoll(file))  // 如果不是事件轮询文件
		goto error_fput;  // 跳转到释放文件引用的错误处理

	/*
	 * At this point it is safe to assume that the "private_data" contains
	 * our own data structure.
	 */
	/* 在这一点上，可以安全地假设 "private_data" 包含我们自己的数据结构。 */
	ep = file->private_data;  // 获取私有数据，即事件轮询结构

	/* Time to fish for events ... */
	/* 是时候寻找事件了… */
	error = ep_poll(ep, events, maxevents, timeout);  // 调用 ep_poll 来等待事件

error_fput:
	fput(file);	// 释放文件的引用
error_return:

	return error;	// 返回错误码
}

#ifdef HAVE_SET_RESTORE_SIGMASK

/*
 * Implement the event wait interface for the eventpoll file. It is the kernel
 * part of the user space epoll_pwait(2).
 */
/* 实现事件轮询文件的事件等待接口。这是用户空间 epoll_pwait(2) 的内核部分。 */
SYSCALL_DEFINE6(epoll_pwait, int, epfd, struct epoll_event __user *, events,
		int, maxevents, int, timeout, const sigset_t __user *, sigmask,
		size_t, sigsetsize)
{
	int error;  // 用于存储错误代码
	sigset_t ksigmask, sigsaved;  // 用于存储内核中的信号掩码和保存的信号掩码

	/*
	 * If the caller wants a certain signal mask to be set during the wait,
	 * we apply it here.
	 */
	/* 如果调用者希望在等待期间设置特定的信号掩码，我们在这里应用它。 */
	if (sigmask) {
		if (sigsetsize != sizeof(sigset_t))
			return -EINVAL;  // 如果信号集大小不正确，返回 -EINVAL
		if (copy_from_user(&ksigmask, sigmask, sizeof(ksigmask)))
			return -EFAULT;  // 如果从用户空间复制信号掩码失败，返回 -EFAULT
		sigdelsetmask(&ksigmask, sigmask(SIGKILL) | sigmask(SIGSTOP));
		// 设置新的信号掩码，同时保持 SIGKILL 和 SIGSTOP 无法被阻塞
		sigprocmask(SIG_SETMASK, &ksigmask, &sigsaved);
	}

	// 调用 sys_epoll_wait 来实际执行等待操作
	error = sys_epoll_wait(epfd, events, maxevents, timeout);

	/*
	 * If we changed the signal mask, we need to restore the original one.
	 * In case we've got a signal while waiting, we do not restore the
	 * signal mask yet, and we allow do_signal() to deliver the signal on
	 * the way back to userspace, before the signal mask is restored.
	 */
	/* 
	 * 如果我们改变了信号掩码，我们需要恢复原来的掩码。
	 * 如果在等待时收到信号，我们尚未恢复信号掩码，
	 * 我们允许 do_signal() 在返回用户空间之前传递信号，然后再恢复信号掩码。
	 */
	if (sigmask) {
		if (error == -EINTR) {  // 如果因为信号中断而返回
			memcpy(&current->saved_sigmask, &sigsaved, sizeof(sigsaved));
			// 保存原始的信号掩码，以便稍后恢复
			set_restore_sigmask();  // 标记为恢复信号掩码
		} else
			sigprocmask(SIG_SETMASK, &sigsaved, NULL);  // 恢复原始信号掩码
	}

	return error;  // 返回操作结果
}

#endif /* HAVE_SET_RESTORE_SIGMASK */

static int __init eventpoll_init(void)
{
	struct sysinfo si;	// 定义系统信息结构体，用于获取系统内存信息

	si_meminfo(&si);	// 获取当前系统的内存信息
	/*
	 * Allows top 4% of lomem to be allocated for epoll watches (per user).
	 */
	/* 允许分配最多 4% 的低内存用于每个用户的 epoll 监控对象。 */
	max_user_watches = (((si.totalram - si.totalhigh) / 25) << PAGE_SHIFT) /
		EP_ITEM_COST;	// 计算每个用户可以创建的最大 epoll 监控对象数量

	/* Initialize the structure used to perform safe poll wait head wake ups */
	/* 初始化用于执行安全的 poll 等待头部唤醒的结构 */
	ep_nested_calls_init(&poll_safewake_ncalls);

	/* Initialize the structure used to perform file's f_op->poll() calls */
	/* 初始化用于执行文件的 f_op->poll() 调用的结构 */
	ep_nested_calls_init(&poll_readywalk_ncalls);

	/* Allocates slab cache used to allocate "struct epitem" items */
	/* 分配用于分配 "struct epitem" 项的 slab 缓存 */
	epi_cache = kmem_cache_create("eventpoll_epi", sizeof(struct epitem),
			0, SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);

	/* Allocates slab cache used to allocate "struct eppoll_entry" */
	/* 分配用于分配 "struct eppoll_entry" 的 slab 缓存 */
	pwq_cache = kmem_cache_create("eventpoll_pwq",
			sizeof(struct eppoll_entry), 0, SLAB_PANIC, NULL);

	return 0;	// 初始化成功返回 0
}
fs_initcall(eventpoll_init);
