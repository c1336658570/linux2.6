/*
 * workqueue.h --- work queue handling for Linux.
 */

#ifndef _LINUX_WORKQUEUE_H
#define _LINUX_WORKQUEUE_H

#include <linux/timer.h>
#include <linux/linkage.h>
#include <linux/bitops.h>
#include <linux/lockdep.h>
#include <asm/atomic.h>

struct workqueue_struct;

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

/*
 * The first word is the work queue pointer and the flags rolled into
 * one
 */
// 第一个字是工作队列指针和标志位的组合
#define work_data_bits(work) ((unsigned long *)(&(work)->data))

// 表示工作的数据结构
struct work_struct {
	atomic_long_t data;	// 工作结构的数据字段，包含工作队列指针和标志位
// （工作项等待执行）标志位为0，表示工作项正在等待执行。
#define WORK_STRUCT_PENDING 0		/* T if work item pending execution */
// WORK_STRUCT_STATIC（静态初始化器，用于调试对象）标志位为1，表示静态初始化器。
#define WORK_STRUCT_STATIC  1		/* static initializer (debugobjects) */
// WORK_STRUCT_FLAG_MASK是一个掩码，用于提取标志位。
#define WORK_STRUCT_FLAG_MASK (3UL)
// WORK_STRUCT_WQ_DATA_MASK是一个掩码，用于提取工作队列指针。
#define WORK_STRUCT_WQ_DATA_MASK (~WORK_STRUCT_FLAG_MASK)
	// 这些结构体被连接成链表，每个处理器上的每种类型的队列都对应这样一个链表。当一个工作者线程被唤醒，
	// 它会执行链表的所有工作。工作执行完毕，它就将相应的work_struct对象从链表中移除。当链表不再有对象时，它就会休眠
	struct list_head entry;
	work_func_t func;			// 该工作执行的函数
#ifdef CONFIG_LOCKDEP
	struct lockdep_map lockdep_map;	// 用于配置锁依赖映射。
#endif
};

#define WORK_DATA_INIT()	ATOMIC_LONG_INIT(0)
#define WORK_DATA_STATIC_INIT()	ATOMIC_LONG_INIT(2)

struct delayed_work {
	struct work_struct work;		// 延迟执行的工作项
	struct timer_list timer;		// 定时器
};

static inline struct delayed_work *to_delayed_work(struct work_struct *work)
{
	return container_of(work, struct delayed_work, work);
}

struct execute_work {
	struct work_struct work;
};

#ifdef CONFIG_LOCKDEP
/*
 * NB: because we have to copy the lockdep_map, setting _key
 * here is required, otherwise it could get initialised to the
 * copy of the lockdep_map!
 */
#define __WORK_INIT_LOCKDEP_MAP(n, k) \
	.lockdep_map = STATIC_LOCKDEP_MAP_INIT(n, k),
#else
#define __WORK_INIT_LOCKDEP_MAP(n, k)
#endif

#define __WORK_INITIALIZER(n, f) {				\
	.data = WORK_DATA_STATIC_INIT(),			\
	.entry	= { &(n).entry, &(n).entry },			\
	.func = (f),						\
	__WORK_INIT_LOCKDEP_MAP(#n, &(n))			\
	}

#define __DELAYED_WORK_INITIALIZER(n, f) {			\
	.work = __WORK_INITIALIZER((n).work, (f)),		\
	.timer = TIMER_INITIALIZER(NULL, 0, 0),			\
	}

// 静态地创建一个名为name,处理函数为func，参数为data的work_struct结构体
#define DECLARE_WORK(n, f)					\
	struct work_struct n = __WORK_INITIALIZER(n, f)

#define DECLARE_DELAYED_WORK(n, f)				\
	struct delayed_work n = __DELAYED_WORK_INITIALIZER(n, f)

/*
 * initialize a work item's function pointer
 */
#define PREPARE_WORK(_work, _func)				\
	do {							\
		(_work)->func = (_func);			\
	} while (0)

#define PREPARE_DELAYED_WORK(_work, _func)			\
	PREPARE_WORK(&(_work)->work, (_func))

#ifdef CONFIG_DEBUG_OBJECTS_WORK
extern void __init_work(struct work_struct *work, int onstack);
extern void destroy_work_on_stack(struct work_struct *work);
#else
static inline void __init_work(struct work_struct *work, int onstack) { }
static inline void destroy_work_on_stack(struct work_struct *work) { }
#endif

/*
 * initialize all of a work item in one go
 *
 * NOTE! No point in using "atomic_long_set()": using a direct
 * assignment of the work data initializer allows the compiler
 * to generate better code.
 */
#ifdef CONFIG_LOCKDEP
#define __INIT_WORK(_work, _func, _onstack)				\
	do {								\
		static struct lock_class_key __key;			\
									\
		__init_work((_work), _onstack);				\
		(_work)->data = (atomic_long_t) WORK_DATA_INIT();	\
		lockdep_init_map(&(_work)->lockdep_map, #_work, &__key, 0);\
		INIT_LIST_HEAD(&(_work)->entry);			\
		PREPARE_WORK((_work), (_func));				\
	} while (0)
#else
#define __INIT_WORK(_work, _func, _onstack)				\
	do {								\
		__init_work((_work), _onstack);				\
		(_work)->data = (atomic_long_t) WORK_DATA_INIT();	\
		INIT_LIST_HEAD(&(_work)->entry);			\
		PREPARE_WORK((_work), (_func));				\
	} while (0)
#endif

// 运行时创建一个工作，动态地创建一个名为name,处理函数为func，参数为data的work_struct结构体
#define INIT_WORK(_work, _func)					\
	do {							\
		__INIT_WORK((_work), (_func), 0);		\
	} while (0)

#define INIT_WORK_ON_STACK(_work, _func)			\
	do {							\
		__INIT_WORK((_work), (_func), 1);		\
	} while (0)

#define INIT_DELAYED_WORK(_work, _func)				\
	do {							\
		INIT_WORK(&(_work)->work, (_func));		\
		init_timer(&(_work)->timer);			\
	} while (0)

#define INIT_DELAYED_WORK_ON_STACK(_work, _func)		\
	do {							\
		INIT_WORK_ON_STACK(&(_work)->work, (_func));	\
		init_timer_on_stack(&(_work)->timer);		\
	} while (0)

#define INIT_DELAYED_WORK_DEFERRABLE(_work, _func)		\
	do {							\
		INIT_WORK(&(_work)->work, (_func));		\
		init_timer_deferrable(&(_work)->timer);		\
	} while (0)

/**
 * work_pending - Find out whether a work item is currently pending
 * @work: The work item in question
 */
#define work_pending(work) \
	test_bit(WORK_STRUCT_PENDING, work_data_bits(work))

/**
 * delayed_work_pending - Find out whether a delayable work item is currently
 * pending
 * @work: The work item in question
 */
#define delayed_work_pending(w) \
	work_pending(&(w)->work)

/**
 * work_clear_pending - for internal use only, mark a work item as not pending
 * @work: The work item in question
 */
#define work_clear_pending(work) \
	clear_bit(WORK_STRUCT_PENDING, work_data_bits(work))


extern struct workqueue_struct *
__create_workqueue_key(const char *name, int singlethread,
		       int freezeable, int rt, struct lock_class_key *key,
		       const char *lock_name);

#ifdef CONFIG_LOCKDEP
#define __create_workqueue(name, singlethread, freezeable, rt)	\
({								\
	static struct lock_class_key __key;			\
	const char *__lock_name;				\
								\
	if (__builtin_constant_p(name))				\
		__lock_name = (name);				\
	else							\
		__lock_name = #name;				\
								\
	__create_workqueue_key((name), (singlethread),		\
			       (freezeable), (rt), &__key,	\
			       __lock_name);			\
})
#else
#define __create_workqueue(name, singlethread, freezeable, rt)	\
	__create_workqueue_key((name), (singlethread), (freezeable), (rt), \
			       NULL, NULL)
#endif

// 创建一个新的任务队列和与之相关的工作者线程（系统每个CPU都有一个），name参数用于给该内核线程命名。
// events队列的创建是：struct workqueue_struct *keventd_wq = create_workqueue("events");在workqueue.c/init_workqueues函数中
#define create_workqueue(name) __create_workqueue((name), 0, 0, 0)
#define create_rt_workqueue(name) __create_workqueue((name), 0, 0, 1)
#define create_freezeable_workqueue(name) __create_workqueue((name), 1, 1, 0)
#define create_singlethread_workqueue(name) __create_workqueue((name), 1, 0, 0)

extern void destroy_workqueue(struct workqueue_struct *wq);

// 和schedule_work等函数类似，但是该函数是对指定工作线程进行调度，和schedule_work函数作用雷同，不过变成了指定工作线程(wq)
extern int queue_work(struct workqueue_struct *wq, struct work_struct *work);
extern int queue_work_on(int cpu, struct workqueue_struct *wq,
			struct work_struct *work);
// 对指定工作线程进行延时调度
extern int queue_delayed_work(struct workqueue_struct *wq,
			struct delayed_work *work, unsigned long delay);
extern int queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
			struct delayed_work *work, unsigned long delay);

// 刷新指定的工作队列，函数会一直等待，直到队列中所有对象都被执行以后才返回，只能在进程上下文使用，因为在等待所有待处理工作执行的时候，该函数会进入休眠状态。
extern void flush_workqueue(struct workqueue_struct *wq);
// 刷新events工作队列，函数会一直等待，直到队列中所有对象都被执行以后才返回，只能在进程上下文使用，因为在等待所有待处理工作执行的时候，该函数会进入休眠状态。
// 该函数并不取消任何延迟执行的工作，就是说任何通过schedule_delayed_work调度的工作，如果其延迟时间未结束，并不会因为调用flush_scheduled_work()而被刷新掉
extern void flush_scheduled_work(void);
extern void flush_delayed_work(struct delayed_work *work);

// 对工作线程进行调度(调度events线程)，工作线程直接会执行
extern int schedule_work(struct work_struct *work);
extern int schedule_work_on(int cpu, struct work_struct *work);
// 对工作线程进行调度（调度events线程），延迟delay后工作线程才开始执行
extern int schedule_delayed_work(struct delayed_work *work, unsigned long delay);
extern int schedule_delayed_work_on(int cpu, struct delayed_work *work,
					unsigned long delay);
extern int schedule_on_each_cpu(work_func_t func);
extern int current_is_keventd(void);
extern int keventd_up(void);

extern void init_workqueues(void);
int execute_in_process_context(work_func_t fn, struct execute_work *);

extern int flush_work(struct work_struct *work);

extern int cancel_work_sync(struct work_struct *work);

/*
 * Kill off a pending schedule_delayed_work().  Note that the work callback
 * function may still be running on return from cancel_delayed_work(), unless
 * it returns 1 and the work doesn't re-arm itself. Run flush_workqueue() or
 * cancel_work_sync() to wait on it.
 */
// 取消延迟执行的工作，取消的是events工作线程中的延时工作
static inline int cancel_delayed_work(struct delayed_work *work)
{
	int ret;

	ret = del_timer_sync(&work->timer);
	if (ret)
		work_clear_pending(&work->work);
	return ret;
}

/*
 * Like above, but uses del_timer() instead of del_timer_sync(). This means,
 * if it returns 0 the timer function may be running and the queueing is in
 * progress.
 */
static inline int __cancel_delayed_work(struct delayed_work *work)
{
	int ret;

	ret = del_timer(&work->timer);
	if (ret)
		work_clear_pending(&work->work);
	return ret;
}

extern int cancel_delayed_work_sync(struct delayed_work *work);

/* Obsolete. use cancel_delayed_work_sync() */
static inline
void cancel_rearming_delayed_workqueue(struct workqueue_struct *wq,
					struct delayed_work *work)
{
	cancel_delayed_work_sync(work);
}

/* Obsolete. use cancel_delayed_work_sync() */
static inline
void cancel_rearming_delayed_work(struct delayed_work *work)
{
	cancel_delayed_work_sync(work);
}

#ifndef CONFIG_SMP
static inline long work_on_cpu(unsigned int cpu, long (*fn)(void *), void *arg)
{
	return fn(arg);
}
#else
long work_on_cpu(unsigned int cpu, long (*fn)(void *), void *arg);
#endif /* CONFIG_SMP */
#endif
