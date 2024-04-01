// 读写信号量接口，还有一些和架构相关，在arch/x86/include/asm/rwsem.h  
/* rwsem.h: R/W semaphores, public interface
 *
 * Written by David Howells (dhowells@redhat.com).
 * Derived from asm-i386/semaphore.h
 */

#ifndef _LINUX_RWSEM_H
#define _LINUX_RWSEM_H

#include <linux/linkage.h>

#include <linux/types.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/atomic.h>

// 所有读写信号量的引用计数都等于1，虽然它们只对写者互斥，不对读者。只要没写者，读者数量不限，只有唯一写者可以获得写锁。所有读写锁的睡眠都不能被信号打断
struct rw_semaphore;

// 如下俩文件定义了一些读写信号量实现和体系结构相关的读写信号量的代码
#ifdef CONFIG_RWSEM_GENERIC_SPINLOCK
// 通用代码
#include <linux/rwsem-spinlock.h> /* use a generic implementation */
#else
// 特定架构代码
#include <asm/rwsem.h> /* use an arch-specific implementation */
#endif

// 所有读写锁的睡眠都不会被信号打断，为什么？？？？   因为不会被信号打断，所以只有一种down操作

/*
 * lock for reading
 */
// 获取信号量用于读
extern void down_read(struct rw_semaphore *sem);

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
// 尝试加锁，返回值和普通信号量相反，获得锁返回非0,没获得返回0
extern int down_read_trylock(struct rw_semaphore *sem);

/*
 * lock for writing
 */
// 获取信号量用于写
extern void down_write(struct rw_semaphore *sem);

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
// 尝试加锁，返回值和普通信号量相反，获得锁返回非0,没获得返回0
extern int down_write_trylock(struct rw_semaphore *sem);

/*
 * release a read lock
 */
// 释放读信号量
extern void up_read(struct rw_semaphore *sem);

/*
 * release a write lock
 */
// 释放写信号量
extern void up_write(struct rw_semaphore *sem);

/*
 * downgrade write lock to read lock
 */
// 动态将写锁转换为读锁，读写信号量可以转，读写自旋锁不可以转
extern void downgrade_write(struct rw_semaphore *sem);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/*
 * nested locking. NOTE: rwsems are not allowed to recurse
 * (which occurs if the same task tries to acquire the same
 * lock instance multiple times), but multiple locks of the
 * same lock class might be taken, if the order of the locks
 * is always the same. This ordering rule can be expressed
 * to lockdep via the _nested() APIs, but enumerating the
 * subclasses that are used. (If the nesting relationship is
 * static then another method for expressing nested locking is
 * the explicit definition of lock class keys and the use of
 * lockdep_set_class() at lock initialization time.
 * See Documentation/lockdep-design.txt for more details.)
 */
extern void down_read_nested(struct rw_semaphore *sem, int subclass);
extern void down_write_nested(struct rw_semaphore *sem, int subclass);
/*
 * Take/release a lock when not the owner will release it.
 *
 * [ This API should be avoided as much as possible - the
 *   proper abstraction for this case is completions. ]
 */
extern void down_read_non_owner(struct rw_semaphore *sem);
extern void up_read_non_owner(struct rw_semaphore *sem);
#else
# define down_read_nested(sem, subclass)		down_read(sem)
# define down_write_nested(sem, subclass)	down_write(sem)
# define down_read_non_owner(sem)		down_read(sem)
# define up_read_non_owner(sem)			up_read(sem)
#endif

#endif /* _LINUX_RWSEM_H */
