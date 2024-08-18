/*
 *  linux/fs/file_table.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  Copyright (C) 1997 David S. Miller (davem@caip.rutgers.edu)
 */

#include <linux/string.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/security.h>
#include <linux/eventpoll.h>
#include <linux/rcupdate.h>
#include <linux/mount.h>
#include <linux/capability.h>
#include <linux/cdev.h>
#include <linux/fsnotify.h>
#include <linux/sysctl.h>
#include <linux/percpu_counter.h>
#include <linux/ima.h>

#include <asm/atomic.h>

#include "internal.h"

/* sysctl tunables... */
/* 定义一个文件统计结构 files_stat，用于跟踪系统中文件的总数 */
struct files_stat_struct files_stat = {
	.max_files = NR_FILE // 最大文件数，NR_FILE 为预定义的最大文件描述符数量
};

/* public. Not pretty! */
/* 公开的数据结构，但这种方式并不优雅 */
/* 在 SMP（对称多处理）环境中按缓存行对齐 */
__cacheline_aligned_in_smp DEFINE_SPINLOCK(files_lock); // 定义一个自旋锁，用于文件系统的同步访问

/* SLAB cache for file structures */
/* 用于文件结构体的 SLAB 缓存 */
/* 这个变量在大多数情况下只读 */
static struct kmem_cache *filp_cachep __read_mostly; // 指向一个 kmem_cache 结构的指针，用于高效管理 file 结构的内存分配

/* 按缓存行在 SMP 中对齐的 percpu_counter 结构体 */
static struct percpu_counter nr_files __cacheline_aligned_in_smp; // 每个 CPU 都有自己的文件计数器，用于跟踪系统范围内打开的文件总数

/* 释放 file 结构体的内联函数 */
static inline void file_free_rcu(struct rcu_head *head)
{
	struct file *f = container_of(head, struct file, f_u.fu_rcuhead); // 从 rcu_head 结构中提取出 file 结构体的指针

	put_cred(f->f_cred); // 释放文件所关联的凭证内存
	kmem_cache_free(filp_cachep, f); // 将文件结构体的内存返回到 kmem_cache，以便再次使用
}

/* 定义一个用于释放 file 结构体的内联函数 */
static inline void file_free(struct file *f)
{
	percpu_counter_dec(&nr_files); // 减少每个 CPU 上的文件计数器，表示一个文件被关闭
	file_check_state(f); // 检查文件的状态，确保文件处于正确的状态以被释放
	call_rcu(&f->f_u.fu_rcuhead, file_free_rcu); // 使用 RCU 机制延迟释放文件结构体，保证安全释放
}

/*
 * Return the total number of open files in the system
 */
/*
 * 返回系统中打开的文件总数
 */
static int get_nr_files(void)
{
	return percpu_counter_read_positive(&nr_files); // 读取并返回系统中正数的打开文件计数
}

/*
 * Return the maximum number of open files in the system
 */
/*
 * 返回系统中最大的打开文件数量
 */
int get_max_files(void)
{
	return files_stat.max_files; // 返回文件统计结构中定义的最大文件数
}
EXPORT_SYMBOL_GPL(get_max_files);

/*
 * Handle nr_files sysctl
 */
/*
 * 处理 nr_files 的系统控制（sysctl）
 */
/* 检查是否定义了 CONFIG_SYSCTL 和 CONFIG_PROC_FS */
#if defined(CONFIG_SYSCTL) && defined(CONFIG_PROC_FS)
/* 如果定义了，则定义 proc_nr_files 函数，用于读写 sysctl 相关的文件数信息 */
int proc_nr_files(ctl_table *table, int write,
                     void __user *buffer, size_t *lenp, loff_t *ppos)
{
	files_stat.nr_files = get_nr_files(); // 更新 files_stat 结构的 nr_files 字段，确保它包含最新的打开文件数
	return proc_dointvec(table, write, buffer, lenp, ppos); // 调用 proc_dointvec 处理读写操作，此函数是内核中处理整数向量的标准函数
}
#else
/* 如果未定义 CONFIG_SYSCTL 或 CONFIG_PROC_FS，则定义 proc_nr_files 函数，总是返回 -ENOSYS */
int proc_nr_files(ctl_table *table, int write,
                     void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return -ENOSYS; // 返回 -ENOSYS 表示“功能未实现”，用于指示该配置在当前内核配置中不可用
}
#endif

/* Find an unused file structure and return a pointer to it.
 * Returns NULL, if there are no more free file structures or
 * we run out of memory.
 *
 * Be very careful using this.  You are responsible for
 * getting write access to any mount that you might assign
 * to this filp, if it is opened for write.  If this is not
 * done, you will imbalance int the mount's writer count
 * and a warning at __fput() time.
 */
/*
 * 查找未使用的文件结构并返回指向它的指针。
 * 如果没有更多的空闲文件结构或内存耗尽，则返回 NULL。
 *
 * 使用时需非常小心。如果分配给此 filp 的挂载点是为了写入打开的，
 * 您负责获取对任何可能分配给此 filp 的挂载点的写入访问权限。
 * 如果没有这样做，您将在挂载点的写入者计数中造成不平衡，
 * 并且在 __fput() 时会有警告。
 */
struct file *get_empty_filp(void)
{
	const struct cred *cred = current_cred(); // 获取当前的凭证信息
	static int old_max; // 静态变量，用于记录上一次达到的最大文件数
	struct file * f;

	/*
	 * Privileged users can go above max_files
	 */
	/*
	 * 有特权的用户可以超过 max_files 的限制
	 */
	if (get_nr_files() >= files_stat.max_files && !capable(CAP_SYS_ADMIN)) {
		/*
		 * percpu_counters are inaccurate.  Do an expensive check before
		 * we go and fail.
		 */
		/*
		 * percpu计数器是不精确的。在我们失败之前，先进行一次昂贵的检查。
		 */
		if (percpu_counter_sum_positive(&nr_files) >= files_stat.max_files)
			goto over;	// 如果文件数超过最大文件数，跳转到 over 处理
	}

	f = kmem_cache_zalloc(filp_cachep, GFP_KERNEL); // 从内核内存池中分配一个初始化为零的 file 结构
	if (f == NULL)
		goto fail; // 如果内存分配失败，则跳转到 fail 处理

	percpu_counter_inc(&nr_files); // 增加每 CPU 文件计数器
	if (security_file_alloc(f))
		goto fail_sec; // 如果文件分配安全检查失败，则跳转到 fail_sec 处理

	INIT_LIST_HEAD(&f->f_u.fu_list); // 初始化文件的 list 头
	atomic_long_set(&f->f_count, 1); // 设置文件引用计数为1
	rwlock_init(&f->f_owner.lock); // 初始化文件拥有者的读写锁
	f->f_cred = get_cred(cred); // 设置文件的凭证
	spin_lock_init(&f->f_lock); // 初始化文件的自旋锁
	eventpoll_init_file(f); // 初始化文件的事件轮询
	/* f->f_version: 0 */
	return f; // 返回文件结构的指针

over:
	/* Ran out of filps - report that */
		/* 超出文件结构的限制 - 报告该问题 */
	if (get_nr_files() > old_max) {
		printk(KERN_INFO "VFS: file-max limit %d reached\n",
					get_max_files()); // 打印达到最大文件数的信息
		old_max = get_nr_files(); // 更新记录的最大文件数
	}
	goto fail; // 跳转到失败处理

fail_sec:
	file_free(f); // 释放文件结构
fail:
	return NULL; // 返回 NULL 指示失败
}

/**
 * alloc_file - allocate and initialize a 'struct file'
 * @mnt: the vfsmount on which the file will reside
 * @dentry: the dentry representing the new file
 * @mode: the mode with which the new file will be opened
 * @fop: the 'struct file_operations' for the new file
 *
 * Use this instead of get_empty_filp() to get a new
 * 'struct file'.  Do so because of the same initialization
 * pitfalls reasons listed for init_file().  This is a
 * preferred interface to using init_file().
 *
 * If all the callers of init_file() are eliminated, its
 * code should be moved into this function.
 */
/**
 * alloc_file - 分配并初始化一个 'struct file'
 * @mnt: 文件将驻留的虚拟文件系统挂载点
 * @dentry: 表示新文件的目录项
 * @mode: 打开新文件的模式
 * @fop: 新文件的 'struct file_operations'
 *
 * 使用这个函数来获取一个新的 'struct file'，而不是使用 get_empty_filp()。
 * 原因与 init_file() 中列出的初始化陷阱类似。相比直接使用 init_file()，这是一个优选的接口。
 *
 * 如果所有调用 init_file() 的地方都被替换掉，init_file() 的代码应被移到此函数中。
 */
struct file *alloc_file(struct path *path, fmode_t mode,
		const struct file_operations *fop)
{
	struct file *file;

	file = get_empty_filp(); // 获取一个空闲的 file 结构
	if (!file)
		return NULL; // 如果获取失败，返回 NULL

	file->f_path = *path; // 设置 file 结构的路径
	file->f_mapping = path->dentry->d_inode->i_mapping; // 设置文件的映射，指向 inode 的映射
	file->f_mode = mode; // 设置文件的打开模式
	file->f_op = fop; // 设置文件的操作函数指针

	/*
	 * These mounts don't really matter in practice
	 * for r/o bind mounts.  They aren't userspace-
	 * visible.  We do this for consistency, and so
	 * that we can do debugging checks at __fput()
	 */
	/*
	 * 这些挂载点对于只读绑定挂载实际上并不重要。
	 * 它们对用户空间是不可见的。我们这样做是为了保持一致性，
	 * 并且为了可以在 __fput() 时进行调试检查。
	 */
	if ((mode & FMODE_WRITE) && !special_file(path->dentry->d_inode->i_mode)) {
		file_take_write(file); // 如果文件是以写入模式打开的，并且不是特殊文件，增加写入者计数
		WARN_ON(mnt_clone_write(path->mnt)); // 如果克隆写入失败，发出警告
	}
	ima_counts_get(file); // 更新 IMA（完整性度量架构）计数
	return file; // 返回初始化后的文件结构指针
}
EXPORT_SYMBOL(alloc_file);

/*
 * fput - 释放文件结构的函数
 * @file: 需要释放的文件结构指针
 */
void fput(struct file *file)
{
	// 如果文件的引用计数减少到0，则调用 __fput() 释放文件结构
	if (atomic_long_dec_and_test(&file->f_count)) // 原子地递减文件的引用计数，并检查是否变为0
		__fput(file); // 如果引用计数为0，调用 __fput() 进行实际的释放操作
}

EXPORT_SYMBOL(fput);

/**
 * drop_file_write_access - give up ability to write to a file
 * @file: the file to which we will stop writing
 *
 * This is a central place which will give up the ability
 * to write to @file, along with access to write through
 * its vfsmount.
 */
/**
 * drop_file_write_access - 放弃对文件的写入能力
 * @file: 我们将停止写入的文件
 *
 * 这是一个核心位置，将放弃对文件的写入能力，
 * 以及通过其虚拟文件系统挂载点的写入访问权限。
 */
void drop_file_write_access(struct file *file)
{
	struct vfsmount *mnt = file->f_path.mnt; // 获取文件所在的虚拟文件系统挂载点
	struct dentry *dentry = file->f_path.dentry; // 获取文件的目录项
	struct inode *inode = dentry->d_inode; // 从目录项获取索引节点（inode）

	put_write_access(inode); // 降低索引节点的写入访问计数

	if (special_file(inode->i_mode)) // 如果是特殊文件（如设备文件）
		return; // 直接返回，不做进一步操作

	if (file_check_writeable(file) != 0) // 检查文件是否可写
		return; // 如果不可写（例如，已经关闭了写入访问），则直接返回

	mnt_drop_write(mnt); // 放弃挂载点的写入权限
	file_release_write(file); // 释放文件的写入权限
}
EXPORT_SYMBOL_GPL(drop_file_write_access);

/* __fput is called from task context when aio completion releases the last
 * last use of a struct file *.  Do not use otherwise.
 */
/*
 * __fput 从任务上下文中调用，当异步 I/O 完成释放对 struct file 的最后使用时调用。
 * 除此之外不要使用此函数。
 */
void __fput(struct file *file)
{
	struct dentry *dentry = file->f_path.dentry; // 获取文件的目录项
	struct vfsmount *mnt = file->f_path.mnt; // 获取文件的挂载点
	struct inode *inode = dentry->d_inode; // 获取文件的索引节点

	might_sleep(); // 表示此函数可能会使进程睡眠

	fsnotify_close(file); // 通知文件系统有文件关闭
	/*
	 * The function eventpoll_release() should be the first called
	 * in the file cleanup chain.
	 */
	/*
	 * 在文件清理链的最开始应该调用 eventpoll_release() 函数。
	 */
	eventpoll_release(file); // 释放与 event poll 相关的资源
	locks_remove_flock(file); // 移除文件上的锁

	if (unlikely(file->f_flags & FASYNC)) { // 如果文件设置了异步通知标志
		if (file->f_op && file->f_op->fasync) // 如果文件操作结构中定义了 fasync 函数
			file->f_op->fasync(-1, file, 0); // 调用 fasync 函数来处理异步通知
	}
	if (file->f_op && file->f_op->release) // 如果文件操作结构中定义了 release 函数
		file->f_op->release(inode, file); // 调用 release 函数来关闭文件

	security_file_free(file); // 调用安全模块的文件释放函数
	ima_file_free(file); // 处理 IMA 相关的文件释放逻辑
	if (unlikely(S_ISCHR(inode->i_mode) && inode->i_cdev != NULL)) // 如果是字符设备文件并且存在字符设备
		cdev_put(inode->i_cdev); // 递减字符设备的引用计数
	fops_put(file->f_op); // 递减文件操作结构的引用计数
	put_pid(file->f_owner.pid); // 递减文件所有者 PID 的引用计数
	file_kill(file); // 标记文件为死亡状态
	if (file->f_mode & FMODE_WRITE) // 如果文件是以写模式打开的
		drop_file_write_access(file); // 放弃写访问权限
	file->f_path.dentry = NULL; // 清空文件的目录项指针
	file->f_path.mnt = NULL; // 清空文件的挂载点指针
	file_free(file); // 释放文件结构
	dput(dentry); // 递减目录项的引用计数
	mntput(mnt); // 递减挂载点的引用计数
}

/*
 * fget - 根据文件描述符获取文件结构的引用
 * @fd: 文件描述符
 * 返回文件结构的指针，如果文件不存在或无法获取引用，则返回 NULL。
 */
struct file *fget(unsigned int fd)
{
	struct file *file;
	struct files_struct *files = current->files; // 获取当前任务的文件结构

	rcu_read_lock(); // 进入 RCU 读锁定区域，保证在此区域内访问的数据结构的一致性

	file = fcheck_files(files, fd); // 检查文件描述符 fd 是否有效，并返回对应的文件结构
	if (file) { // 如果文件结构存在
		if (!atomic_long_inc_not_zero(&file->f_count)) { // 尝试增加文件引用计数，如果文件已被释放（引用计数为 0），则增加失败
			/* File object ref couldn't be taken */
			/* 文件对象的引用计数无法增加 */
			rcu_read_unlock(); // 释放 RCU 读锁
			return NULL; // 返回 NULL 表示获取文件结构失败
		}
	}
	rcu_read_unlock(); // 释放 RCU 读锁

	return file; // 返回文件结构的指针
}
EXPORT_SYMBOL(fget);

/*
 * Lightweight file lookup - no refcnt increment if fd table isn't shared. 
 * You can use this only if it is guranteed that the current task already 
 * holds a refcnt to that file. That check has to be done at fget() only
 * and a flag is returned to be passed to the corresponding fput_light().
 * There must not be a cloning between an fget_light/fput_light pair.
 */
/*
 * 轻量级文件查找 - 如果文件描述符表没有被共享，则不增加引用计数。
 * 只有在确保当前任务已经持有该文件的引用计数时才能使用此函数。
 * 该检查只需在 fget() 中进行一次，并且返回一个标志，该标志需要传递给相应的 fput_light()。
 * 在 fget_light() 和 fput_light() 对之间不能有克隆操作。
 */
struct file *fget_light(unsigned int fd, int *fput_needed)
{
	struct file *file;
	struct files_struct *files = current->files; // 获取当前进程的文件结构体

	*fput_needed = 0; // 默认设置为不需要放置引用
	if (likely((atomic_read(&files->count) == 1))) { // 如果文件描述符表没有被共享
		file = fcheck_files(files, fd); // 直接检查文件描述符，不需要加锁
	} else { // 如果文件描述符表可能被共享
		rcu_read_lock(); // 使用 RCU 锁保护
		file = fcheck_files(files, fd); // 检查文件描述符
		if (file) { // 如果文件存在
			if (atomic_long_inc_not_zero(&file->f_count)) // 尝试增加文件的引用计数
				*fput_needed = 1; // 如果增加成功，设置需要在之后调用 fput_light()
			else
				/* Didn't get the reference, someone's freed */
				/* 没有获取到引用，可能文件已经被释放 */
				file = NULL; // 设置文件指针为 NULL
		}
		rcu_read_unlock(); // 解锁 RCU
	}

	return file; // 返回文件指针
}

/*
 * put_filp - 减少文件结构的引用计数并可能释放文件
 * @file: 要操作的文件结构
 * 如果引用计数减到 0，则调用相关的清理函数并释放文件。
 */
void put_filp(struct file *file)
{
	if (atomic_long_dec_and_test(&file->f_count)) { // 原子性减少文件的引用计数并检查是否为0
		security_file_free(file); // 调用安全模块相关函数进行文件释放前的安全检查
		file_kill(file); // 从文件列表中移除文件
		file_free(file); // 释放文件结构
	}
}

/*
 * file_move - 将文件结构移动到指定的列表中
 * @file: 要移动的文件结构
 * @list: 目标列表头指针
 * 如果 list 为空则不执行任何操作。此函数用于在文件列表之间移动文件结构。
 */
void file_move(struct file *file, struct list_head *list)
{
	if (!list)
		return; // 如果列表头为空，直接返回
	file_list_lock(); // 锁定文件列表
	list_move(&file->f_u.fu_list, list); // 将文件移动到新的列表中
	file_list_unlock(); // 解锁文件列表
}

/*
 * file_kill - 从文件列表中删除文件结构
 * @file: 要删除的文件结构
 * 这个函数检查文件是否在列表中，如果是，则从中删除。
 */
void file_kill(struct file *file)
{
	if (!list_empty(&file->f_u.fu_list)) { // 检查文件是否在某个列表中
		file_list_lock(); // 锁定文件列表
		list_del_init(&file->f_u.fu_list); // 从列表中删除文件，并重新初始化列表项
		file_list_unlock(); // 解锁文件列表
	}
}

/*
 * fs_may_remount_ro - 检查是否可以将文件系统重新挂载为只读
 * @sb: 要检查的文件系统的超级块
 * 返回 1 表示可以安全地重新挂载为只读，返回 0 表示存在写入操作，不可重新挂载为只读。
 */
int fs_may_remount_ro(struct super_block *sb)
{
	struct file *file;

	/* Check that no files are currently opened for writing. */
	/* 检查当前是否有文件被打开为写模式 */
	file_list_lock(); // 锁定文件列表，以遍历文件
	list_for_each_entry(file, &sb->s_files, f_u.fu_list) { // 遍历超级块关联的所有文件
		struct inode *inode = file->f_path.dentry->d_inode; // 获取文件的索引节点

		/* File with pending delete? */
		/* 文件是否有待处理的删除？ */
		if (inode->i_nlink == 0) // 如果索引节点的链接计数为0，即文件已被删除但仍被打开
			goto too_bad; // 不可以重新挂载为只读

		/* Writeable file? */
		/* 文件是否可写？ */
		if (S_ISREG(inode->i_mode) && (file->f_mode & FMODE_WRITE)) // 如果是常规文件且文件模式为写
			goto too_bad; // 不可以重新挂载为只读
	}
	file_list_unlock(); // 解锁文件列表
	/* Tis' cool bro. */
	return 1; // 所有检查通过，可以重新挂载为只读

too_bad:
	file_list_unlock(); // 解锁文件列表后返回
	return 0; // 存在写入操作，不可重新挂载为只读
}

/**
 *	mark_files_ro - mark all files read-only
 *	@sb: superblock in question
 *
 *	All files are marked read-only.  We don't care about pending
 *	delete files so this should be used in 'force' mode only.
 */
/**
 *	mark_files_ro - 将所有文件标记为只读
 *	@sb: 相关的超级块
 *
 *	所有文件都被标记为只读。我们不关心有待删除的文件，因此这应该只在“强制”模式下使用。
 */
void mark_files_ro(struct super_block *sb)
{
	struct file *f;

retry: // 重试标记
	file_list_lock(); // 锁定文件列表
	list_for_each_entry(f, &sb->s_files, f_u.fu_list) { // 遍历超级块的所有文件
		struct vfsmount *mnt;
		if (!S_ISREG(f->f_path.dentry->d_inode->i_mode))
		       continue; // 如果不是常规文件，继续下一个文件
		if (!file_count(f))
			continue; // 如果文件没有引用计数，继续下一个文件
		if (!(f->f_mode & FMODE_WRITE))
			continue; // 如果文件不是写模式，继续下一个文件
		spin_lock(&f->f_lock); // 锁定文件的自旋锁
		f->f_mode &= ~FMODE_WRITE; // 修改文件模式，移除写权限
		spin_unlock(&f->f_lock); // 解锁文件的自旋锁
		if (file_check_writeable(f) != 0)
			continue; // 如果文件仍然是可写的，继续下一个文件
		file_release_write(f); // 释放文件的写权限
		mnt = mntget(f->f_path.mnt); // 增加挂载点的引用计数
		file_list_unlock(); // 解锁文件列表

		/*
		 * This can sleep, so we can't hold
		 * the file_list_lock() spinlock.
		 */
		/*
		 * 这个操作可能会导致睡眠，因此我们不能持有 file_list_lock() 自旋锁。
		 */
		mnt_drop_write(mnt); // 放弃挂载点的写权限
		mntput(mnt); // 减少挂载点的引用计数
		goto retry; // 返回并重试，确保所有文件都被标记
	}
	file_list_unlock(); // 最终解锁文件列表
}

/*
 * __init files_init - 初始化文件系统相关的结构
 * @mempages: 系统中的内存页总数
 * 
 * 此函数在系统启动时调用，用于初始化文件相关的内核缓存和其他结构。
 */
void __init files_init(unsigned long mempages)
{ 
	int n; 

	// 创建一个用于文件结构的缓存区，如果创建失败则内核会触发 panic
	filp_cachep = kmem_cache_create("filp", sizeof(struct file), 0,
			SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);

	/*
	 * One file with associated inode and dcache is very roughly 1K.
	 * Per default don't use more than 10% of our memory for files. 
	 */
	/*
	 * 一个文件及其关联的inode和dcache大约占用1K内存。
	 * 默认情况下，不要使用超过我们内存的10%用于文件。
	 */

	n = (mempages * (PAGE_SIZE / 1024)) / 10; // 计算最大文件数，基于总内存的10%
	files_stat.max_files = n; // 设置最大文件数量
	if (files_stat.max_files < NR_FILE) // 如果计算出的最大文件数小于系统定义的最小文件数
		files_stat.max_files = NR_FILE; // 使用系统定义的最小文件数
	files_defer_init(); // 延迟初始化其他文件相关的结构
	percpu_counter_init(&nr_files, 0); // 初始化每 CPU 文件计数器
}
