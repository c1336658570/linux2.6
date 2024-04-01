// 信号量相关代码
/*
 * Copyright (c) 2008 Intel Corporation
 * Author: Matthew Wilcox <willy@linux.intel.com>
 *
 * Distributed under the terms of the GNU GPL, version 2
 *
 * Please see kernel/semaphore.c for documentation of these functions
 */
#ifndef __LINUX_SEMAPHORE_H
#define __LINUX_SEMAPHORE_H

#include <linux/list.h>
#include <linux/spinlock.h>

/* Please don't access any members of this structure directly */
// 信号量定义
struct semaphore {
	spinlock_t		lock;				// 自旋锁
	unsigned int		count;		// 个数
	struct list_head	wait_list;		// 睡眠队列
};

// 初始化，由DECLARE_MUTEX调用
#define __SEMAPHORE_INITIALIZER(name, n)				\
{									\
	.lock		= __SPIN_LOCK_UNLOCKED((name).lock),		\
	.count		= n,						\
	.wait_list	= LIST_HEAD_INIT((name).wait_list),		\
}

// 初始化互斥锁（二元信号量）
#define DECLARE_MUTEX(name)	\
	struct semaphore name = __SEMAPHORE_INITIALIZER(name, 1)

// 以指定的计数值初始化动态创建的信号量
static inline void sema_init(struct semaphore *sem, int val)
{	
	// struct lock_class_key 是锁的分类关键字的结构体类型。锁的分类关键字用于标记锁的类型和用途，以便在调试和分析时进行跟踪和记录。
	static struct lock_class_key __key;	// 静态变量，用于锁的分类关键字
	*sem = (struct semaphore) __SEMAPHORE_INITIALIZER(*sem, val);
	/**
	 * 用于初始化锁依赖映射的函数调用。它会初始化信号量对象中的锁依赖映射 dep_map。
	 * 锁依赖映射用于在调试和分析时追踪锁的依赖关系。第一个参数 &sem->lock.dep_map 表示要初始化的锁依赖映射对象，
	 * 第二个参数 "semaphore->lock" 是用于标识该锁依赖映射的字符串，
	 * 第三个参数 &__key 是锁的分类关键字，第四个参数 0 是初始化标志
	 */
	lockdep_init_map(&sem->lock.dep_map, "semaphore->lock", &__key, 0);	// 初始化锁依赖映射
}

// 初始化一个动态创建的互斥锁（以计数值1初始化动态创建的信号量）
#define init_MUTEX(sem)		sema_init(sem, 1)
// 以计数值为0初始化动态创建的信号量(初始为加锁状态)
#define init_MUTEX_LOCKED(sem)	sema_init(sem, 0)

// 试图获取指定信号量，如果信号量不可用，进程进入TASK_UNTERRUPTIBLE状态。
extern void down(struct semaphore *sem);
// 试图获取指定信号量，如果信号量不可用，进程进入TASK_INTERRUPTIBLE状态。如果进程等待信号量被信号唤醒，那么该函数返回-EINTR
extern int __must_check down_interruptible(struct semaphore *sem);
// 用于尝试获取指定信号量。如果信号量已经被占用，则函数会在等待期间可被终止（通过发送一个信号），并立即返回非零值。如果成功获取了信号量锁，则函数返回 0。
extern int __must_check down_killable(struct semaphore *sem);
// 尝试获取指定信号量。信号量被占用时，立刻返回非0值，否则返回0，并成功持有信号量锁
extern int __must_check down_trylock(struct semaphore *sem);
// 用于尝试获取指定信号量。如果信号量已经被占用，则函数会在指定的时间间隔内等待，直到成功获取信号量锁或超时。参数 jiffies 是一个时间戳，表示等待的时间，以节拍数为单位。如果成功获取了信号量锁，则函数返回 0。如果在指定时间内未能获取锁，则函数返回非零值。
extern int __must_check down_timeout(struct semaphore *sem, long jiffies);
// 释放指定信号量，如果睡眠队列不为空，则唤醒其中一个
extern void up(struct semaphore *sem);

#endif /* __LINUX_SEMAPHORE_H */
