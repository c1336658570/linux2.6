#ifndef _LINUX_KTHREAD_H
#define _LINUX_KTHREAD_H
/* Simple interface for creating and stopping kernel threads without mess. */
#include <linux/err.h>
#include <linux/sched.h>

// 创建内核线程，如果其他线程不调用wake_up_process，新创建的线程永远不会运行，namefmt是新内核进程的名字
struct task_struct *kthread_create(int (*threadfn)(void *data),
				   void *data,
				   const char namefmt[], ...)
	__attribute__((format(printf, 3, 4)));

/**
 * kthread_run - create and wake a thread.
 * @threadfn: the function to run until signal_pending(current).
 * @data: data ptr for @threadfn.
 * @namefmt: printf-style name for the thread.
 *
 * Description: Convenient wrapper for kthread_create() followed by
 * wake_up_process().  Returns the kthread or ERR_PTR(-ENOMEM).
 */
//  创建一个内核进程并让他运行起来，直接调用kthread_create的话新创建的进程处于不可运行状态，必须再调用wake_up_process()明确去唤醒它
#define kthread_run(threadfn, data, namefmt, ...)			   \
({									   \
	struct task_struct *__k						   \
		= kthread_create(threadfn, data, namefmt, ## __VA_ARGS__); \
	if (!IS_ERR(__k))						   \
		wake_up_process(__k);					   \
	__k;								   \
})

void kthread_bind(struct task_struct *k, unsigned int cpu);
int kthread_stop(struct task_struct *k);		// 内核的其他部分调用，用来停止运行k这个内核线程
int kthread_should_stop(void);

// 内核时通过从kthreadadd内核进程中衍生出所有新的内核线程
int kthreadd(void *unused);
extern struct task_struct *kthreadd_task;		// 指向kthreadd内核线程

#endif /* _LINUX_KTHREAD_H */
