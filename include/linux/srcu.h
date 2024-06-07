/*
 * Sleepable Read-Copy Update mechanism for mutual exclusion
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) IBM Corporation, 2006
 *
 * Author: Paul McKenney <paulmck@us.ibm.com>
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 * 		Documentation/RCU/ *.txt
 *
 */

#ifndef _LINUX_SRCU_H
#define _LINUX_SRCU_H

/*
 * struct srcu_struct_array - 用于 SRCU 结构的辅助数组
 * 每个处理器核心都有自己的计数器数组，以跟踪对 SRCU 保护的数据结构的读取次数。
 */
struct srcu_struct_array {
	// 这是一个包含两个整数的数组，用于计算每个处理器核心的 SRCU 读取次数。
	int c[2];
};

/*
 * struct srcu_struct - 描述一个 SRCU 保护的数据结构。
 * 这个结构提供了进行锁操作所需的所有元数据。
 */
struct srcu_struct {
	// 一个计数器，用于跟踪已完成的 SRCU 更新周期。
	int completed;
	// 指向每个 CPU 的 srcu_struct_array 的指针，用于处理每个 CPU 的 SRCU 引用计数。
	struct srcu_struct_array __percpu *per_cpu_ref;
	// 一个互斥锁，用于同步对此 SRCU 结构的访问。
	struct mutex mutex;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	// 用于锁依赖性调试的结构。
	struct lockdep_map dep_map;
#endif /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */
};

#ifndef CONFIG_PREEMPT
/*
 * srcu_barrier - 在非抢占配置中定义为简单的内存屏障。
 * 用于确保编译器和硬件不会重排指令，从而确保 SRCU 读取和更新操作的顺序性。
 */
#define srcu_barrier() barrier()
#else /* #ifndef CONFIG_PREEMPT */
/*
 * 在抢占配置中，srcu_barrier 定义为空操作。
 * 在支持抢占的系统中，不需要额外的屏障来保持 SRCU 的顺序性，因为抢占本身会保证必要的同步和内存屏障。
 */
#define srcu_barrier()
#endif /* #else #ifndef CONFIG_PREEMPT */

#ifdef CONFIG_DEBUG_LOCK_ALLOC

int __init_srcu_struct(struct srcu_struct *sp, const char *name,
		       struct lock_class_key *key);

#define init_srcu_struct(sp) \
({ \
	static struct lock_class_key __srcu_key; \
	\
	__init_srcu_struct((sp), #sp, &__srcu_key); \
})

# define srcu_read_acquire(sp) \
		lock_acquire(&(sp)->dep_map, 0, 0, 2, 1, NULL, _THIS_IP_)
# define srcu_read_release(sp) \
		lock_release(&(sp)->dep_map, 1, _THIS_IP_)

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

int init_srcu_struct(struct srcu_struct *sp);

# define srcu_read_acquire(sp)  do { } while (0)
# define srcu_read_release(sp)  do { } while (0)

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

void cleanup_srcu_struct(struct srcu_struct *sp);
int __srcu_read_lock(struct srcu_struct *sp) __acquires(sp);
void __srcu_read_unlock(struct srcu_struct *sp, int idx) __releases(sp);
void synchronize_srcu(struct srcu_struct *sp);
void synchronize_srcu_expedited(struct srcu_struct *sp);
long srcu_batches_completed(struct srcu_struct *sp);

#ifdef CONFIG_DEBUG_LOCK_ALLOC

/**
 * srcu_read_lock_held - might we be in SRCU read-side critical section?
 *
 * If CONFIG_PROVE_LOCKING is selected and enabled, returns nonzero iff in
 * an SRCU read-side critical section.  In absence of CONFIG_PROVE_LOCKING,
 * this assumes we are in an SRCU read-side critical section unless it can
 * prove otherwise.
 */
static inline int srcu_read_lock_held(struct srcu_struct *sp)
{
	if (debug_locks)
		return lock_is_held(&sp->dep_map);
	return 1;
}

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

static inline int srcu_read_lock_held(struct srcu_struct *sp)
{
	return 1;
}

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/**
 * srcu_dereference - fetch SRCU-protected pointer with checking
 *
 * Makes rcu_dereference_check() do the dirty work.
 */
#define srcu_dereference(p, sp) \
		rcu_dereference_check(p, srcu_read_lock_held(sp))

/**
 * srcu_read_lock - register a new reader for an SRCU-protected structure.
 * @sp: srcu_struct in which to register the new reader.
 *
 * Enter an SRCU read-side critical section.  Note that SRCU read-side
 * critical sections may be nested.
 */
static inline int srcu_read_lock(struct srcu_struct *sp) __acquires(sp)
{
	int retval = __srcu_read_lock(sp);

	srcu_read_acquire(sp);
	return retval;
}

/**
 * srcu_read_unlock - unregister a old reader from an SRCU-protected structure.
 * @sp: srcu_struct in which to unregister the old reader.
 * @idx: return value from corresponding srcu_read_lock().
 *
 * Exit an SRCU read-side critical section.
 */
static inline void srcu_read_unlock(struct srcu_struct *sp, int idx)
	__releases(sp)
{
	srcu_read_release(sp);
	__srcu_read_unlock(sp, idx);
}

#endif
