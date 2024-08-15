/*
 * descriptor table internals; you almost certainly want file.h instead.
 */

#ifndef __LINUX_FDTABLE_H
#define __LINUX_FDTABLE_H

#include <linux/posix_types.h>
#include <linux/compiler.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/types.h>
#include <linux/init.h>

#include <asm/atomic.h>

/*
 * The default fd array needs to be at least BITS_PER_LONG,
 * as this is the granularity returned by copy_fdset().
 */
/*
 * 默认的文件描述符数组至少需要 BITS_PER_LONG 个元素，
 * 因为这是 copy_fdset() 返回的粒度。
 */
#define NR_OPEN_DEFAULT BITS_PER_LONG  // 定义默认的文件描述符数量为 BITS_PER_LONG，即系统中一个 `long` 类型所占的位数

/*
 * The embedded_fd_set is a small fd_set,
 * suitable for most tasks (which open <= BITS_PER_LONG files)
 */
/*
 * embedded_fd_set 是一个小型的 fd_set，
 * 适用于大多数任务（打开的文件数 <= BITS_PER_LONG）
 */
struct embedded_fd_set {
	unsigned long fds_bits[1];  // 文件描述符位图，大小为一个 `unsigned long`，即 BITS_PER_LONG 位
};

// 定义文件描述符表的结构体，用于管理进程的文件描述符信息
struct fdtable {
	unsigned int max_fds;  // 文件描述符表能够容纳的最大文件描述符数量
  /* current fd array */
	struct file **fd;      /* 当前的文件描述符数组 */
	fd_set *close_on_exec;  // 执行 `exec` 系列系统调用时需要关闭的文件描述符集合
	fd_set *open_fds;       // 当前已打开的文件描述符集合
	struct rcu_head rcu;    // RCU（Read-Copy Update）头部，用于同步文件描述符表的更新
	struct fdtable *next;   // 指向下一个文件描述符表，通常用于延迟释放
};

/*
 * Open file table structure
 */
/*
 * 打开文件表结构
 */
// 每个进程多有自己的一组打开的文件，像跟文件系统，当前工作目录，安装点等。
// 有三个将VFS层和系统的进程紧密联系在一起，它们分别是file_struct, fs_struct, namespace(mnt_namespace)结构体。
// 该结构体由进程描述符的files目录项指向，所有与单个进程相关的信息(如打开的文件及文件描述符)都包含在其中
struct files_struct {
  /*
   * read mostly part
   */
	/*
   * 主要用于读取的部分
   */
	atomic_t count;  // 结构的使用计数，用于引用计数，多个线程共享时需要进行计数管理
	struct fdtable *fdt;  // 指向文件描述符表 (fdtable) 的指针，表示当前进程使用的文件描述符表
	struct fdtable fdtab;  // 基础文件描述符表 (fdtable)，如果文件描述符数量较少，可以直接使用这个表

  /*
   * written part on a separate cache line in SMP
   */
	/*
   * SMP 系统中写操作部分，放在单独的缓存行中
   */
	spinlock_t file_lock ____cacheline_aligned_in_smp;	// 自旋锁，用于在多处理器系统中保护文件描述符表的并发访问
	int next_fd;		// 缓存下一个可用的文件描述符，用于快速分配新的文件描述符
	struct embedded_fd_set close_on_exec_init;	// 初始化时用于存储 exec() 调用时需要关闭的文件描述符的位图
	struct embedded_fd_set open_fds_init;		// 初始化时用于存储已打开文件描述符的位图
	// fd_array数组指向已打开的文件对象。如果一个进程打开的文件对象超过NR_OPEN_DEFAULT，那么内核就会重新分配一个数组，
	// 并且将fdt指针指向它。如果有大量的进程打开的文件描述符都超过NR_OPEN_DEFAULT，那么为了减少内存分配次数，
	// 提高性能，可以适当的提高NR_OPEN_DEFAULT的值。
	struct file * fd_array[NR_OPEN_DEFAULT];	// 默认的文件对象数组，大小为 NR_OPEN_DEFAULT，默认是 64
};

/*
 * rcu_dereference_check_fdtable 是一个宏，用于在访问文件描述符表 (fdtable) 时进行检查。
 * 这个宏使用 RCU（读-复制-更新）机制来安全地访问文件描述符表，并确保在以下情况下安全：
 * - 持有 RCU 读锁 (rcu_read_lock)；
 * - 持有文件锁 (file_lock)；
 * - 文件结构 (files_struct) 只有一个引用（即未被其他线程共享）。
 */
#define rcu_dereference_check_fdtable(files, fdtfd) \
	(rcu_dereference_check((fdtfd), \
			       rcu_read_lock_held() || \
			       lockdep_is_held(&(files)->file_lock) || \
			       atomic_read(&(files)->count) == 1))

/*
 * files_fdtable 是一个宏，用于获取与给定文件结构 (files_struct) 关联的文件描述符表。
 * 该宏通过 rcu_dereference_check_fdtable 来检查访问的安全性。
 */
#define files_fdtable(files) \
		(rcu_dereference_check_fdtable((files), (files)->fdt))

struct file_operations;
struct vfsmount;
struct dentry;

/*
 * 声明 expand_files 函数，它用于扩展文件描述符表，以容纳更多文件描述符。
 * 声明 free_fdtable_rcu 函数，它用于通过 RCU 机制释放文件描述符表。
 * 声明 files_defer_init 函数，它在系统初始化时被调用，用于初始化延迟释放机制。
 */
extern int expand_files(struct files_struct *, int nr);
extern void free_fdtable_rcu(struct rcu_head *rcu);
extern void __init files_defer_init(void);

/*
 * free_fdtable 是一个内联函数，用于通过 RCU 机制延迟释放文件描述符表。
 * 它通过调用 call_rcu 函数来安排 free_fdtable_rcu 函数在安全的时间点执行，以释放内存。
 */
static inline void free_fdtable(struct fdtable *fdt)
{
	call_rcu(&fdt->rcu, free_fdtable_rcu);
}

/*
 * fcheck_files 是一个内联函数，用于检查给定文件结构 (files_struct) 中的特定文件描述符 (fd)。
 * 如果文件描述符在文件描述符表的范围内，并且指向有效的文件对象，则返回指向该文件对象的指针；
 * 否则返回 NULL。
 */
static inline struct file * fcheck_files(struct files_struct *files, unsigned int fd)
{
	struct file * file = NULL;  // 初始化文件指针为 NULL
	struct fdtable *fdt = files_fdtable(files);  // 获取文件描述符表指针

	if (fd < fdt->max_fds)  // 如果文件描述符在表的范围内
		file = rcu_dereference_check_fdtable(files, fdt->fd[fd]);  // 安全地获取文件指针
	return file;  // 返回文件指针，如果文件描述符无效则返回 NULL
}

/*
 * Check whether the specified fd has an open file.
 */
/*
 * 检查指定的文件描述符是否有一个已打开的文件。
 */
#define fcheck(fd)	fcheck_files(current->files, fd)

/*
 * fcheck 是一个宏，用于检查当前进程的指定文件描述符 (fd) 是否有一个已打开的文件。
 * 它调用 fcheck_files 函数，并传递当前进程的文件结构 (files_struct) 和文件描述符 (fd)。
 * 如果该文件描述符有效且对应一个已打开的文件，宏将返回文件指针；否则返回 NULL。
 */
struct task_struct;  // 前向声明 task_struct 结构，表示进程的主要数据结构

/*
 * 获取指定任务 (task_struct) 的文件结构 (files_struct)。
 */
struct files_struct *get_files_struct(struct task_struct *);
/*
 * 释放一个文件结构 (files_struct)，减少其引用计数。
 * 如果引用计数降为 0，则释放与其关联的所有资源。
 */
void put_files_struct(struct files_struct *fs);
/*
 * 重置文件结构 (files_struct)，通常在进程执行时调用，以确保文件描述符表与新进程的环境一致。
 */
void reset_files_struct(struct files_struct *);
/*
 * 创建当前进程的文件结构 (files_struct) 的副本，并使其与其他进程的文件结构不再共享。
 * 如果成功，返回 0；否则返回错误代码。
 */
int unshare_files(struct files_struct **);
/*
 * 复制给定的文件结构 (files_struct)，创建一个新的副本并返回指向新副本的指针。
 * 如果失败，返回 NULL，并在 errorp 中设置错误代码。
 */
struct files_struct *dup_fd(struct files_struct *, int *);

/*
 * 一个用于文件结构 (files_struct) 的内存缓存池，供内核使用以优化内存分配。
 */
extern struct kmem_cache *files_cachep;
#endif /* __LINUX_FDTABLE_H */
