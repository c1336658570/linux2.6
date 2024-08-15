/*
 *  linux/fs/file.c
 *
 *  Copyright (C) 1998-1999, Stephen Tweedie and Bill Hawes
 *
 *  Manage the dynamic fd arrays in the process files_struct.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/time.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>

// 定义结构体 fdtable_defer 用于推迟释放含有 vmalloc 分配的集合/数组的 fdtables。
struct fdtable_defer {
	spinlock_t lock;  // 旋转锁，用于保护对此数据结构的并发访问。
	struct work_struct wq;  // 工作队列结构，用于安排和执行延迟任务。
	struct fdtable *next;  // 指向下一个需要延迟释放的 fdtable 的指针。
};

// 系统控制变量，定义系统中允许打开的文件描述符的初始数量。
int sysctl_nr_open __read_mostly = 1024*1024;
// 系统控制变量，定义系统中允许打开的文件描述符的最小数量。
int sysctl_nr_open_min = BITS_PER_LONG;
// 系统控制变量，定义系统中允许打开的文件描述符的最大数量，随后可能会调整。
int sysctl_nr_open_max = 1024 * 1024; /* raised later */

/*
 * We use this list to defer free fdtables that have vmalloced
 * sets/arrays. By keeping a per-cpu list, we avoid having to embed
 * the work_struct in fdtable itself which avoids a 64 byte (i386) increase in
 * this per-task structure.
 */
/*
 * 我们使用这个列表来推迟释放那些拥有通过 vmalloc 分配的集合/数组的 fdtables。
 * 通过保持每个 CPU 的列表，我们避免了将 work_struct 嵌入到 fdtable 自身中，
 * 这避免了每个任务结构中 64 字节（在 i386 架构下）的增加。
 */
static DEFINE_PER_CPU(struct fdtable_defer, fdtable_defer_list);

// 定义内联函数 alloc_fdmem，根据所需大小分配内存，使用 kmalloc 或 vmalloc。
static inline void * alloc_fdmem(unsigned int size)
{
	if (size <= PAGE_SIZE)  // 如果所需大小小于等于一个页面的大小，使用 kmalloc 分配内存。
		return kmalloc(size, GFP_KERNEL);
	else  // 如果所需大小大于一个页面的大小，使用 vmalloc 分配内存。
		return vmalloc(size);
}

// 定义内联函数 free_fdarr，用于释放 fdtable 中的文件描述符数组。
static inline void free_fdarr(struct fdtable *fdt)
{
	if (fdt->max_fds <= (PAGE_SIZE / sizeof(struct file *)))  // 如果最大文件描述符数量小于等于一页内可以容纳的指针数量，使用 kfree 释放内存。
		kfree(fdt->fd);
	else  // 如果超过一页内可以容纳的指针数量，使用 vfree 释放内存。
		vfree(fdt->fd);
}

// 定义内联函数 free_fdset，用于释放文件描述符表中的 open_fds 字段。
static inline void free_fdset(struct fdtable *fdt)
{
	// 检查如果最大文件描述符数不大于一页大小的一半（按位计算），则使用 kfree 释放内存。
	if (fdt->max_fds <= (PAGE_SIZE * BITS_PER_BYTE / 2))
		kfree(fdt->open_fds);
	else  // 否则，使用 vfree 释放内存。
		vfree(fdt->open_fds);
}

// 定义函数 free_fdtable_work，作为工作队列结构中的回调函数，用于释放 fdtable。
static void free_fdtable_work(struct work_struct *work)
{
	// 通过宏 container_of 从 work_struct 成员获取其容器结构 fdtable_defer 的指针。
	struct fdtable_defer *f =
		container_of(work, struct fdtable_defer, wq);
	struct fdtable *fdt;

	// 锁定，保护对下一个 fdtable 的访问。
	spin_lock_bh(&f->lock);
	// 取出 fdtable 列表的第一个元素。
	fdt = f->next;
	// 将 fdtable 链表的头指针设置为 NULL。
	f->next = NULL;
	// 解锁。
	spin_unlock_bh(&f->lock);
	// 遍历 fdtable 链表并释放每个 fdtable。
	while(fdt) {
		// 保存下一个 fdtable 的地址。
		struct fdtable *next = fdt->next;
		// 使用 vfree 释放 fdtable 的 fd 数组。
		vfree(fdt->fd);
		// 调用 free_fdset 释放 fdtable 的 open_fds 数组。
		free_fdset(fdt);
		// 使用 kfree 释放 fdtable 结构本身。
		kfree(fdt);
		// 移动到链表中的下一个 fdtable。
		fdt = next;
	}
}

// 定义函数 free_fdtable_rcu，用于通过 RCU (读-复制-更新) 机制释放 fdtable 结构。
void free_fdtable_rcu(struct rcu_head *rcu)
{
	// 通过 RCU 结构获取 fdtable 的指针。
	struct fdtable *fdt = container_of(rcu, struct fdtable, rcu);
	struct fdtable_defer *fddef;

	// 确保 fdtable 不为空。
	BUG_ON(!fdt);

	// 如果 fdtable 的最大文件描述符数量小于等于默认值，意味着 fdtable 嵌入在 files 结构体中。
	if (fdt->max_fds <= NR_OPEN_DEFAULT) {
		/*
		 * This fdtable is embedded in the files structure and that
		 * structure itself is getting destroyed.
		 */
		/* 此 fdtable 嵌入在 files 结构体中，而该结构体本身正在被销毁。 */
		kmem_cache_free(files_cachep,
				container_of(fdt, struct files_struct, fdtab));
		return;
	}
	// 如果 fdtable 的最大文件描述符数量小于等于一页内可以容纳的指针数量，直接使用 kfree 释放。
	if (fdt->max_fds <= (PAGE_SIZE / sizeof(struct file *))) {
		kfree(fdt->fd);
		kfree(fdt->open_fds);
		kfree(fdt);
	} else {
		// 否则，将释放操作推迟到工作队列中处理。
		fddef = &get_cpu_var(fdtable_defer_list);
		spin_lock(&fddef->lock);
		fdt->next = fddef->next;
		fddef->next = fdt;
		/* vmallocs are handled from the workqueue context */
		/* 通过工作队列上下文处理 vmalloc 分配的内存 */
		schedule_work(&fddef->wq);
		spin_unlock(&fddef->lock);
		put_cpu_var(fdtable_defer_list);
	}
}

/*
 * Expand the fdset in the files_struct.  Called with the files spinlock
 * held for write.
 */
/*
 * 在 files_struct 中扩展 fdset。调用时必须持有用于写操作的 files 自旋锁。
 */
static void copy_fdtable(struct fdtable *nfdt, struct fdtable *ofdt)
{
	unsigned int cpy, set;

	// 确保新的 fdtable 有足够的空间来包含所有旧 fdtable 的文件描述符。
	BUG_ON(nfdt->max_fds < ofdt->max_fds);

	// 计算需要复制的文件指针的字节数和需要置零的字节数。
	cpy = ofdt->max_fds * sizeof(struct file *);
	set = (nfdt->max_fds - ofdt->max_fds) * sizeof(struct file *);
	// 复制旧 fdtable 中的文件指针到新的 fdtable。
	memcpy(nfdt->fd, ofdt->fd, cpy);
	// 将新 fdtable 中剩余的部分置零。
	memset((char *)(nfdt->fd) + cpy, 0, set);

	// 计算需要复制的打开文件描述符位图的字节数和需要置零的字节数。
	cpy = ofdt->max_fds / BITS_PER_BYTE;
	set = (nfdt->max_fds - ofdt->max_fds) / BITS_PER_BYTE;
	// 复制旧的打开文件描述符位图到新的 fdtable。
	memcpy(nfdt->open_fds, ofdt->open_fds, cpy);
	// 将新的打开文件描述符位图中剩余的部分置零。
	memset((char *)(nfdt->open_fds) + cpy, 0, set);
	// 复制旧的执行关闭标志位图到新的 fdtable。
	memcpy(nfdt->close_on_exec, ofdt->close_on_exec, cpy);
	// 将新的执行关闭标志位图中剩余的部分置零。
	memset((char *)(nfdt->close_on_exec) + cpy, 0, set);
}

// 定义一个函数，用于分配一个新的文件描述符表结构。
static struct fdtable * alloc_fdtable(unsigned int nr)
{
	struct fdtable *fdt;
	char *data;

	/*
	 * Figure out how many fds we actually want to support in this fdtable.
	 * Allocation steps are keyed to the size of the fdarray, since it
	 * grows far faster than any of the other dynamic data. We try to fit
	 * the fdarray into comfortable page-tuned chunks: starting at 1024B
	 * and growing in powers of two from there on.
	 */
	/*
	 * 确定在此 fdtable 中实际想要支持多少个文件描述符。
	 * 分配步骤依据 fdarray 的大小，因为它比其他动态数据增长得快得多。
	 * 我们尝试将 fdarray 放入适当的以页为单位的大小块中：从 1024B 开始，
	 * 从那里以二的幂次增长。
	 */
	nr /= (1024 / sizeof(struct file *));   // 调整 nr 以适应每页可以容纳的文件指针数
	nr = roundup_pow_of_two(nr + 1);         // 将 nr 调整为最接近的二次幂
	nr *= (1024 / sizeof(struct file *));    // 将 nr 转换回文件描述符的实际数量
	
	/*
	 * Note that this can drive nr *below* what we had passed if sysctl_nr_open
	 * had been set lower between the check in expand_files() and here.  Deal
	 * with that in caller, it's cheaper that way.
	 *
	 * We make sure that nr remains a multiple of BITS_PER_LONG - otherwise
	 * bitmaps handling below becomes unpleasant, to put it mildly...
	 */
	/*
	 * 注意，这可能会使 nr *低于*我们传递的值，如果在 expand_files() 中的检查和这里之间
	 * sysctl_nr_open 被设置得更低。在调用者中处理这一点，那样更省事。
	 *
	 * 我们确保 nr 保持为 BITS_PER_LONG 的倍数——否则下面的位图处理就会变得非常不愉快...
	 */
	
	if (unlikely(nr > sysctl_nr_open))
		nr = ((sysctl_nr_open - 1) | (BITS_PER_LONG - 1)) + 1;	// 确保 nr 是 BITS_PER_LONG 的倍数

	// 为 fdtable 结构体分配内存。
	fdt = kmalloc(sizeof(struct fdtable), GFP_KERNEL);
	if (!fdt)
		goto out;	// 如果失败，释放已分配的 fdtable 并返回 NULL
	fdt->max_fds = nr;	 // 设置 fdtable 的最大文件描述符数量
	// 为 fdtable 中的文件指针数组分配内存。
	data = alloc_fdmem(nr * sizeof(struct file *));
	if (!data)
		goto out_fdt;	// 如果失败，跳到函数末尾并返回 NULL
	fdt->fd = (struct file **)data;	// 将分配的内存赋给 fd 指针数组
	// 为打开文件描述符位图和执行关闭位图分配内存。
	data = alloc_fdmem(max_t(unsigned int,
				 2 * nr / BITS_PER_BYTE, L1_CACHE_BYTES));	// 分配足够容纳位图的内存
	if (!data)
		goto out_arr;	// 如果失败，释放已分配的文件指针数组和 fdtable，然后返回 NULL
	fdt->open_fds = (fd_set *)data;	// 将分配的内存赋给打开文件描述符位图
	data += nr / BITS_PER_BYTE;	// 调整指针，指向执行关闭位图的起始位置
	fdt->close_on_exec = (fd_set *)data;	// 将剩余的内存赋给执行关闭位图
	// 初始化 RCU 头部。
	INIT_RCU_HEAD(&fdt->rcu);
	fdt->next = NULL;	// 将 fdtable 的下一个指针设为 NULL

	return fdt;		// 返回分配和初始化好的 fdtable

out_arr:
	// 如果在为位图分配内存时失败，释放文件指针数组。
	free_fdarr(fdt);
out_fdt:
	// 如果在为文件指针数组分配内存时失败，释放 fdtable 结构。
	kfree(fdt);
out:
	// 如果分配 fdtable 结构失败，返回 NULL。
	return NULL;
}

/*
 * Expand the file descriptor table.
 * This function will allocate a new fdtable and both fd array and fdset, of
 * the given size.
 * Return <0 error code on error; 1 on successful completion.
 * The files->file_lock should be held on entry, and will be held on exit.
 */
/*
 * 扩展文件描述符表。
 * 此函数将分配一个新的 fdtable 以及给定大小的 fd 数组和 fdset。
 * 错误时返回小于0的错误代码；成功完成时返回 1。
 * 调用时应持有 files->file_lock，返回时依然持有。
 */
static int expand_fdtable(struct files_struct *files, int nr)
	__releases(files->file_lock)	// 表示此函数释放 file_lock
	__acquires(files->file_lock)	// 表示此函数重新获得 file_lock
{
	
	struct fdtable *new_fdt, *cur_fdt;

	spin_unlock(&files->file_lock); // 释放文件锁以进行内存分配，减少锁持有时间
	new_fdt = alloc_fdtable(nr);    // 分配一个新的文件描述符表，大小为 nr
	spin_lock(&files->file_lock);   // 重新获得文件锁
	if (!new_fdt)
		return -ENOMEM;  // 如果内存分配失败，返回 ENOMEM
	/*
	 * extremely unlikely race - sysctl_nr_open decreased between the check in
	 * caller and alloc_fdtable().  Cheaper to catch it here...
	 */
	/*
	 * 极不可能的竞争情况 - sysctl_nr_open 在调用者检查和 alloc_fdtable() 之间被减少。
	 * 在这里捕获它比较简单...
	 */
	if (unlikely(new_fdt->max_fds <= nr)) {
		free_fdarr(new_fdt);  // 释放文件指针数组
		free_fdset(new_fdt);  // 释放文件描述符集
		kfree(new_fdt);       // 释放 fdtable 结构本身
		return -EMFILE;       // 返回文件过多的错误码
	}

	/*
	 * Check again since another task may have expanded the fd table while
	 * we dropped the lock
	 */
	/*
	 * 再次检查，因为当我们释放锁时，可能有其他任务已经扩展了 fd 表
	 */
	cur_fdt = files_fdtable(files); // 获取当前的文件描述符表
	if (nr >= cur_fdt->max_fds) {
		/* Continue as planned */
		/* 按计划继续 */
		copy_fdtable(new_fdt, cur_fdt);     // 复制旧表到新表
		rcu_assign_pointer(files->fdt, new_fdt);  // 原子更新文件描述符表指针
		if (cur_fdt->max_fds > NR_OPEN_DEFAULT)
			free_fdtable(cur_fdt); // 释放旧的 fdtable，如果其大小超过默认值
	} else {
		/* Somebody else expanded, so undo our attempt */
		/* 如果其他人已扩展，则撤销我们的尝试 */
		free_fdarr(new_fdt);  // 释放文件指针数组
		free_fdset(new_fdt);  // 释放文件描述符集
		kfree(new_fdt);       // 释放 fdtable 结构本身
	}
	return 1;  // 成功完成，返回 1
}

/*
 * Expand files.
 * This function will expand the file structures, if the requested size exceeds
 * the current capacity and there is room for expansion.
 * Return <0 error code on error; 0 when nothing done; 1 when files were
 * expanded and execution may have blocked.
 * The files->file_lock should be held on entry, and will be held on exit.
 */
/*
 * 扩展文件结构。
 * 如果请求的大小超过当前容量且有扩展的空间，此函数将扩展文件结构。
 * 错误时返回小于0的错误代码；什么都没做时返回0；文件扩展且可能阻塞执行时返回1。
 * 调用时应持有 files->file_lock，返回时依然持有。
 */
int expand_files(struct files_struct *files, int nr)
{
	struct fdtable *fdt;

	fdt = files_fdtable(files);	// 获取当前文件结构的文件描述符表

	/*
	 * N.B. For clone tasks sharing a files structure, this test
	 * will limit the total number of files that can be opened.
	 */
	/*
   * 注意：对于共享文件结构的克隆任务，此测试将限制可以打开的文件总数。
   */
	if (nr >= rlimit(RLIMIT_NOFILE))
		return -EMFILE;	// 如果请求的文件数超过了资源限制，返回文件过多错误

	/* Do we need to expand? */
	/* 我们需要扩展吗？ */
	if (nr < fdt->max_fds)
		return 0;  // 如果当前的最大文件描述符数已经满足需求，不需要扩展，返回0

	/* Can we expand? */
	/* 我们可以扩展吗？ */
	if (nr >= sysctl_nr_open)
		return -EMFILE;  // 如果请求的文件数超过了系统设置的最大打开文件数，返回文件过多错误

	/* All good, so we try */
	/* 一切正常，我们尝试扩展 */
	return expand_fdtable(files, nr);  // 调用 expand_fdtable 函数尝试扩展文件描述符表，并返回其结果
}

// 定义一个函数，用于计算 fdtable 中打开的文件数量
static int count_open_files(struct fdtable *fdt)
{
	int size = fdt->max_fds;  // 获取 fdtable 的最大文件描述符数
	int i;

	/* Find the last open fd */
	/* 查找最后一个打开的文件描述符 */
	for (i = size/(8*sizeof(long)); i > 0; ) {
		// 检查每个 long 单位的位，查找至少有一个位被设置的情况
		if (fdt->open_fds->fds_bits[--i])
			break;  // 如果找到一个非零的位组，停止查找
	}
	// 将位置转换为文件描述符索引
	i = (i+1) * 8 * sizeof(long);  // 计算实际的文件描述符索引
	return i;  // 返回打开文件的数量（实际上是最大的文件描述符索引）
}

/*
 * Allocate a new files structure and copy contents from the
 * passed in files structure.
 * errorp will be valid only when the returned files_struct is NULL.
 */
/*
 * 分配一个新的 files 结构并从传入的 files 结构复制内容。
 * 当返回的 files_struct 为 NULL 时，errorp 将是有效的。
 */
// 定义一个函数，用于复制一个 files_struct 结构。
struct files_struct *dup_fd(struct files_struct *oldf, int *errorp)
{
	struct files_struct *newf;  // 指向新的 files_struct 的指针
	struct file **old_fds, **new_fds;  // 旧的和新的文件描述符数组指针
	int open_files, size, i;  // 打开的文件数量、数组大小、迭代变量
	struct fdtable *old_fdt, *new_fdt;  // 旧的和新的文件描述符表指针

	*errorp = -ENOMEM;  // 默认错误设置为内存不足
	newf = kmem_cache_alloc(files_cachep, GFP_KERNEL);  // 从内存缓存分配一个新的 files_struct
	if (!newf)
		goto out;  // 如果分配失败，则跳到错误处理部分

	atomic_set(&newf->count, 1);  // 初始化新 files_struct 的引用计数为 1

	spin_lock_init(&newf->file_lock);  // 初始化新 files_struct 的锁
	newf->next_fd = 0;  // 初始化下一个可用的文件描述符为 0
	new_fdt = &newf->fdtab;  // 设置指向新文件描述符表的指针
	new_fdt->max_fds = NR_OPEN_DEFAULT;  // 设置默认的文件描述符上限
	new_fdt->close_on_exec = (fd_set *)&newf->close_on_exec_init;  // 初始化执行关闭的文件描述符集
	new_fdt->open_fds = (fd_set *)&newf->open_fds_init;  // 初始化打开的文件描述符集
	new_fdt->fd = &newf->fd_array[0];  // 设置文件描述符数组的起始指针
	INIT_RCU_HEAD(&new_fdt->rcu);  // 初始化 RCU 头部
	new_fdt->next = NULL;  // 设置链表的下一个元素为 NULL

	spin_lock(&oldf->file_lock);  // 加锁旧的 files_struct
	old_fdt = files_fdtable(oldf);  // 获取旧的文件描述符表
	open_files = count_open_files(old_fdt);  // 计算旧的文件描述符表中打开的文件数量

	/*
	 * Check whether we need to allocate a larger fd array and fd set.
	 */
	/* 检查是否需要分配一个更大的文件描述符数组和集合 */
	// 如果当前打开的文件数量超过了新文件描述符表的最大容量，则进入循环
	while (unlikely(open_files > new_fdt->max_fds)) {
		// 首先解锁旧文件结构的锁，以避免在重新分配时持有锁
		spin_unlock(&oldf->file_lock);  // 如果需要更大的空间，先释放旧锁

		if (new_fdt != &newf->fdtab) {	// 如果新的文件描述符表不是初始分配的表
			free_fdarr(new_fdt);  // 释放新的文件描述符数组
			free_fdset(new_fdt);  // 释放新的文件描述符集
			kfree(new_fdt);  // 释放文件描述符表结构本身
		}

		new_fdt = alloc_fdtable(open_files - 1);  // 分配一个更大的文件描述符表
		if (!new_fdt) {
			*errorp = -ENOMEM;  // 如果分配失败，设置错误码
			goto out_release;  // 跳到资源释放部分
		}

		/* beyond sysctl_nr_open; nothing to do */
		/* 如果超出系统允许的文件打开数，就没有操作可做 */
		// 如果新分配的文件描述符表的最大文件描述符数仍然小于当前打开文件的数量
		if (unlikely(new_fdt->max_fds < open_files)) {
			free_fdarr(new_fdt);      // 释放文件描述符数组
			free_fdset(new_fdt);      // 释放文件描述符集
			kfree(new_fdt);           // 释放文件描述符表结构本身
			*errorp = -EMFILE;        // 设置错误码为打开的文件过多
			goto out_release;         // 跳到资源释放段
		}

		/*
		 * Reacquire the oldf lock and a pointer to its fd table
		 * who knows it may have a new bigger fd table. We need
		 * the latest pointer.
		 */
		/* 重新获取旧锁和旧的文件描述符表指针，因为可能在这期间有变化 */
		spin_lock(&oldf->file_lock);	// 重新加锁旧文件结构
		old_fdt = files_fdtable(oldf);	// 更新旧文件描述符表的指针
		open_files = count_open_files(old_fdt);	// 重新计算打开的文件数量
	}

	old_fds = old_fdt->fd;	// 获取旧的文件描述符数组指针
	new_fds = new_fdt->fd;	// 获取新的文件描述符数组指针

	/* 复制打开和执行关闭的文件描述符位图 */
	// 复制已打开文件描述符的位图
	memcpy(new_fdt->open_fds->fds_bits,
		old_fdt->open_fds->fds_bits, open_files/8);
	// 复制执行关闭文件描述符的位图
	memcpy(new_fdt->close_on_exec->fds_bits,
		old_fdt->close_on_exec->fds_bits, open_files/8);

	/* 复制文件指针，并为每个打开的文件增加引用计数 */
	// 遍历每个打开的文件描述符
	for (i = open_files; i != 0; i--) {
		struct file *f = *old_fds++;	// 获取当前文件描述符指向的文件对象
		if (f) {
			get_file(f);	// 如果文件对象存在，增加其引用计数
		} else {
			/*
			 * The fd may be claimed in the fd bitmap but not yet
			 * instantiated in the files array if a sibling thread
			 * is partway through open().  So make sure that this
			 * fd is available to the new process.
			 */
			/*
			 * 文件描述符可能在位图中被标记为已分配，但在文件数组中尚未实例化，
			 * 如果一个兄弟线程正在执行 open() 操作。因此，确保这个文件描述符在新进程中可用。
			 */
			/* 如果文件描述符已标记为打开但尚未实例化，则清除对应位 */
			FD_CLR(open_files - i, new_fdt->open_fds);	// 清除位图中相应的位
		}
		rcu_assign_pointer(*new_fds++, f);	// 为新文件描述符表赋值，并更新指针
	}
	spin_unlock(&oldf->file_lock);	 // 解锁旧文件结构的自旋锁

	/* compute the remainder to be cleared */
	/* 计算需要清除的剩余部分 */
	/* 清除剩余的文件描述符空间 */
	size = (new_fdt->max_fds - open_files) * sizeof(struct file *);

	/* This is long word aligned thus could use a optimized version */
	/* 由于对齐到长字，可以使用优化的版本进行清零 */
	memset(new_fds, 0, size);	// 清零未使用的文件描述符指针部分

	/* 清除剩余的位图空间 */
	if (new_fdt->max_fds > open_files) {
		int left = (new_fdt->max_fds-open_files)/8;	// 计算剩余未设置的位图部分
		int start = open_files / (8 * sizeof(unsigned long));	// 计算起始位置

		// 清零剩余的打开文件描述符和执行关闭文件描述符位图部分
		memset(&new_fdt->open_fds->fds_bits[start], 0, left);
		memset(&new_fdt->close_on_exec->fds_bits[start], 0, left);
	}

	rcu_assign_pointer(newf->fdt, new_fdt);	// 更新新的文件描述符表指针

	return newf;	// 返回新的 files_struct

out_release:
	kmem_cache_free(files_cachep, newf);	// 释放新的 files_struct
out:
	return NULL;	// 返回 NULL，表示分配失败
}

// 定义一个静态函数，用于初始化每个 CPU 的文件描述符表延迟释放列表
static void __devinit fdtable_defer_list_init(int cpu)
{
	struct fdtable_defer *fddef = &per_cpu(fdtable_defer_list, cpu);  // 获取每个 CPU 的文件描述符表延迟释放列表
	spin_lock_init(&fddef->lock);  // 初始化自旋锁
	INIT_WORK(&fddef->wq, free_fdtable_work);  // 初始化工作队列条目，指定释放工作的函数
	fddef->next = NULL;  // 初始化下一个元素指针为 NULL
}

// 定义一个初始化函数，用于在系统启动时初始化所有可能的 CPU 的文件描述符表延迟释放列表
void __init files_defer_init(void)
{
	int i;
	// 遍历所有可能的 CPU
	for_each_possible_cpu(i)
		fdtable_defer_list_init(i);  // 为每个 CPU 初始化文件描述符表延迟释放列表
	// 设置系统最大打开文件描述符数，考虑到 int 类型的最大值和指针大小
	sysctl_nr_open_max = min((size_t)INT_MAX, ~(size_t)0/sizeof(void *)) &
			     -BITS_PER_LONG;
}

// 初始化一个全局的 files_struct 结构，用于系统启动和初始化
struct files_struct init_files = {
	.count		= ATOMIC_INIT(1),  // 引用计数初始化为 1
	.fdt		= &init_files.fdtab,  // 指向文件描述符表的指针
	.fdtab		= {
		.max_fds	= NR_OPEN_DEFAULT,  // 默认最大文件描述符数
		.fd		= &init_files.fd_array[0],  // 文件描述符数组的起始指针
		.close_on_exec	= (fd_set *)&init_files.close_on_exec_init,  // 执行关闭位图的初始化
		.open_fds	= (fd_set *)&init_files.open_fds_init,  // 打开文件描述符位图的初始化
		.rcu		= RCU_HEAD_INIT,  // RCU 头部初始化
	},
	.file_lock	= __SPIN_LOCK_UNLOCKED(init_task.file_lock),  // 初始化文件锁，未锁定状态
};

/*
 * allocate a file descriptor, mark it busy.
 */
/*
 * 分配一个文件描述符，标记为忙。
 */
int alloc_fd(unsigned start, unsigned flags)
{
	struct files_struct *files = current->files;  // 获取当前进程的文件结构
	unsigned int fd;  // 用于存储分配的文件描述符
	int error;  // 用于存储错误码
	struct fdtable *fdt;  // 文件描述符表的指针

	spin_lock(&files->file_lock);  // 锁定文件结构以保证线程安全
repeat:
	fdt = files_fdtable(files);  // 获取当前文件描述符表
	fd = start;  // 设置起始搜索点
	if (fd < files->next_fd)  // 如果起始点小于下一个可用文件描述符
		fd = files->next_fd;  // 更新 fd 为下一个可用文件描述符

	if (fd < fdt->max_fds)  // 如果 fd 在文件描述符表的最大值之内
		fd = find_next_zero_bit(fdt->open_fds->fds_bits,
					   fdt->max_fds, fd);  // 查找下一个未使用的位

	error = expand_files(files, fd);  // 尝试扩展文件描述符表以包含 fd
	if (error < 0)  // 如果扩展失败
		goto out;  // 跳转到出错处理

	/*
	 * If we needed to expand the fs array we
	 * might have blocked - try again.
	 */
	/*
	 * 如果我们需要扩展文件数组，可能会阻塞 - 重试。
	 */
	if (error)
		goto repeat;  // 如果扩展成功但有其他改动，重试查找和设置流程

	if (start <= files->next_fd)  // 如果分配的起始点小于等于下一个可用文件描述符
		files->next_fd = fd + 1;  // 更新下一个可用文件描述符

	FD_SET(fd, fdt->open_fds);  // 在打开文件描述符集中设置 fd 位
	if (flags & O_CLOEXEC)  // 如果标志位中包含执行时关闭
		FD_SET(fd, fdt->close_on_exec);  // 在执行关闭集中设置 fd 位
	else
		FD_CLR(fd, fdt->close_on_exec);  // 否则清除执行关闭集中的 fd 位
	error = fd;  // 设置返回值为分配的文件描述符

#if 1
	/* Sanity check */
	/* 健全性检查 */
	if (rcu_dereference_raw(fdt->fd[fd]) != NULL) {  // 如果文件描述符已经被占用
		printk(KERN_WARNING "alloc_fd: slot %d not NULL!\n", fd);  // 打印警告信息
		rcu_assign_pointer(fdt->fd[fd], NULL);  // 清除文件描述符指针，避免野指针
	}
#endif

out:
	spin_unlock(&files->file_lock);  // 解锁文件结构
	return error;  // 返回错误码或文件描述符
}

// 定义一个函数，用于获取一个未使用的文件描述符。
int get_unused_fd(void)
{
	return alloc_fd(0, 0);  // 调用 alloc_fd 函数从文件描述符 0 开始寻找未使用的文件描述符，没有额外的标志位
}
EXPORT_SYMBOL(get_unused_fd);
