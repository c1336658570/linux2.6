#ifndef _LINUX_WAIT_H
#define _LINUX_WAIT_H

// waitpid和相关函数使用的选项
#define WNOHANG		0x00000001	/* 非阻塞wait；如果没有已终止的子进程则立即返回 */
// WUNTRACED 和 WSTOPPED 允许父进程接收子进程由于信号而停止的通知。
// 如果子进程进入暂停状态，则马上返回。
#define WUNTRACED	0x00000002	/* 报告已停止（但尚未报告过的）子进程的状态 */
#define WSTOPPED	WUNTRACED	/* WUNTRACED的别名 */
#define WEXITED		0x00000004	/* 报告已退出的子进程状态 */
#define WCONTINUED	0x00000008	/* 报告已经继续执行的由SIGCONT信号停止的子进程状态 */
/* 不清理子进程状态，仅轮询状态 */
#define WNOWAIT		0x01000000	/* Don't reap, just poll status.  */

// 专为特定线程或类型的子进程等待状态的标志
/* 不等待此线程组中其他线程的子进程 */
#define __WNOTHREAD	0x20000000	/* Don't wait on children of other threads in this group */
/* 等待所有类型的子进程，不论其状态 */
#define __WALL		0x40000000	/* Wait on all children, regardless of type */
/* 仅等待非SIGCHLD类型的子进程 */
#define __WCLONE	0x80000000	/* Wait only on non-SIGCHLD children */

/* First argument to waitid: */
/* waitid函数的第一个参数：指定等待子进程的类型 */
#define P_ALL		0		/* 等待任何子进程 */
#define P_PID		1		/* 等待指定的PID */
#define P_PGID		2		/* 等待指定的进程组ID */

#ifdef __KERNEL__

#include <linux/list.h>
#include <linux/stddef.h>
#include <linux/spinlock.h>
#include <asm/system.h>
#include <asm/current.h>

/* 类型定义，便于引用 __wait_queue 结构体 */
typedef struct __wait_queue wait_queue_t;
/* 等待队列函数指针类型，用于定义可以调用的回调函数 */
typedef int (*wait_queue_func_t)(wait_queue_t *wait, unsigned mode, int flags, void *key);
/* 默认的唤醒函数，用于唤醒等待队列中的一个或多个进程 */
int default_wake_function(wait_queue_t *wait, unsigned mode, int flags, void *key);

/* 定义一个等待队列的结构体 */
struct __wait_queue {
	unsigned int flags;         /* 等待队列标志，例如是否为独占 */
// 独占意味着当此进程被唤醒时，不会唤醒其他等待此条件的进程。
#define WQ_FLAG_EXCLUSIVE	0x01  /* 如果设置，表示等待队列为独占模式 */
	void *private;              /* 私有数据指针，通常用于存储指向任务结构的指针 */
	wait_queue_func_t func;     /* 等待队列处理函数 */
	struct list_head task_list; /* 任务链表头，用于链接等待队列中的各个元素 */
};

/* 等待位键结构体，用于wait_bit相关操作 */
struct wait_bit_key {
	void *flags;    /* 指向用于等待的标志位的指针 */
	int bit_nr;     /* 需要测试或设置的位的编号 */
};

/* 等待位队列结构体，包括一个等待位键和一个等待队列项 */
struct wait_bit_queue {
	struct wait_bit_key key;   /* 等待位键，定义了等待的位标志和位编号 */
	wait_queue_t wait;         /* 等待队列元素，包括等待条件和链接到等待队列的链表头 */
};

/* 等待队列头结构体，用于管理一组等待队列元素 */
struct __wait_queue_head {
	spinlock_t lock;           /* 自旋锁，用于保护对等待队列的访问 */
	struct list_head task_list;/* 任务列表头，链接所有等待队列元素 */
};
typedef struct __wait_queue_head wait_queue_head_t; /* 别名定义，简化引用 */

/* 前向声明，定义一个任务结构，具体结构体定义在其他头文件中 */
struct task_struct;

/*
 * Macros for declaration and initialisaton of the datatypes
 */
/*
 * 用于声明和初始化数据类型的宏定义
 */

/* 初始化等待队列元素 */
#define __WAITQUEUE_INITIALIZER(name, tsk) {				\
	.private	= tsk,						/* 私有数据指针，通常指向一个任务结构体 */	\
	.func		= default_wake_function,			/* 默认的唤醒函数 */	\
	.task_list	= { NULL, NULL } /* 初始化任务列表节点 */ }

// 创建一个等待队列，休眠的进程可以在等待队列上，可以将cfs中的进程移动到等待队列，也可以将等待队列进程移动到cfs
/* 声明并初始化一个等待队列元素 */
#define DECLARE_WAITQUEUE(name, tsk)					\
	wait_queue_t name = __WAITQUEUE_INITIALIZER(name, tsk)	/* 使用初始化宏定义一个新的等待队列元素 */

/* 初始化等待队列头 */
#define __WAIT_QUEUE_HEAD_INITIALIZER(name) {				\
	.lock		= __SPIN_LOCK_UNLOCKED(name.lock),		/* 初始化自旋锁 */	\
	.task_list	= { &(name).task_list, &(name).task_list } /* 初始化任务列表头 */ }

/* 声明并初始化一个等待队列头 */
#define DECLARE_WAIT_QUEUE_HEAD(name) \
	wait_queue_head_t name = __WAIT_QUEUE_HEAD_INITIALIZER(name)	/* 使用初始化宏定义一个新的等待队列头 */

/* 初始化等待位键 */
#define __WAIT_BIT_KEY_INITIALIZER(word, bit)				\
	{ .flags = word, .bit_nr = bit, } /* 设定等待位键的位地址和位编号 */

/* 外部函数声明，用于初始化等待队列头 */
extern void __init_waitqueue_head(wait_queue_head_t *q, struct lock_class_key *);

// 创建一个等待队列
/* 初始化等待队列头的宏定义 */
#define init_waitqueue_head(q)				\
	do {						\
		static struct lock_class_key __key;	/* 定义一个静态的锁类键，用于锁依赖检查 */ \
							\
		__init_waitqueue_head((q), &__key);	/* 调用初始化函数，传入等待队列头和锁类键地址 */ \
	} while (0) /* 使用do-while循环确保宏的安全使用，但实际只执行一次 */

/* 如果配置了CONFIG_LOCKDEP（锁依赖检查），定义特定的初始化宏 */
#ifdef CONFIG_LOCKDEP
# define __WAIT_QUEUE_HEAD_INIT_ONSTACK(name) \
	({ init_waitqueue_head(&name); name; })	/* 在栈上初始化等待队列头，并返回其值 */
# define DECLARE_WAIT_QUEUE_HEAD_ONSTACK(name) \
	wait_queue_head_t name = __WAIT_QUEUE_HEAD_INIT_ONSTACK(name)	/* 使用上述宏在栈上声明并初始化一个等待队列头 */
#else
/* 如果没有启用CONFIG_LOCKDEP，使用标准的声明宏 */
# define DECLARE_WAIT_QUEUE_HEAD_ONSTACK(name) DECLARE_WAIT_QUEUE_HEAD(name)	/* 直接使用通用的等待队列头声明宏 */
#endif

/* 初始化等待队列元素 */
static inline void init_waitqueue_entry(wait_queue_t *q, struct task_struct *p)
{
	q->flags = 0;                /* 将标志位清零 */
	q->private = p;              /* 将private指针设置为传入的任务结构指针 */
	q->func = default_wake_function; /* 设置唤醒函数为默认唤醒函数 */
}

/* 初始化具有特定唤醒函数的等待队列元素 */
static inline void init_waitqueue_func_entry(wait_queue_t *q,
					wait_queue_func_t func)
{
	q->flags = 0;                /* 将标志位清零 */
	q->private = NULL;           /* 将private指针设置为NULL */
	q->func = func;              /* 设置唤醒函数为传入的函数 */
}

/* 检查等待队列是否有活跃的元素 */
static inline int waitqueue_active(wait_queue_head_t *q)
{
	return !list_empty(&q->task_list); /* 如果任务列表不为空，则返回非零（表示有活跃元素） */
}

// 向等待队列中添加元素
/* 外部声明，用于添加等待队列元素到等待队列 */
extern void add_wait_queue(wait_queue_head_t *q, wait_queue_t *wait);
/* 外部声明，用于以独占方式添加等待队列元素到等待队列 */
extern void add_wait_queue_exclusive(wait_queue_head_t *q, wait_queue_t *wait);
/* 外部声明，用于从等待队列移除等待队列元素 */
extern void remove_wait_queue(wait_queue_head_t *q, wait_queue_t *wait);

/* 将等待队列元素添加到等待队列的头部 */
static inline void __add_wait_queue(wait_queue_head_t *head, wait_queue_t *new)
{
	list_add(&new->task_list, &head->task_list); /* 在链表的头部插入新的等待队列元素 */
}

/*
 * Used for wake-one threads:
 */
/*
 * 用于唤醒单一线程：
 */
/* 将等待队列元素添加到等待队列的尾部 */
static inline void __add_wait_queue_tail(wait_queue_head_t *head,
						wait_queue_t *new)
{
	list_add_tail(&new->task_list, &head->task_list); /* 在链表的尾部插入新的等待队列元素 */
}

/* 从等待队列中移除等待队列元素 */
static inline void __remove_wait_queue(wait_queue_head_t *head,
							wait_queue_t *old)
{
	list_del(&old->task_list); /* 从链表中删除等待队列元素 */
}

/* 唤醒等待队列中的线程 */
void __wake_up(wait_queue_head_t *q, unsigned int mode, int nr, void *key);
/* 在锁定状态下根据key唤醒等待队列中的线程 */
void __wake_up_locked_key(wait_queue_head_t *q, unsigned int mode, void *key);
/* 同步地根据key唤醒等待队列中的线程 */
void __wake_up_sync_key(wait_queue_head_t *q, unsigned int mode, int nr, void *key);
/* 在锁定状态下唤醒等待队列中的线程 */
void __wake_up_locked(wait_queue_head_t *q, unsigned int mode);
/* 同步地唤醒等待队列中的线程 */
void __wake_up_sync(wait_queue_head_t *q, unsigned int mode, int nr);
/* 唤醒等待特定位变化的线程 */
void __wake_up_bit(wait_queue_head_t *, void *, int);
/* 在特定位上等待，直到条件满足或通过action函数中断 */
int __wait_on_bit(wait_queue_head_t *, struct wait_bit_queue *, int (*)(void *), unsigned);
/* 在特定位上加锁等待，直到条件满足或通过action函数中断 */
int __wait_on_bit_lock(wait_queue_head_t *, struct wait_bit_queue *, int (*)(void *), unsigned);
/* 唤醒在特定位上等待的线程 */
void wake_up_bit(void *, int);
/* 在特定位上非锁定状态下等待 */
int out_of_line_wait_on_bit(void *, int, int (*)(void *), unsigned);
/* 在特定位上加锁等待 */
int out_of_line_wait_on_bit_lock(void *, int, int (*)(void *), unsigned);
/* 获取与特定内存位关联的等待队列 */
wait_queue_head_t *bit_waitqueue(void *, int);

// 唤醒
/* 定义用于唤醒操作的宏 */
// TASK_NORMAL 和 TASK_INTERRUPTIBLE 分别指定了普通模式和可中断模式的唤醒。普通模式下的线程唤醒不考虑线程的中断状态，而可中断模式下唤醒的线程可以在收到信号时被中断。
/* 唤醒等待队列中的一个线程，使用普通的唤醒模式 */
#define wake_up(x)			__wake_up(x, TASK_NORMAL, 1, NULL)
/* 唤醒等待队列中指定数量的线程，使用普通的唤醒模式 */
#define wake_up_nr(x, nr)		__wake_up(x, TASK_NORMAL, nr, NULL)
/* 唤醒等待队列中的所有线程，使用普通的唤醒模式 */
#define wake_up_all(x)			__wake_up(x, TASK_NORMAL, 0, NULL)
/* 在锁定状态下唤醒等待队列中的线程，使用普通的唤醒模式 */
#define wake_up_locked(x)		__wake_up_locked((x), TASK_NORMAL)

/* 唤醒等待队列中的一个线程，使用可中断的唤醒模式 */
#define wake_up_interruptible(x)	__wake_up(x, TASK_INTERRUPTIBLE, 1, NULL)
/* 唤醒等待队列中指定数量的线程，使用可中断的唤醒模式 */
#define wake_up_interruptible_nr(x, nr)	__wake_up(x, TASK_INTERRUPTIBLE, nr, NULL)
/* 唤醒等待队列中的所有线程，使用可中断的唤醒模式 */
#define wake_up_interruptible_all(x)	__wake_up(x, TASK_INTERRUPTIBLE, 0, NULL)
/* 同步唤醒等待队列中的一个线程，使用可中断的唤醒模式 */
#define wake_up_interruptible_sync(x)	__wake_up_sync((x), TASK_INTERRUPTIBLE, 1)

/*
 * Wakeup macros to be used to report events to the targets.
 */
/*
 * 用于报告事件给目标线程的唤醒宏。
 */
/* 唤醒等待队列中的一个线程，使用普通唤醒模式，并传递特定的唤醒标志 */
#define wake_up_poll(x, m)				\
	__wake_up(x, TASK_NORMAL, 1, (void *) (m))
/* 在锁定状态下唤醒等待队列中的一个线程，使用普通唤醒模式，并传递特定的唤醒标志 */
#define wake_up_locked_poll(x, m)				\
	__wake_up_locked_key((x), TASK_NORMAL, (void *) (m))
/* 唤醒等待队列中的一个线程，使用可中断唤醒模式，并传递特定的唤醒标志 */
#define wake_up_interruptible_poll(x, m)			\
	__wake_up(x, TASK_INTERRUPTIBLE, 1, (void *) (m))
/* 同步地唤醒等待队列中的一个线程，使用可中断唤醒模式，并传递特定的唤醒标志 */
#define wake_up_interruptible_sync_poll(x, m)				\
	__wake_up_sync_key((x), TASK_INTERRUPTIBLE, 1, (void *) (m))

/*
 * 宏定义，用于等待特定条件成立
 */
#define __wait_event(wq, condition) 					\
do {									\
	DEFINE_WAIT(__wait);						/* 定义并初始化等待队列元素 */	\
									\
	for (;;) {							/* 循环直到条件成立 */	\
		prepare_to_wait(&wq, &__wait, TASK_UNINTERRUPTIBLE);	/* 准备等待，将线程置为不可中断状态 */	\
		if (condition)						/* 检查条件 */	\
			break;						/* 如果条件成立，跳出循环 */	\
		schedule();						/* 调度其他线程运行 */	\
	}								\
	finish_wait(&wq, &__wait);					/* 完成等待，清理等待队列元素 */	\
} while (0)

/**
 * wait_event - sleep until a condition gets true
 * @wq: the waitqueue to wait on
 * @condition: a C expression for the event to wait for
 *
 * The process is put to sleep (TASK_UNINTERRUPTIBLE) until the
 * @condition evaluates to true. The @condition is checked each time
 * the waitqueue @wq is woken up.
 *
 * wake_up() has to be called after changing any variable that could
 * change the result of the wait condition.
 */
/**
 * wait_event - 等待直到一个条件成为真
 * @wq: 要等待的等待队列
 * @condition: 等待触发的事件的C表达式
 *
 * 进程被置于睡眠状态（TASK_UNINTERRUPTIBLE），直到 @condition 表达式的结果为真。
 * 每次唤醒等待队列 @wq 时都会检查 @condition。
 *
 * 在改变可能影响等待条件判断结果的任何变量后，必须调用 wake_up()。
 */
#define wait_event(wq, condition) 					\
do {									\
	if (condition)	 						/* 如果条件已经成立，则跳出循环 */	\
		break;							\
	__wait_event(wq, condition);					/* 否则调用 __wait_event 宏继续等待 */	\
} while (0)

/**
 * __wait_event_timeout - 在给定的时间内等待一个条件变为真
 * @wq: 要等待的等待队列
 * @condition: 等待触发的事件的C表达式
 * @ret: 超时时间，通常以jiffies计算
 *
 * 这个过程将进程置于不可中断的睡眠状态，直到@condition变为真或超时。
 * 每次唤醒等待队列@wq时都会检查@condition。
 * 如果在超时时间结束之前@condition成立，进程将被唤醒。
 * 如果超时时间耗尽，循环将终止并返回剩余的时间（通常为0）。
 */
#define __wait_event_timeout(wq, condition, ret)			\
do {									\
	DEFINE_WAIT(__wait);						/* 定义并初始化等待队列元素 */	\
									\
	for (;;) {							/* 无限循环直到条件成立或超时 */	\
		prepare_to_wait(&wq, &__wait, TASK_UNINTERRUPTIBLE);	/* 将线程置为不可中断状态并准备等待 */	\
		if (condition)						/* 检查条件是否已经成立 */	\
			break;						/* 如果成立，跳出循环 */	\
		ret = schedule_timeout(ret);				/* 调度其他任务并设置超时，返回剩余时间 */	\
		if (!ret)						/* 如果超时时间耗尽 */	\
			break;						/* 跳出循环 */		\
	}								\
	finish_wait(&wq, &__wait);					/* 清理等待队列，恢复线程状态 */	\
} while (0)

/**
 * wait_event_timeout - sleep until a condition gets true or a timeout elapses
 * @wq: the waitqueue to wait on
 * @condition: a C expression for the event to wait for
 * @timeout: timeout, in jiffies
 *
 * The process is put to sleep (TASK_UNINTERRUPTIBLE) until the
 * @condition evaluates to true. The @condition is checked each time
 * the waitqueue @wq is woken up.
 *
 * wake_up() has to be called after changing any variable that could
 * change the result of the wait condition.
 *
 * The function returns 0 if the @timeout elapsed, and the remaining
 * jiffies if the condition evaluated to true before the timeout elapsed.
 */
/**
 * wait_event_timeout - 睡眠直到一个条件成真或超时发生
 * @wq: 要等待的等待队列
 * @condition: 等待触发的事件的C表达式
 * @timeout: 超时时间，以jiffies为单位
 *
 * 进程被置于睡眠状态（TASK_UNINTERRUPTIBLE），直到@condition评估为真。
 * 每次唤醒等待队列@wq时都会检查@condition。
 *
 * 在改变可能影响等待条件判断结果的任何变量后，必须调用wake_up()。
 *
 * 如果超时时间耗尽，则函数返回0；如果在超时前条件评估为真，则返回剩余的jiffies。
 */
#define wait_event_timeout(wq, condition, timeout)			\
({									\
	long __ret = timeout;						/* 初始化超时时间为传入的jiffies值 */	\
	if (!(condition)) 						/* 如果条件未立即成立 */	\
		__wait_event_timeout(wq, condition, __ret);		/* 执行等待事件的函数 */	\
	__ret;								/* 返回剩余的jiffies或0 */	\
})

/**
 * __wait_event_interruptible - 可中断地等待直到一个条件变为真
 * @wq: 要等待的等待队列
 * @condition: 等待触发的事件的C表达式
 * @ret: 用于存储返回状态
 *
 * 这个过程将进程置于可中断的睡眠状态（TASK_INTERRUPTIBLE），直到@condition变为真。
 * 如果在等待期间进程接收到一个信号，等待将被中断，返回-ERESTARTSYS。
 * 每次唤醒等待队列@wq时都会检查@condition。
 */
#define __wait_event_interruptible(wq, condition, ret)			\
do {									\
	DEFINE_WAIT(__wait);						/* 定义并初始化等待队列元素 */	\
									\
	for (;;) {							/* 无限循环直到条件成立或被信号中断 */	\
		prepare_to_wait(&wq, &__wait, TASK_INTERRUPTIBLE);	/* 将线程置为可中断状态并准备等待 */	\
		if (condition)						/* 检查条件是否已经成立 */	\
			break;						/* 如果成立，跳出循环 */	\
		if (!signal_pending(current)) {				/* 检查当前进程是否有待处理的信号 */	\
			schedule();					/* 调度其他任务并等待 */	\
			continue;					/* 继续循环检查条件 */	\
		}							\
		ret = -ERESTARTSYS;					/* 如果有信号待处理，设置返回值为-ERESTARTSYS */	\
		break;							/* 并跳出循环 */	\
	}								\
	finish_wait(&wq, &__wait);					/* 清理等待队列，恢复线程状态 */	\
} while (0)

/**
 * wait_event_interruptible - sleep until a condition gets true
 * @wq: the waitqueue to wait on
 * @condition: a C expression for the event to wait for
 *
 * The process is put to sleep (TASK_INTERRUPTIBLE) until the
 * @condition evaluates to true or a signal is received.
 * The @condition is checked each time the waitqueue @wq is woken up.
 *
 * wake_up() has to be called after changing any variable that could
 * change the result of the wait condition.
 *
 * The function will return -ERESTARTSYS if it was interrupted by a
 * signal and 0 if @condition evaluated to true.
 */
/**
 * wait_event_interruptible - 等待直到一个条件变为真
 * @wq: 要等待的等待队列
 * @condition: 等待触发的事件的C表达式
 *
 * 进程将被置于可中断的睡眠状态（TASK_INTERRUPTIBLE），直到@condition评估为真或接收到一个信号。
 * 每次唤醒等待队列@wq时都会检查@condition。
 *
 * 在改变可能影响等待条件判断结果的任何变量后，必须调用wake_up()。
 *
 * 如果等待因为信号被中断，函数将返回-ERESTARTSYS；如果@condition评估为真，则返回0。
 */
#define wait_event_interruptible(wq, condition)				\
({									\
	int __ret = 0;							/* 定义一个变量__ret用于存储返回值，默认为0 */	\
	if (!(condition))						/* 如果条件不成立 */	\
		__wait_event_interruptible(wq, condition, __ret);	/* 调用__wait_event_interruptible宏进行等待 */	\
	__ret;								/* 返回__ret的值，表明函数的执行结果 */	\
})

/**
 * __wait_event_interruptible_timeout - 可中断地等待直到一个条件变为真或超时发生
 * @wq: 要等待的等待队列
 * @condition: 等待触发的事件的C表达式
 * @ret: 超时值，以jiffies为单位
 *
 * 这个过程将进程置于可中断的睡眠状态（TASK_INTERRUPTIBLE），直到@condition变为真或超时。
 * 每次唤醒等待队列@wq时都会检查@condition。
 * 如果在等待期间进程接收到一个信号，等待将被中断，返回-ERESTARTSYS。
 * 如果超时时间耗尽，循环将终止并返回剩余的时间（通常为0）。
 */
#define __wait_event_interruptible_timeout(wq, condition, ret)		\
do {									\
	DEFINE_WAIT(__wait);						/* 定义并初始化等待队列元素 */	\
									\
	for (;;) {							/* 无限循环直到条件成立或超时 */	\
		prepare_to_wait(&wq, &__wait, TASK_INTERRUPTIBLE);	/* 将线程置为可中断状态并准备等待 */	\
		if (condition)						/* 检查条件是否已经成立 */	\
			break;						/* 如果成立，跳出循环 */	\
		if (!signal_pending(current)) {				/* 检查当前进程是否有待处理的信号 */	\
			ret = schedule_timeout(ret);			/* 调度其他任务并设置超时，返回剩余时间 */	\
			if (!ret)					/* 如果超时时间耗尽 */	\
				break;					/* 跳出循环 */		\
			continue;					/* 继续循环检查条件 */	\
		}							\
		ret = -ERESTARTSYS;					/* 如果有信号待处理，设置返回值为-ERESTARTSYS */	\
		break;							/* 并跳出循环 */	\
	}								\
	finish_wait(&wq, &__wait);					/* 清理等待队列，恢复线程状态 */	\
} while (0)

/**
 * wait_event_interruptible_timeout - sleep until a condition gets true or a timeout elapses
 * @wq: the waitqueue to wait on
 * @condition: a C expression for the event to wait for
 * @timeout: timeout, in jiffies
 *
 * The process is put to sleep (TASK_INTERRUPTIBLE) until the
 * @condition evaluates to true or a signal is received.
 * The @condition is checked each time the waitqueue @wq is woken up.
 *
 * wake_up() has to be called after changing any variable that could
 * change the result of the wait condition.
 *
 * The function returns 0 if the @timeout elapsed, -ERESTARTSYS if it
 * was interrupted by a signal, and the remaining jiffies otherwise
 * if the condition evaluated to true before the timeout elapsed.
 */
/**
 * wait_event_interruptible_timeout - 等待直到一个条件变为真或超时发生
 * @wq: 要等待的等待队列
 * @condition: 等待触发的事件的C表达式
 * @timeout: 超时时间，以jiffies为单位
 *
 * 进程被置于可中断的睡眠状态（TASK_INTERRUPTIBLE），直到@condition评估为真或接收到一个信号。
 * 每次唤醒等待队列@wq时都会检查@condition。
 *
 * 在改变可能影响等待条件判断结果的任何变量后，必须调用wake_up()。
 *
 * 如果超时时间耗尽，则函数返回0；如果等待被信号中断，则返回-ERESTARTSYS；
 * 如果在超时前条件评估为真，则返回剩余的jiffies。
 */
#define wait_event_interruptible_timeout(wq, condition, timeout)	\
({									\
	long __ret = timeout;						/* 初始化超时时间为传入的jiffies值 */	\
	if (!(condition))						/* 如果条件未立即成立 */	\
		__wait_event_interruptible_timeout(wq, condition, __ret); /* 调用__wait_event_interruptible_timeout宏进行等待 */	\
	__ret;								/* 返回__ret的值，表明函数的执行结果 */	\
})

/**
 * wait_event_interruptible_timeout - 等待直到一个条件变为真或超时发生
 * @wq: 要等待的等待队列
 * @condition: 等待触发的事件的C表达式
 * @timeout: 超时时间，以jiffies为单位
 *
 * 进程被置于可中断的睡眠状态（TASK_INTERRUPTIBLE），直到@condition评估为真或接收到一个信号。
 * 每次唤醒等待队列@wq时都会检查@condition。
 *
 * 在改变可能影响等待条件判断结果的任何变量后，必须调用wake_up()。
 *
 * 如果超时时间耗尽，则函数返回0；如果等待被信号中断，则返回-ERESTARTSYS；
 * 如果在超时前条件评估为真，则返回剩余的jiffies。
 */
#define wait_event_interruptible_timeout(wq, condition, timeout)	\
({									\
	long __ret = timeout;						/* 初始化超时时间为传入的jiffies值 */	\
	if (!(condition))						/* 如果条件未立即成立 */	\
		__wait_event_interruptible_timeout(wq, condition, __ret); /* 调用__wait_event_interruptible_timeout宏进行等待 */	\
	__ret;								/* 返回__ret的值，表明函数的执行结果 */	\
})

/**
 * __wait_event_interruptible_exclusive - 以独占且可中断的方式等待直到一个条件变为真
 * @wq: 要等待的等待队列
 * @condition: 等待触发的事件的C表达式
 * @ret: 用于存储返回状态
 *
 * 这个过程将进程置于可中断且独占的睡眠状态（TASK_INTERRUPTIBLE），直到@condition变为真。
 * 如果在等待期间进程接收到一个信号，等待将被中断，并返回-ERESTARTSYS。
 * 独占等待意味着当此进程被唤醒时，不会唤醒其他等待此条件的进程。
 */
#define __wait_event_interruptible_exclusive(wq, condition, ret)	\
do {									\
	DEFINE_WAIT(__wait);						/* 定义并初始化等待队列元素 */	\
									\
	for (;;) {							/* 无限循环直到条件成立或被信号中断 */	\
		prepare_to_wait_exclusive(&wq, &__wait,			\
					TASK_INTERRUPTIBLE);		/* 将线程置为可中断且独占的等待状态 */	\
		if (condition) {					/* 检查条件是否已经成立 */	\
			finish_wait(&wq, &__wait);			/* 清理等待队列，恢复线程状态 */	\
			break;						/* 如果成立，跳出循环 */	\
		}							\
		if (!signal_pending(current)) {				/* 检查当前进程是否有待处理的信号 */	\
			schedule();					/* 调度其他任务并等待 */	\
			continue;					/* 继续循环检查条件 */	\
		}							\
		ret = -ERESTARTSYS;					/* 如果有信号待处理，设置返回值为-ERESTARTSYS */	\
		abort_exclusive_wait(&wq, &__wait, 			\
				TASK_INTERRUPTIBLE, NULL);		/* 中止等待并清理状态 */	\
		break;							/* 并跳出循环 */	\
	}								\
} while (0)

/**
 * wait_event_interruptible_exclusive - 在一个条件变为真之前，以独占且可中断的方式等待
 * @wq: 要等待的等待队列
 * @condition: 等待触发的事件的C表达式
 *
 * 这个宏将进程置于可中断且独占的睡眠状态，直到@condition评估为真。
 * 如果在等待期间进程接收到一个信号，等待将被中断，并返回-ERESTARTSYS。
 * 如果@condition成立，返回0。
 */
#define wait_event_interruptible_exclusive(wq, condition)		\
({									\
	int __ret = 0;							\
	if (!(condition))						\
		__wait_event_interruptible_exclusive(wq, condition, __ret);/* 如果条件不成立，则进入独占等待 */	\
	__ret;								/* 返回结果 */		\
})

/**
 * __wait_event_killable - 在一个条件变为真之前，以可终止方式等待
 * @wq: 要等待的等待队列
 * @condition: 等待触发的事件的C表达式
 * @ret: 用于存储返回状态
 *
 * 这个过程将进程置于可终止的睡眠状态（TASK_KILLABLE），直到@condition变为真。
 * 如果在等待期间进程接收到一个致命信号，等待将被中断，并返回-ERESTARTSYS。
 * 如果@condition成立，则跳出循环并完成等待。
 */
/**
 * TASK_KILLABLE（可杀的深度睡眠）：可以被等到的资源唤醒，不能被常规信号唤醒，但是可以被致命信号唤醒。
 * TASK_KILLABLE状态的定义是：
 * #define TASK_KILLABLE           (TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)
 * 所以它显然是属于TASK_UNINTERRUPTIBLE的，只是可以被TASK_WAKEKILL。
 * 什么叫致命信号呢？talk is cheap，show me the code。
 * 看该函数__fatal_signal_pending
 * 所以，足够致命的信号就是SIGKILL。SIGKILL何许人也，就是传说中的信号9，无法阻挡无法被应用覆盖的终极杀器
 */
#define __wait_event_killable(wq, condition, ret)			\
do {									\
	DEFINE_WAIT(__wait);						\
									\
	for (;;) {							\
		prepare_to_wait(&wq, &__wait, TASK_KILLABLE);		/* 将线程置为可终止状态并准备等待 */	\
		if (condition)						/* 检查条件是否已经成立 */	\
			break;						/* 如果成立，跳出循环 */	\
		if (!fatal_signal_pending(current)) {			/* 检查当前进程是否有致命信号待处理 */	\
			schedule();					/* 调度其他任务并等待 */	\
			continue;					/* 继续循环检查条件 */	\
		}							\
		ret = -ERESTARTSYS;					/* 如果有致命信号待处理，设置返回值为-ERESTARTSYS */	\
		break;							/* 并跳出循环 */	\
	}								\
	finish_wait(&wq, &__wait);					/* 清理等待队列，恢复线程状态 */	\
} while (0)

/**
 * wait_event_killable - sleep until a condition gets true
 * @wq: the waitqueue to wait on
 * @condition: a C expression for the event to wait for
 *
 * The process is put to sleep (TASK_KILLABLE) until the
 * @condition evaluates to true or a signal is received.
 * The @condition is checked each time the waitqueue @wq is woken up.
 *
 * wake_up() has to be called after changing any variable that could
 * change the result of the wait condition.
 *
 * The function will return -ERESTARTSYS if it was interrupted by a
 * signal and 0 if @condition evaluated to true.
 */
/**
 * wait_event_killable - 等待直到一个条件变为真
 * @wq: 要等待的等待队列
 * @condition: 等待触发的事件的C表达式
 *
 * 进程将被置于可终止的睡眠状态（TASK_KILLABLE），直到@condition评估为真或接收到一个信号。
 * 每次唤醒等待队列@wq时都会检查@condition。
 *
 * 在改变可能影响等待条件判断结果的任何变量后，必须调用wake_up()。
 *
 * 如果等待因为信号被中断，函数将返回-ERESTARTSYS；如果@condition评估为真，则返回0。
 */
#define wait_event_killable(wq, condition)				\
({									\
	int __ret = 0;							/* 初始化返回值为0 */	\
	if (!(condition))						/* 如果条件不成立 */	\
		__wait_event_killable(wq, condition, __ret);		/* 调用__wait_event_killable进行等待 */	\
	__ret;								/* 返回__ret，表示等待结果 */	\
})

/*
 * Must be called with the spinlock in the wait_queue_head_t held.
 */
/*
 * 在持有等待队列头部的自旋锁时必须调用此函数。
 * add_wait_queue_exclusive_locked - 在等待队列中以独占方式添加一个等待队列元素，并且必须在已锁状态下调用
 * @q: 操作的等待队列头
 * @wait: 要添加的等待队列元素
 *
 * 此函数将等待队列元素设置为独占模式，并将其添加到等待队列的尾部。
 */
static inline void add_wait_queue_exclusive_locked(wait_queue_head_t *q,
							 wait_queue_t *wait)
{
	wait->flags |= WQ_FLAG_EXCLUSIVE;  // 将等待队列元素标记为独占
	__add_wait_queue_tail(q, wait);    // 将等待队列元素添加到等待队列的尾部
}

/*
 * Must be called with the spinlock in the wait_queue_head_t held.
 */
/*
 * 在持有等待队列头部的自旋锁时必须调用此函数。
 * remove_wait_queue_locked - 在等待队列中移除一个等待队列元素，并且必须在已锁状态下调用
 * @q: 操作的等待队列头
 * @wait: 要移除的等待队列元素
 *
 * 此函数从等待队列中移除指定的等待队列元素。
 */
static inline void remove_wait_queue_locked(wait_queue_head_t *q,
					    wait_queue_t *wait)
{
	__remove_wait_queue(q, wait);  // 从等待队列中移除等待队列元素
}

/*
 * These are the old interfaces to sleep waiting for an event.
 * They are racy.  DO NOT use them, use the wait_event* interfaces above.
 * We plan to remove these interfaces.
 */
/*
 * 这些是等待事件时进入睡眠的旧接口。
 * 它们存在竞态条件。请不要使用它们，请使用上面的 wait_event* 接口。
 * 我们计划移除这些接口。
 */
extern void sleep_on(wait_queue_head_t *q);  // 使调用进程在给定的等待队列上无限期睡眠，直到被明确唤醒
extern long sleep_on_timeout(wait_queue_head_t *q,
                             signed long timeout);  // 使调用进程在给定的等待队列上睡眠，直到超时或被唤醒
extern void interruptible_sleep_on(wait_queue_head_t *q);  // 使调用进程在给定的等待队列上可中断地睡眠，直到被唤醒或接收到信号
extern long interruptible_sleep_on_timeout(wait_queue_head_t *q,
                                           signed long timeout);  // 使调用进程在给定的等待队列上可中断地睡眠，直到超时或被唤醒或接收到信号

/*
 * Waitqueues which are removed from the waitqueue_head at wakeup time
 */
/*
 * 在唤醒时从等待队列头中移除的等待队列
 */
// 修改进程状态，可以修改为TASK_INTERRUPTIBLE或TASK_UNINTERRUPTIBLE
/*
 * prepare_to_wait - 准备一个进程进入等待状态
 * @q: 目标等待队列头
 * @wait: 等待队列元素，代表要等待的进程
 * @state: 进程等待时的状态 (TASK_INTERRUPTIBLE 或 TASK_UNINTERRUPTIBLE)
 *
 * 此函数将一个进程加入到指定的等待队列，并设置其任务状态，以便安全地进入调度前的等待。
 */
void prepare_to_wait(wait_queue_head_t *q, wait_queue_t *wait, int state);
/*
 * prepare_to_wait_exclusive - 以独占方式准备一个进程进入等待状态
 * @q: 目标等待队列头
 * @wait: 等待队列元素，代表要等待的进程
 * @state: 进程等待时的状态 (TASK_INTERRUPTIBLE 或 TASK_UNINTERRUPTIBLE)
 *
 * 此函数类似于 prepare_to_wait，但它确保当条件触发时，只有一个等待者被唤醒。
 */
void prepare_to_wait_exclusive(wait_queue_head_t *q, wait_queue_t *wait, int state);
/*
 * finish_wait - 完成等待，将进程从等待队列中移除并恢复其状态为TASK_RUNNING
 * @q: 等待队列头
 * @wait: 等待队列元素
 *
 * 当等待条件被满足后调用此函数，清理等待状态，准备返回到正常执行。
 */
void finish_wait(wait_queue_head_t *q, wait_queue_t *wait);
/*
 * abort_exclusive_wait - 中止一个独占的等待，通常在条件未满足时调用
 * @q: 等待队列头
 * @wait: 等待队列元素
 * @mode: 等待模式（TASK_INTERRUPTIBLE 或 TASK_UNINTERRUPTIBLE）
 * @key: 用于确定唤醒的关键信息，通常用于复杂的唤醒逻辑
 *
 * 如果等待过程中需要中止等待（例如，接收到一个信号），调用此函数来清理等待状态。
 */
void abort_exclusive_wait(wait_queue_head_t *q, wait_queue_t *wait,
			unsigned int mode, void *key);
/*
 * autoremove_wake_function - 当进程被唤醒时自动从等待队列中移除
 * @wait: 等待队列元素
 * @mode: 唤醒的模式
 * @sync: 是否同步唤醒
 * @key: 唤醒关键信息
 *
 * 这是一个回调函数，用于唤醒时自动处理等待队列的移除操作，减少管理负担。
 */
int autoremove_wake_function(wait_queue_t *wait, unsigned mode, int sync, void *key);
/*
 * wake_bit_function - 根据位掩码进行条件唤醒的函数
 * @wait: 等待队列元素
 * @mode: 唤醒的模式
 * @sync: 是否同步唤醒
 * @key: 用于唤醒条件的位掩码
 *
 * 当等待的条件涉及位操作时，此函数用于根据位的变化进行有选择性的唤醒。
 */

int wake_bit_function(wait_queue_t *wait, unsigned mode, int sync, void *key);

/*
 * 定义一个等待函数
 * 使用给定的函数初始化一个等待队列元素
 */
#define DEFINE_WAIT_FUNC(name, function)				\
	wait_queue_t name = {						\
		.private	= current,				/* 设置private为当前进程 */	\
		.func		= function,				/* 设置等待队列的唤醒函数 */	\
		.task_list	= LIST_HEAD_INIT((name).task_list),	/* 初始化任务列表 */	\
	}

// 定义一个等待队列，名字是name，func是autoremove_wake_function
/*
 * 定义一个等待队列元素，并将其唤醒函数设置为 autoremove_wake_function，
 * 这个函数在唤醒时会自动从等待队列中移除等待元素。
 */
#define DEFINE_WAIT(name) DEFINE_WAIT_FUNC(name, autoremove_wake_function)

/*
 * 定义一个等待位队列元素，用于位级等待。
 * @name: 等待队列变量名
 * @word: 位操作的目标地址
 * @bit: 目标位
 * 这个宏初始化等待位队列元素，设置了唤醒函数为 wake_bit_function，这个函数专门用于处理位级唤醒。
 */
#define DEFINE_WAIT_BIT(name, word, bit)				\
	struct wait_bit_queue name = {					\
		.key = __WAIT_BIT_KEY_INITIALIZER(word, bit),		\
		.wait	= {						\
			.private	= current,			\
			.func		= wake_bit_function,		\
			.task_list	=				\
				LIST_HEAD_INIT((name).wait.task_list),	\
		},							\
	}

/*
 * 初始化一个等待队列元素。
 * @wait: 指向等待队列元素的指针
 * 这个宏设置等待队列元素的 private 字段为当前进程，唤醒函数为 autoremove_wake_function，
 * 并初始化其任务列表。
 */
#define init_wait(wait)							\
	do {								\
		(wait)->private = current;				\
		(wait)->func = autoremove_wake_function;		\
		INIT_LIST_HEAD(&(wait)->task_list);			\
	} while (0)

/**
 * wait_on_bit - wait for a bit to be cleared
 * @word: the word being waited on, a kernel virtual address
 * @bit: the bit of the word being waited on
 * @action: the function used to sleep, which may take special actions
 * @mode: the task state to sleep in
 *
 * There is a standard hashed waitqueue table for generic use. This
 * is the part of the hashtable's accessor API that waits on a bit.
 * For instance, if one were to have waiters on a bitflag, one would
 * call wait_on_bit() in threads waiting for the bit to clear.
 * One uses wait_on_bit() where one is waiting for the bit to clear,
 * but has no intention of setting it.
 */
/**
 * wait_on_bit - 等待一个位被清除
 * @word: 被等待的字，一个内核虚拟地址
 * @bit: 被等待的字的具体位
 * @action: 用于睡眠的函数，可能执行特殊动作
 * @mode: 睡眠中的任务状态
 *
 * 有一个标准的哈希等待队列表用于一般用途。
 * 这是哈希表访问器API的一部分，用于等待一个位。
 * 例如，如果有人在一个位标志上有等待者，那么他们会在等待位清除时调用wait_on_bit()。
 * 当需要等待一个位被清除但没有意图去设置它时，使用wait_on_bit()。
 */
static inline int wait_on_bit(void *word, int bit,
				int (*action)(void *), unsigned mode)
{
	if (!test_bit(bit, word))  // 测试指定位是否已清除
		return 0;  // 如果位已经被清除，返回0
	return out_of_line_wait_on_bit(word, bit, action, mode);  // 否则调用out_of_line_wait_on_bit进行阻塞等待
}

/**
 * wait_on_bit_lock - wait for a bit to be cleared, when wanting to set it
 * @word: the word being waited on, a kernel virtual address
 * @bit: the bit of the word being waited on
 * @action: the function used to sleep, which may take special actions
 * @mode: the task state to sleep in
 *
 * There is a standard hashed waitqueue table for generic use. This
 * is the part of the hashtable's accessor API that waits on a bit
 * when one intends to set it, for instance, trying to lock bitflags.
 * For instance, if one were to have waiters trying to set bitflag
 * and waiting for it to clear before setting it, one would call
 * wait_on_bit() in threads waiting to be able to set the bit.
 * One uses wait_on_bit_lock() where one is waiting for the bit to
 * clear with the intention of setting it, and when done, clearing it.
 */
/**
 * wait_on_bit_lock - 在想要设置一个位时等待该位被清除
 * @word: 被等待的字，一个内核虚拟地址
 * @bit: 被等待的字的具体位
 * @action: 用于睡眠的函数，可能执行特殊动作
 * @mode: 睡眠中的任务状态
 *
 * 有一个标准的哈希等待队列表用于一般用途。
 * 这是哈希表访问器API的一部分，用于在有意设置一个位时等待这个位。
 * 例如，如果有等待者试图设置位标志，并在设置前等待其清除，那么他们会在等待能够设置位的线程中调用wait_on_bit_lock()。
 * 当有意在清除位后设置它，并在完成后清除它时，使用wait_on_bit_lock()。
 */
static inline int wait_on_bit_lock(void *word, int bit,
				int (*action)(void *), unsigned mode)
{
	if (!test_and_set_bit(bit, word))  // 测试并设置指定位，如果之前位已清除（即为0），则现在设置为1
		return 0;  // 如果位在调用前已经是0，表示无需等待，直接返回0
	return out_of_line_wait_on_bit_lock(word, bit, action, mode);  // 否则调用out_of_line_wait_on_bit_lock进行阻塞等待
}
	
#endif /* __KERNEL__ */

#endif
