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
#define NR_OPEN_DEFAULT BITS_PER_LONG

/*
 * The embedded_fd_set is a small fd_set,
 * suitable for most tasks (which open <= BITS_PER_LONG files)
 */
struct embedded_fd_set {
	unsigned long fds_bits[1];
};

struct fdtable {
	unsigned int max_fds;
	struct file ** fd;      /* current fd array */
	fd_set *close_on_exec;
	fd_set *open_fds;
	struct rcu_head rcu;
	struct fdtable *next;
};

/*
 * Open file table structure
 */
// 每个进程多有自己的一组打开的文件，像跟文件系统，当前工作目录，安装点等。有三个将VFS层和系统的进程紧密联系在一起，它们分别是file_struct, fs_struct, namespace结构体。
// 该结构体由进程描述符的files目录项指向，所有与单个进程相关的信息(如打开的文件及文件描述符)都包含在其中
struct files_struct {
  /*
   * read mostly part
   */
	atomic_t count;		// 结构的使用计数
	struct fdtable *fdt;		// 指向其他fd表的指针
	struct fdtable fdtab;		// 基fd表
  /*
   * written part on a separate cache line in SMP
   */
	spinlock_t file_lock ____cacheline_aligned_in_smp;
	int next_fd;		// 缓存下一个可用的fd
	struct embedded_fd_set close_on_exec_init;	// exec()时关闭的文件描述符表
	struct embedded_fd_set open_fds_init;		// 打开的文件描述符链表
	// fd_array数组指向已打开的文件对象。如果一个进程打开的文件对象超过NR_OPEN_DEFAULT，那么内核就会重新分配一个数组，
	// 并且将fdt指针指向它。
	struct file * fd_array[NR_OPEN_DEFAULT];	// 缺省的文件对象数组
};

#define rcu_dereference_check_fdtable(files, fdtfd) \
	(rcu_dereference_check((fdtfd), \
			       rcu_read_lock_held() || \
			       lockdep_is_held(&(files)->file_lock) || \
			       atomic_read(&(files)->count) == 1))

#define files_fdtable(files) \
		(rcu_dereference_check_fdtable((files), (files)->fdt))

struct file_operations;
struct vfsmount;
struct dentry;

extern int expand_files(struct files_struct *, int nr);
extern void free_fdtable_rcu(struct rcu_head *rcu);
extern void __init files_defer_init(void);

static inline void free_fdtable(struct fdtable *fdt)
{
	call_rcu(&fdt->rcu, free_fdtable_rcu);
}

static inline struct file * fcheck_files(struct files_struct *files, unsigned int fd)
{
	struct file * file = NULL;
	struct fdtable *fdt = files_fdtable(files);

	if (fd < fdt->max_fds)
		file = rcu_dereference_check_fdtable(files, fdt->fd[fd]);
	return file;
}

/*
 * Check whether the specified fd has an open file.
 */
#define fcheck(fd)	fcheck_files(current->files, fd)

struct task_struct;

struct files_struct *get_files_struct(struct task_struct *);
void put_files_struct(struct files_struct *fs);
void reset_files_struct(struct files_struct *);
int unshare_files(struct files_struct **);
struct files_struct *dup_fd(struct files_struct *, int *);

extern struct kmem_cache *files_cachep;

#endif /* __LINUX_FDTABLE_H */
