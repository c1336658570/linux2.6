/*
 * linux/fs/inode.c
 *
 * (C) 1997 Linus Torvalds
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/dcache.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/writeback.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/wait.h>
#include <linux/rwsem.h>
#include <linux/hash.h>
#include <linux/swap.h>
#include <linux/security.h>
#include <linux/pagemap.h>
#include <linux/cdev.h>
#include <linux/bootmem.h>
#include <linux/inotify.h>
#include <linux/fsnotify.h>
#include <linux/mount.h>
#include <linux/async.h>
#include <linux/posix_acl.h>

/*
 * This is needed for the following functions:
 *  - inode_has_buffers
 *  - invalidate_inode_buffers
 *  - invalidate_bdev
 *
 * FIXME: remove all knowledge of the buffer layer from this file
 */
/*
 * 这是为了下面几个函数需要：
 *  - inode_has_buffers
 *  - invalidate_inode_buffers
 *  - invalidate_bdev
 *
 * 待修复：从此文件中移除所有缓冲层的相关知识
 */
#include <linux/buffer_head.h>

/*
 * New inode.c implementation.
 *
 * This implementation has the basic premise of trying
 * to be extremely low-overhead and SMP-safe, yet be
 * simple enough to be "obviously correct".
 *
 * Famous last words.
 */
/*
 * 新的inode.c实现。
 *
 * 这个实现的基本前提是试图极低的开销和对称多处理安全，同时又简单到足以“显然正确”。
 *
 * 著名的遗言。
 */

/* inode dynamic allocation 1999, Andrea Arcangeli <andrea@suse.de> */
/* inode动态分配1999, Andrea Arcangeli <andrea@suse.de> */

/* #define INODE_PARANOIA 1 */
/* #define INODE_DEBUG 1 */
/* #define INODE_PARANOIA 1 */
/* #define INODE_DEBUG 1 */

/*
 * Inode lookup is no longer as critical as it used to be:
 * most of the lookups are going to be through the dcache.
 */
/*
 * Inode查找不再像过去那样关键：
 * 大部分查找操作将通过目录缓存完成。
 */
#define I_HASHBITS   i_hash_shift  // inode哈希表的位移量，用于计算哈希桶位置
#define I_HASHMASK   i_hash_mask   // inode哈希表的掩码，用于确保哈希值在有效范围内

static unsigned int i_hash_mask __read_mostly;  // 用于inode哈希计算的掩码，标记为主要用于读取操作
static unsigned int i_hash_shift __read_mostly; // 用于计算inode哈希值的位移值，同样标记为主要用于读取操作

/*
 * Each inode can be on two separate lists. One is
 * the hash list of the inode, used for lookups. The
 * other linked list is the "type" list:
 *  "in_use" - valid inode, i_count > 0, i_nlink > 0
 *  "dirty"  - as "in_use" but also dirty
 *  "unused" - valid inode, i_count = 0
 *
 * A "dirty" list is maintained for each super block,
 * allowing for low-overhead inode sync() operations.
 */
/* 
 * 每个inode可以位于两个独立的列表上。一个是用于查找的inode的哈希列表。
 * 另一个是链接列表，称为“类型”列表：
 *  “in_use” - 有效的inode，i_count > 0, i_nlink > 0
 *  “dirty” - 与“in_use”相同，但同时标记为脏
 *  “unused” - 有效的inode，i_count = 0
 *
 * 每个超级块维护一个“dirty”列表，
 * 允许进行低开销的inode sync()操作。
 */

// 当前正在使用的inode链表，i_count > 0且 i_nlink > 0
LIST_HEAD(inode_in_use);
// 目前未被使用的inode节点链表，即尚在内存中没有销毁，但是没有进程使用，i_count为0。
LIST_HEAD(inode_unused);
// 静态定义一个指针数组 inode_hashtable，数组的每个元素都是一个哈希表头结构，
// __read_mostly 告诉编译器这个变量主要用于读取，很少修改，有助于编译器优化。
// 这个哈希表用于存储正在使用和被标记为脏的inode，
// 不同的inode可能有相同的哈希值，具有相同哈希值的inode通过其i_hash成员连接成一个链表。
// inode_hashtable 是一个结合了数组和链表的结构，每个数组元素是一个链表的头部，
// 在这些链表中存放的是具有相同哈希值的inode。
static struct hlist_head *inode_hashtable __read_mostly;

/*
 * A simple spinlock to protect the list manipulations.
 *
 * NOTE! You also have to own the lock if you change
 * the i_state of an inode while it is in use..
 */
/*
 * 一个简单的自旋锁，用于保护列表操作。
 *
 * 注意！如果你在使用中更改 inode 的 i_state，你也必须持有这个锁。
 */
DEFINE_SPINLOCK(inode_lock);  // 定义一个名为 inode_lock 的自旋锁，用于同步对 inode 列表的操作

/*
 * iprune_sem provides exclusion between the kswapd or try_to_free_pages
 * icache shrinking path, and the umount path.  Without this exclusion,
 * by the time prune_icache calls iput for the inode whose pages it has
 * been invalidating, or by the time it calls clear_inode & destroy_inode
 * from its final dispose_list, the struct super_block they refer to
 * (for inode->i_sb->s_op) may already have been freed and reused.
 *
 * We make this an rwsem because the fastpath is icache shrinking. In
 * some cases a filesystem may be doing a significant amount of work in
 * its inode reclaim code, so this should improve parallelism.
 */
/*
 * iprune_sem 提供了在 kswapd 或 try_to_free_pages
 * 的 icache 缩减路径和卸载（umount）路径之间的互斥。
 * 没有这种互斥的话，在 prune_icache 调用 iput 释放
 * inode 页面时，或者在它从最终的 dispose_list 调用
 * clear_inode & destroy_inode 时，它们引用的
 * struct super_block（对应 inode->i_sb->s_op）可能已经被释放并重用。
 *
 * 我们使用读写信号量（rwsem），因为快速路径是 icache 缩减。
 * 在某些情况下，文件系统在其 inode 回收代码中可能会执行大量工作，
 * 所以这应该会提高并行性。
 */
static DECLARE_RWSEM(iprune_sem);  // 静态声明一个名为 iprune_sem 的读写信号量，用于在 inode 缓存收缩和卸载操作之间提供互斥

/*
 * Statistics gathering..
 */
/*
 * 用于收集统计信息。
 */
struct inodes_stat_t inodes_stat;  // 定义一个结构体变量 inodes_stat，用于存储 inode 的统计数据

// 定义了一个用于 inode 的内存缓存。这种缓存是专门优化的内存区域，用来快速分配和释放 inode 结构体，提升文件系统的性能。
static struct kmem_cache *inode_cachep __read_mostly;  // 静态定义一个指向 kmem_cache 结构的指针 inode_cachep，
                                                       // __read_mostly 表示这个变量主要被读取，很少被修改，有助于优化访问性能

static void wake_up_inode(struct inode *inode)
{
	/*
	 * Prevent speculative execution through spin_unlock(&inode_lock);
	 */
	/*
	 * 阻止通过 spin_unlock(&inode_lock) 的投机性执行。
	 */
	smp_mb();  // 在多核处理器系统中插入一个内存屏障，确保之前的所有操作完成后才继续执行后续操作
	wake_up_bit(&inode->i_state, __I_NEW);  // 唤醒等待 inode 状态位 __I_NEW 改变的所有进程
}

/**
 * inode_init_always - perform inode structure intialisation
 * @sb: superblock inode belongs to
 * @inode: inode to initialise
 *
 * These are initializations that need to be done on every inode
 * allocation as the fields are not initialised by slab allocation.
 */
/**
 * inode_init_always - 执行 inode 结构初始化
 * @sb: inode 所属的超级块
 * @inode: 需要初始化的 inode
 *
 * 需要在每次 inode 分配时进行的初始化，
 * 因为这些字段不会通过 slab 分配进行初始化。
 */
int inode_init_always(struct super_block *sb, struct inode *inode)
{
	// 定义一组空的操作集，用于 inode 的默认操作。
	static const struct address_space_operations empty_aops;
	static const struct inode_operations empty_iops;
	static const struct file_operations empty_fops;
	// 获取 inode 中的地址空间映射的引用。
	struct address_space *const mapping = &inode->i_data;

	// 初始化 inode 的基本属性。
	inode->i_sb = sb;  // 设置 inode 的超级块指针
	inode->i_blkbits = sb->s_blocksize_bits;  // 设置 inode 的块大小位数
	inode->i_flags = 0;  // 初始化 inode 标志为 0
	atomic_set(&inode->i_count, 1);  // 设置 inode 的引用计数为 1
	inode->i_op = &empty_iops;  // 设置 inode 操作为默认空操作
	inode->i_fop = &empty_fops;  // 设置文件操作为默认空操作
	inode->i_nlink = 1;  // 设置链接计数为 1
	inode->i_uid = 0;  // 用户 ID 设为 0
	inode->i_gid = 0;  // 组 ID 设为 0
	atomic_set(&inode->i_writecount, 0);  // 设置写入计数为 0
	inode->i_size = 0;  // 文件大小设置为 0
	inode->i_blocks = 0;  // 块数设置为 0
	inode->i_bytes = 0;  // 以字节计的块数设置为 0
	inode->i_generation = 0;  // 生成号设置为 0
#ifdef CONFIG_QUOTA
	memset(&inode->i_dquot, 0, sizeof(inode->i_dquot));  // 如果启用了配额，将 inode 配额信息清零
#endif
	inode->i_pipe = NULL;  // 管道指针设为空
	inode->i_bdev = NULL;  // 块设备指针设为空
	inode->i_cdev = NULL;  // 字符设备指针设为空
	inode->i_rdev = 0;  // 设备 ID 设为 0
	inode->dirtied_when = 0;  // 最后一次脏时间戳设置为 0

	// 安全模块初始化，如果初始化失败则返回内存错误
	if (security_inode_alloc(inode))
		goto out;
	spin_lock_init(&inode->i_lock);  // 初始化 inode 的自旋锁
	lockdep_set_class(&inode->i_lock, &sb->s_type->i_lock_key);  // 设置锁依赖类别

	mutex_init(&inode->i_mutex);  // 初始化 inode 的互斥锁
	lockdep_set_class(&inode->i_mutex, &sb->s_type->i_mutex_key);  // 设置互斥锁的锁依赖类别

	init_rwsem(&inode->i_alloc_sem);  // 初始化读写信号量
	lockdep_set_class(&inode->i_alloc_sem, &sb->s_type->i_alloc_sem_key);  // 设置读写信号量的锁依赖类别

	// 地址空间操作设置
	mapping->a_ops = &empty_aops;  // 设置地址空间操作为默认空操作
	mapping->host = inode;  // 设置地址空间的宿主为当前 inode
	mapping->flags = 0;  // 地址空间标志设置为 0
	// 设置地址空间的内存分配标志为高优先级的用户可移动类型，以优化内存管理。
	mapping_set_gfp_mask(mapping, GFP_HIGHUSER_MOVABLE);  // 设置地址空间的 GFP 掩码
	mapping->assoc_mapping = NULL;  // 关联的地址空间设置为空
	mapping->backing_dev_info = &default_backing_dev_info;  // 设置默认的后备设备信息
	mapping->writeback_index = 0;  // 设置回写索引为 0

	/*
	 * If the block_device provides a backing_dev_info for client
	 * inodes then use that.  Otherwise the inode share the bdev's
	 * backing_dev_info.
	 */
	/*
	 * 如果块设备提供了用于客户端 inode 的后备设备信息，就使用它。
	 * 否则，inode 将共享块设备的后备设备信息。
	 */
	if (sb->s_bdev) {
		struct backing_dev_info *bdi;

		// 从块设备获取后备设备信息，并将其设置为当前 inode 的映射的后备设备信息。
		bdi = sb->s_bdev->bd_inode->i_mapping->backing_dev_info;
		mapping->backing_dev_info = bdi;
	}
	inode->i_private = NULL;  // 私有数据设置为空
	inode->i_mapping = mapping;  // 设置 inode 的地址空间映射
#ifdef CONFIG_FS_POSIX_ACL
	// 如果定义了 POSIX ACL 支持，则初始化 inode 的访问控制列表状态为未缓存。
	inode->i_acl = inode->i_default_acl = ACL_NOT_CACHED;  // 如果启用 POSIX ACL，设置默认 ACL 状态
#endif

#ifdef CONFIG_FSNOTIFY
	inode->i_fsnotify_mask = 0;  // 如果启用文件系统通知，初始化通知掩码为 0
#endif

	return 0;  // 成功返回 0
out:
	return -ENOMEM;  // 初始化失败，返回内存错误代码
}
EXPORT_SYMBOL(inode_init_always);

/**
 * alloc_inode - 分配一个新的 inode
 * @sb: 指向超级块的指针，inode 将属于此超级块
 *
 * 这个函数首先尝试使用超级块定义的特定 inode 分配方法（如果有的话），
 * 否则它会从通用的 inode 缓存中分配一个 inode。
 */
static struct inode *alloc_inode(struct super_block *sb)
{
	struct inode *inode;

	// 如果超级块有自定义的 inode 分配函数，则使用之。
	if (sb->s_op->alloc_inode)
		inode = sb->s_op->alloc_inode(sb);
	else
		// 否则从 inode 缓存中分配一个 inode。
		inode = kmem_cache_alloc(inode_cachep, GFP_KERNEL);

	// 如果分配失败，则返回 NULL。
	if (!inode)
		return NULL;

	// 初始化 inode，如果初始化失败：
	if (unlikely(inode_init_always(sb, inode))) {
		// 如果超级块有自定义的 inode 销毁函数，则使用之销毁 inode。
		if (inode->i_sb->s_op->destroy_inode)
			inode->i_sb->s_op->destroy_inode(inode);
		else
			// 否则从 inode 缓存中释放这个 inode。
			kmem_cache_free(inode_cachep, inode);
		return NULL;
	}

	// 返回新分配并初始化的 inode。
	return inode;
}

/**
 * __destroy_inode - 销毁 inode 的内部函数
 * @inode: 要销毁的 inode 结构指针
 *
 * 这个函数负责执行 inode 销毁的通用步骤，包括安全释放和通知相关子系统。
 */
void __destroy_inode(struct inode *inode)
{
	// 检查 inode 是否还有缓存的 buffer，如果有则触发 bug。
	BUG_ON(inode_has_buffers(inode));
	// 调用安全模块的 inode 释放函数。
	security_inode_free(inode);
	// 发送文件系统通知，表明 inode 被删除。
	fsnotify_inode_delete(inode);
#ifdef CONFIG_FS_POSIX_ACL
	// 如果定义了 POSIX ACL 支持，并且 inode 有 ACL 数据，则释放它。
	if (inode->i_acl && inode->i_acl != ACL_NOT_CACHED)
		posix_acl_release(inode->i_acl);
	if (inode->i_default_acl && inode->i_default_acl != ACL_NOT_CACHED)
		posix_acl_release(inode->i_default_acl);
#endif
}
EXPORT_SYMBOL(__destroy_inode);

/**
 * destroy_inode - 公共接口，用于销毁 inode
 * @inode: 要销毁的 inode 结构指针
 *
 * 根据超级块的操作决定使用自定义销毁方法或者通用方法从缓存中释放 inode。
 */
void destroy_inode(struct inode *inode)
{
	// 首先调用内部销毁函数处理通用销毁步骤。
	__destroy_inode(inode);
	// 根据超级块提供的方法决定如何最终销毁 inode。
	if (inode->i_sb->s_op->destroy_inode)
		// 如果定义了自定义的 destroy_inode 方法，调用它。
		inode->i_sb->s_op->destroy_inode(inode);
	else
		// 否则，从 inode 缓存中释放 inode。
		kmem_cache_free(inode_cachep, (inode));
}

/*
 * These are initializations that only need to be done
 * once, because the fields are idempotent across use
 * of the inode, so let the slab aware of that.
 */
/**
 * inode_init_once - 只需要初始化一次的 inode 设置
 * @inode: 要初始化的 inode 结构指针
 *
 * 这些初始化只需完成一次，因为这些字段在 inode 的使用期间是幂等的，
 * 让 slab 系统知晓这一点。
 */
void inode_init_once(struct inode *inode)
{
	// 将整个 inode 结构体的内存清零。
	memset(inode, 0, sizeof(*inode));
	// 初始化 inode 的哈希链表节点。
	INIT_HLIST_NODE(&inode->i_hash);
	// 初始化 inode 的 dentry 链表。
	INIT_LIST_HEAD(&inode->i_dentry);
	// 初始化 inode 设备链表。
	INIT_LIST_HEAD(&inode->i_devices);
	// 初始化页树，用于管理与 inode 关联的数据页。
	INIT_RADIX_TREE(&inode->i_data.page_tree, GFP_ATOMIC);
	// 初始化用于页树的自旋锁。
	spin_lock_init(&inode->i_data.tree_lock);
	// 初始化用于内存映射区的自旋锁。
	spin_lock_init(&inode->i_data.i_mmap_lock);
	// 初始化 inode 的私有数据链表。
	INIT_LIST_HEAD(&inode->i_data.private_list);
	// 初始化管理私有数据链表的自旋锁。
	spin_lock_init(&inode->i_data.private_lock);
	// 初始化原始优先级树根，用于管理虚拟内存区。
	INIT_RAW_PRIO_TREE_ROOT(&inode->i_data.i_mmap);
	// 初始化非线性内存映射链表。
	INIT_LIST_HEAD(&inode->i_data.i_mmap_nonlinear);
	// 初始化有序大小扩展列表。
	i_size_ordered_init(inode);
#ifdef CONFIG_INOTIFY
	// 如果启用了 INOTIFY，初始化通知监视链表和互斥锁。
	INIT_LIST_HEAD(&inode->inotify_watches);
	mutex_init(&inode->inotify_mutex);
#endif
#ifdef CONFIG_FSNOTIFY
	// 如果启用了 FSNOTIFY，初始化文件系统通知标记的头节点。
	INIT_HLIST_HEAD(&inode->i_fsnotify_mark_entries);
#endif
}
EXPORT_SYMBOL(inode_init_once);

/**
 * init_once - 对传入的 inode 执行一次性初始化
 * @foo: 指向 inode 的指针，类型为 void，需要在函数内部转换为正确类型
 *
 * 此函数是一个通用的初始化回调，用于 slab 分配器在首次创建对象时调用。
 */
static void init_once(void *foo)
{
	struct inode *inode = (struct inode *) foo;  // 将 void 类型指针转换为 struct inode 类型

	inode_init_once(inode);  // 调用 inode_init_once 函数来进行 inode 的一次性初始化
}

/**
 * __iget - 增加 inode 的引用计数
 * @inode: 要操作的 inode 结构指针
 *
 * inode_lock 必须被持有（即调用此函数前必须已经获取 inode_lock）
 *
 * 此函数用于在 inode 被再次使用时安全地增加其引用计数。
 */
/*
 * inode_lock must be held
 */
void __iget(struct inode *inode)
{
	if (atomic_read(&inode->i_count)) {  // 读取 inode 的引用计数
		atomic_inc(&inode->i_count);  // 如果引用计数非零，直接增加
		return;  // 并提前返回
	}
	// 如果引用计数为零，说明此 inode 正在首次使用，增加引用计数
	atomic_inc(&inode->i_count);

	// 检查 inode 状态，如果 inode 没有标记为脏或同步中，则将其移动到使用中列表
	if (!(inode->i_state & (I_DIRTY | I_SYNC)))
		list_move(&inode->i_list, &inode_in_use);

	inodes_stat.nr_unused--;  // 减少未使用的 inode 计数
}

/**
 * clear_inode - clear an inode
 * @inode: inode to clear
 *
 * This is called by the filesystem to tell us
 * that the inode is no longer useful. We just
 * terminate it with extreme prejudice.
 */
/**
 * clear_inode - 清理一个 inode
 * @inode: 要清理的 inode
 *
 * 这个函数由文件系统调用，用来通知内核这个 inode 不再被需要了。
 * 我们会彻底终结这个 inode。
 */
void clear_inode(struct inode *inode)
{
	// 检查是否在可能会睡眠的上下文中调用此函数
	might_sleep();
	// 使 inode 相关联的所有缓冲区失效
	invalidate_inode_buffers(inode);

	// 检查 inode 是否仍有页缓存，如果有，则触发 bug
	BUG_ON(inode->i_data.nrpages);
	// 确保 inode 正在被释放，否则触发 bug
	BUG_ON(!(inode->i_state & I_FREEING));
	// 确保 inode 状态不是已清理，否则触发 bug
	BUG_ON(inode->i_state & I_CLEAR);
	// 等待与 inode 相关的所有 I/O 操作完成
	inode_sync_wait(inode);
	// 如果超级块有自定义的清理 inode 函数，调用之
	if (inode->i_sb->s_op->clear_inode)
		inode->i_sb->s_op->clear_inode(inode);
	// 如果 inode 是块设备，并且关联了块设备结构，执行忘记块设备操作
	if (S_ISBLK(inode->i_mode) && inode->i_bdev)
		bd_forget(inode);
	// 如果 inode 是字符设备，并且关联了字符设备结构，执行忘记字符设备操作
	if (S_ISCHR(inode->i_mode) && inode->i_cdev)
		cd_forget(inode);
	// 设置 inode 的状态为已清理
	inode->i_state = I_CLEAR;
}
EXPORT_SYMBOL(clear_inode);

/*
 * dispose_list - dispose of the contents of a local list
 * @head: the head of the list to free
 *
 * Dispose-list gets a local list with local inodes in it, so it doesn't
 * need to worry about list corruption and SMP locks.
 */
/**
 * dispose_list - 处理并释放一个本地列表中的内容
 * @head: 列表头，指向要释放的列表
 *
 * dispose_list 获取一个包含本地 inode 的本地列表，因此它不需要担心列表损坏和 SMP 锁的问题。
 */
static void dispose_list(struct list_head *head)
{
	int nr_disposed = 0;  // 计数器，记录处理（释放）的 inode 数量

	// 当列表不为空时，循环处理列表中的每一个 inode
	while (!list_empty(head)) {
		struct inode *inode;

		// 从列表头部获取第一个 inode
		inode = list_first_entry(head, struct inode, i_list);
		// 从列表中删除这个 inode 的节点
		list_del(&inode->i_list);

		// 如果 inode 有关联的页，截断这些页
		if (inode->i_data.nrpages)
			truncate_inode_pages(&inode->i_data, 0);
		// 清理 inode，进行必要的收尾工作
		clear_inode(inode);

		// 锁定全局 inode 锁，进行全局性的删除操作
		spin_lock(&inode_lock);
		// 从哈希表中删除 inode
		hlist_del_init(&inode->i_hash);
		// 从 inode 所属的超级块的 inode 列表中删除
		list_del_init(&inode->i_sb_list);
		// 释放锁
		spin_unlock(&inode_lock);

		// 唤醒等待这个 inode 的进程
		wake_up_inode(inode);
		// 销毁 inode，释放内存
		destroy_inode(inode);
		// 已处理的 inode 数量增加
		nr_disposed++;
	}
	// 再次锁定全局 inode 锁，更新 inode 统计数据
	spin_lock(&inode_lock);
	// 减少全局统计中的 inode 数量
	inodes_stat.nr_inodes -= nr_disposed;
	// 释放锁
	spin_unlock(&inode_lock);
}

/*
 * Invalidate all inodes for a device.
 */
/**
 * invalidate_list - 使一个设备上的所有 inode 无效
 * @head: 指向 inode 列表的头部
 * @dispose: 用于临时存储即将被释放的 inode 的列表
 *
 * 此函数使设备上的所有 inode 无效，通常在卸载文件系统时使用。
 */
static int invalidate_list(struct list_head *head, struct list_head *dispose)
{
	struct list_head *next;
	int busy = 0, count = 0;  // busy 标志任何 inode 是否正在使用，count 用于计数被处理的 inode 数

	next = head->next;  // 从列表的第一个元素开始
	for (;;) {  // 无限循环，直到 break 语句跳出
		struct list_head *tmp = next;  // 临时指针指向当前 inode
		struct inode *inode;

		/*
		 * We can reschedule here without worrying about the list's
		 * consistency because the per-sb list of inodes must not
		 * change during umount anymore, and because iprune_sem keeps
		 * shrink_icache_memory() away.
		 */
		/*
		 * 在此处可以重新调度，而不用担心列表的一致性，因为在卸载期间，
		 * 每个超级块的 inode 列表不应再发生变化，并且 iprune_sem 保持
		 * shrink_icache_memory() 无法干扰。
		 */
		cond_resched_lock(&inode_lock);  // 条件性重新调度，如果需要则释放并重新获得 inode_lock

		next = next->next;  // 移动到列表的下一个元素
		if (tmp == head)  // 如果回到了列表头，结束循环
			break;
		inode = list_entry(tmp, struct inode, i_sb_list);  // 获取当前的 inode 结构

		if (inode->i_state & I_NEW)  // 如果 inode 是新创建的，跳过处理
			continue;
		invalidate_inode_buffers(inode);  // 使 inode 的所有缓冲区无效

		if (!atomic_read(&inode->i_count)) {  // 如果没有进程使用这个 inode
			list_move(&inode->i_list, dispose);  // 将 inode 移动到 dispose 列表
			WARN_ON(inode->i_state & I_NEW);  // 警告：不应在 NEW 状态时执行此操作
			inode->i_state |= I_FREEING;  // 标记 inode 正在被释放
			count++;  // 增加处理计数
			continue;
		}
		busy = 1;  // 如果 inode 正在使用，设置 busy 标志
	}
	/* only unused inodes may be cached with i_count zero */
	/* 只有未使用的 inode 可以在 i_count 为零时被缓存 */
	inodes_stat.nr_unused -= count;  // 更新统计信息，减少未使用的 inode 数
	return busy;  // 返回是否有 inode 正在使用的信息
}

/**
 *	invalidate_inodes	- discard the inodes on a device
 *	@sb: superblock
 *
 *	Discard all of the inodes for a given superblock. If the discard
 *	fails because there are busy inodes then a non zero value is returned.
 *	If the discard is successful all the inodes have been discarded.
 */
/**
 *	invalidate_inodes - 废弃设备上的所有 inode
 *	@sb: 超级块
 *
 *	废弃给定超级块上的所有 inode。如果废弃操作失败（因为有 inode 正在使用中），
 *	则返回一个非零值。如果废弃成功，所有的 inode 都将被废弃。
 */
int invalidate_inodes(struct super_block *sb)
{
	int busy;  // 用于存储 invalidate_list 返回的结果，表示是否有 inode 正在使用
	LIST_HEAD(throw_away);  // 初始化临时列表 throw_away，用于存放即将被废弃的 inode

	down_write(&iprune_sem);  // 获取写锁，阻止其他进程对 inode 进行修剪或修改
	spin_lock(&inode_lock);  // 获取 inode 锁，以同步访问 inode 列表
	// 使用 inotify 和 fsnotify 通知系统所有 inode 即将卸载，可能用于清理相关监视或通知资源
	inotify_unmount_inodes(&sb->s_inodes);
	fsnotify_unmount_inodes(&sb->s_inodes);

	// 废弃超级块的 inode 列表，并收集正在使用的 inode
	busy = invalidate_list(&sb->s_inodes, &throw_away);
	spin_unlock(&inode_lock);  // 释放 inode 锁

	// 处理和释放 throw_away 列表中的所有 inode
	dispose_list(&throw_away);
	up_write(&iprune_sem);  // 释放写锁

	return busy;  // 返回是否有 inode 正在使用的状态
}
EXPORT_SYMBOL(invalidate_inodes);

/**
 * can_unuse - 检查 inode 是否可以被回收
 * @inode: 要检查的 inode 对象
 *
 * 通过检查 inode 的几个关键状态来确定是否可以将其回收。如果 inode 
 * 处于任何非空闲状态，该函数返回 0，否则返回 1。
 */
static int can_unuse(struct inode *inode)
{
	// 检查 inode 的状态，如果 inode 有任何状态标记，则不能回收
	if (inode->i_state)
		return 0;
	// 检查 inode 是否有关联的缓冲区（buffer），如果有，则不能回收
	if (inode_has_buffers(inode))
		return 0;
	// 检查 inode 的引用计数，如果引用计数非零，则有进程或系统部分正在使用，不能回收
	if (atomic_read(&inode->i_count))
		return 0;
	// 检查 inode 的数据页数，如果有数据页，则表示 inode 有活跃的数据，不能回收
	if (inode->i_data.nrpages)
		return 0;
	// 如果上述检查都未触发，说明 inode 当前未被使用，可以被回收
	return 1;
}

/*
 * Scan `goal' inodes on the unused list for freeable ones. They are moved to
 * a temporary list and then are freed outside inode_lock by dispose_list().
 *
 * Any inodes which are pinned purely because of attached pagecache have their
 * pagecache removed.  We expect the final iput() on that inode to add it to
 * the front of the inode_unused list.  So look for it there and if the
 * inode is still freeable, proceed.  The right inode is found 99.9% of the
 * time in testing on a 4-way.
 *
 * If the inode has metadata buffers attached to mapping->private_list then
 * try to remove them.
 */
/**
 * prune_icache - 修剪 inode 缓存
 * @nr_to_scan: 要扫描的 inode 数量
 *
 * 在未使用列表中扫描 `goal` 个 inode 寻找可释放的 inode。它们被移动到一个临时列表中，
 * 然后在释放 inode_lock 之后通过 dispose_list() 释放。
 *
 * 任何因为附加的页缓存而被固定的 inode 将移除其页缓存。我们预期对该 inode 的最后一次 iput()
 * 会将其添加到 inode_unused 列表的前端。因此在那里查找它，如果 inode 仍然可以被释放，
 * 则继续进行。在测试中在 4 路系统上找到正确的 inode 的概率为 99.9%。
 *
 * 如果 inode 有附加到 mapping->private_list 的元数据缓冲区，那么尝试移除它们。
 */
static void prune_icache(int nr_to_scan)
{
	LIST_HEAD(freeable);  // 初始化临时列表，用于存放可释放的 inode
	int nr_pruned = 0;  // 计数器，记录已修剪的 inode 数量
	int nr_scanned;  // 已扫描的 inode 数量
	unsigned long reap = 0;  // 用于记录已释放的页数

	down_read(&iprune_sem);  // 获取读锁
	spin_lock(&inode_lock);  // 获取 inode 锁，用于保护 inode 列表的操作
	for (nr_scanned = 0; nr_scanned < nr_to_scan; nr_scanned++) {
		struct inode *inode;

		if (list_empty(&inode_unused))  // 如果未使用的 inode 列表为空，则退出循环
			break;

		inode = list_entry(inode_unused.prev, struct inode, i_list);  // 获取列表最后一个 inode

		if (inode->i_state || atomic_read(&inode->i_count)) {  // 如果 inode 正在使用或有状态标记，则跳过
			list_move(&inode->i_list, &inode_unused);
			continue;
		}
		if (inode_has_buffers(inode) || inode->i_data.nrpages) {  // 如果 inode 有缓冲区或数据页
			__iget(inode);  // 增加 inode 的引用计数
			spin_unlock(&inode_lock);  // 释放锁，因为下面的操作可能会睡眠
			if (remove_inode_buffers(inode))  // 移除 inode 的缓冲区
				reap += invalidate_mapping_pages(&inode->i_data,
								0, -1);  // 使 inode 的页缓存失效
			iput(inode);  // 递减 inode 的引用计数
			spin_lock(&inode_lock);  // 重新获取锁

			if (inode != list_entry(inode_unused.next,
						struct inode, i_list))
				/* wrong inode or list_empty */
				continue;  // 如果 inode 已经不在预期位置，则跳过
			if (!can_unuse(inode))
				continue;  // 如果不能释放 inode，则跳过
		}
		list_move(&inode->i_list, &freeable);  // 将 inode 移动到临时列表
		WARN_ON(inode->i_state & I_NEW);  // 警告：不应在 NEW 状态时执行此操作
		inode->i_state |= I_FREEING;  // 标记 inode 正在被释放
		nr_pruned++;
	}
	inodes_stat.nr_unused -= nr_pruned;  // 更新统计数据
	if (current_is_kswapd())
		__count_vm_events(KSWAPD_INODESTEAL, reap);  // 如果是 kswapd 进程，记录事件
	else
		__count_vm_events(PGINODESTEAL, reap);  // 否则，记录页面窃取事件
	spin_unlock(&inode_lock);  // 释放 inode 锁

	dispose_list(&freeable);  // 处理临时列表，释放其中的 inode
	up_read(&iprune_sem);  // 释放读锁
}

/*
 * shrink_icache_memory() will attempt to reclaim some unused inodes.  Here,
 * "unused" means that no dentries are referring to the inodes: the files are
 * not open and the dcache references to those inodes have already been
 * reclaimed.
 *
 * This function is passed the number of inodes to scan, and it returns the
 * total number of remaining possibly-reclaimable inodes.
 */
/**
 * shrink_icache_memory - 尝试回收一些未使用的 inode
 * @nr: 要扫描的 inode 数量
 * @gfp_mask: GFP（Get Free Page）标记，决定内存分配的行为和紧急程度
 *
 * 此函数尝试回收一些未使用的 inode。这里的“未使用”指的是没有 dentries 引用这些 inode：
 * 文件没有被打开且与这些 inode 相关的 dcache 引用已经被回收。
 * 函数接收要扫描的 inode 数量，并返回可能可回收的 inode 的总数。
 */
static int shrink_icache_memory(int nr, gfp_t gfp_mask)
{
	if (nr) {  // 如果需要扫描的 inode 数量非零
		/*
		 * Nasty deadlock avoidance.  We may hold various FS locks,
		 * and we don't want to recurse into the FS that called us
		 * in clear_inode() and friends..
		 */
    /*
     * 为避免死锁，采取措施。我们可能持有各种文件系统锁，
     * 并且我们不希望在 clear_inode() 及其相关函数中递归调用引起死锁
     * 使我们调用的文件系统。
     */
		if (!(gfp_mask & __GFP_FS))
			return -1;  // 如果 gfp_mask 表明不应从文件系统代码调用，直接返回 -1
        
		prune_icache(nr);  // 清理 inode 缓存，尝试回收 nr 个 inode
	}
	// 计算并返回可能可回收的 inode 的比例
	return (inodes_stat.nr_unused / 100) * sysctl_vfs_cache_pressure;
}

static struct shrinker icache_shrinker = {
	.shrink = shrink_icache_memory,  // 指定缩减函数
	.seeks = DEFAULT_SEEKS,  // 默认的预估寻找成本
};

static void __wait_on_freeing_inode(struct inode *inode);
/*
 * Called with the inode lock held.
 * NOTE: we are not increasing the inode-refcount, you must call __iget()
 * by hand after calling find_inode now! This simplifies iunique and won't
 * add any additional branch in the common code.
 */
/*
 * 在持有 inode 锁的情况下调用。
 * 注意：我们没有增加 inode 的引用计数，调用 find_inode 后你必须手动调用 __iget()！
 * 这简化了 iunique 的处理，并且在常规代码中不会增加任何额外的分支。
 */
/**
 * find_inode - 在哈希桶中查找符合条件的 inode
 * @sb: inode 应属于的超级块
 * @head: 要搜索的哈希桶
 * @test: 一个函数指针，用于测试 inode 是否符合搜索条件
 * @data: 传递给测试函数的额外数据
 *
 * 此函数在特定的哈希桶中搜索符合条件的 inode。它需要在持有 inode 锁的情况下调用。
 */
static struct inode *find_inode(struct super_block *sb,
                                struct hlist_head *head,
                                int (*test)(struct inode *, void *),
                                void *data)
{
	struct hlist_node *node;  // 用于遍历哈希桶的节点
	struct inode *inode = NULL;  // 初始化为空，用于存放找到的 inode

repeat:  // 重复标签，用于处理 inode 状态变更的情况
	hlist_for_each_entry(inode, node, head, i_hash) {  // 遍历哈希桶中的每个 inode
		if (inode->i_sb != sb)  // 如果 inode 的超级块不匹配，则跳过
			continue;
		if (!test(inode, data))  // 如果测试函数返回 false，则跳过
			continue;
		if (inode->i_state & (I_FREEING|I_CLEAR|I_WILL_FREE)) {  // 如果 inode 正在被释放
			__wait_on_freeing_inode(inode);  // 等待 inode 释放过程完成
			goto repeat;  // 从头开始重复搜索，因为 inode 状态可能已改变
		}
		break;  // 找到匹配的 inode，跳出循环
	}
	return node ? inode : NULL;  // 如果找到 inode，返回它；否则返回 NULL
}

/*
 * find_inode_fast is the fast path version of find_inode, see the comment at
 * iget_locked for details.
 */
/*
 * find_inode_fast - 快速路径版本的 find_inode
 * @sb: inode 所属的超级块
 * @head: 要搜索的哈希表头
 * @ino: 要找的 inode 号
 *
 * find_inode_fast 是 find_inode 的快速路径版本，具体细节请参见 iget_locked 的注释。
 */
static struct inode *find_inode_fast(struct super_block *sb,
                                     struct hlist_head *head, unsigned long ino)
{
	struct hlist_node *node;  // 哈希链表节点，用于遍历
	struct inode *inode = NULL;  // 初始化为空，用于存放找到的 inode

repeat:
	hlist_for_each_entry(inode, node, head, i_hash) {  // 遍历哈希桶
		if (inode->i_ino != ino)  // 如果 inode 号不匹配，继续遍历
			continue;
		if (inode->i_sb != sb)  // 如果超级块不匹配，继续遍历
			continue;
		if (inode->i_state & (I_FREEING|I_CLEAR|I_WILL_FREE)) {  // 如果 inode 正在被释放
			__wait_on_freeing_inode(inode);  // 等待 inode 释放完成
			goto repeat;  // 重新开始搜索，因为列表状态可能已改变
		}
		break;  // 找到符合条件的 inode，跳出循环
	}
	return node ? inode : NULL;  // 如果找到 inode，则返回它；否则返回 NULL
}

/**
 * hash - 计算 inode 的哈希值
 * @sb: inode 所属的超级块
 * @hashval: 原始的哈希值（通常是 inode 号）
 *
 * 这个函数用于根据 inode 号和超级块指针计算哈希值，以决定 inode 在哈希表中的位置。
 */
static unsigned long hash(struct super_block *sb, unsigned long hashval)
{
	unsigned long tmp;

	// 通过混合超级块地址和 inode 号生成一个初步的哈希值
	tmp = (hashval * (unsigned long)sb) ^ (GOLDEN_RATIO_PRIME + hashval) /
			L1_CACHE_BYTES;
	// 进一步混合和折叠哈希值以减小碰撞和提高哈希表的分布性
	tmp = tmp ^ ((tmp ^ GOLDEN_RATIO_PRIME) >> I_HASHBITS);
	// 根据 I_HASHMASK 掩码确定最终的哈希值，确保哈希值不会超出哈希表的大小
	return tmp & I_HASHMASK;
}

/**
 * __inode_add_to_lists - 将 inode 添加到相关列表中
 * @sb: inode 所属的超级块
 * @head: 哈希桶的头节点
 * @inode: 要添加的 inode
 *
 * 这个函数将 inode 添加到全局 inode 列表、超级块的 inode 列表和哈希桶中。
 */
static inline void
__inode_add_to_lists(struct super_block *sb, struct hlist_head *head,
                     struct inode *inode)
{
	// 增加统计中的 inode 数量
	inodes_stat.nr_inodes++;
	// 将 inode 添加到全局的 inode 使用列表
	list_add(&inode->i_list, &inode_in_use);
	// 将 inode 添加到所属超级块的 inode 列表
	list_add(&inode->i_sb_list, &sb->s_inodes);
	// 如果提供了哈希桶的头节点，则将 inode 添加到对应的哈希桶
	if (head)
		hlist_add_head(&inode->i_hash, head);
}

/**
 * inode_add_to_lists - add a new inode to relevant lists
 * @sb: superblock inode belongs to
 * @inode: inode to mark in use
 *
 * When an inode is allocated it needs to be accounted for, added to the in use
 * list, the owning superblock and the inode hash. This needs to be done under
 * the inode_lock, so export a function to do this rather than the inode lock
 * itself. We calculate the hash list to add to here so it is all internal
 * which requires the caller to have already set up the inode number in the
 * inode to add.
 */
/**
 * inode_add_to_lists - 将新 inode 添加到相关列表
 * @sb: inode 所属的超级块
 * @inode: 要标记为使用中的 inode
 *
 * 当一个 inode 被分配后，需要进行统计并添加到使用中列表、拥有的超级块以及 inode 哈希表中。
 * 这些操作需要在持有 inode_lock 的情况下完成，因此提供一个函数来执行这些操作，
 * 而不是直接导出 inode 锁本身。我们在这里计算要添加的哈希列表，这一切都是内部的，
 * 要求调用者已经在要添加的 inode 中设置了 inode 号。
 */
void inode_add_to_lists(struct super_block *sb, struct inode *inode)
{
	// 计算 inode 应该添加到的哈希桶
	struct hlist_head *head = inode_hashtable + hash(sb, inode->i_ino);

	// 获取 inode 锁
	spin_lock(&inode_lock);
	// 调用 __inode_add_to_lists 将 inode 添加到相关列表
	__inode_add_to_lists(sb, head, inode);
	// 释放 inode 锁
	spin_unlock(&inode_lock);
}
EXPORT_SYMBOL_GPL(inode_add_to_lists);

/**
 *	new_inode 	- obtain an inode
 *	@sb: superblock
 *
 *	Allocates a new inode for given superblock. The default gfp_mask
 *	for allocations related to inode->i_mapping is GFP_HIGHUSER_MOVABLE.
 *	If HIGHMEM pages are unsuitable or it is known that pages allocated
 *	for the page cache are not reclaimable or migratable,
 *	mapping_set_gfp_mask() must be called with suitable flags on the
 *	newly created inode's mapping
 *
 */
/**
 * new_inode - 获取一个 inode
 * @sb: 超级块
 *
 * 为给定的超级块分配一个新的 inode。与 inode->i_mapping 相关的分配的默认 gfp_mask 是 GFP_HIGHUSER_MOVABLE。
 * 如果 HIGHMEM 页面不适合或已知为页缓存分配的页面不可回收或不可迁移，
 * 则必须在新创建的 inode 的映射上调用 mapping_set_gfp_mask() 设置适当的标志。
 */
struct inode *new_inode(struct super_block *sb)
{
	/*
	 * On a 32bit, non LFS stat() call, glibc will generate an EOVERFLOW
	 * error if st_ino won't fit in target struct field. Use 32bit counter
	 * here to attempt to avoid that.
	 */
  /*
   * 在一个32位系统中，非LFS的 stat() 调用中，如果 st_ino 无法适应目标结构字段，glibc 将生成一个 EOVERFLOW 错误。
   * 这里使用32位计数器尝试避免该问题。
   */
	static unsigned int last_ino;  // 静态变量，用于生成 inode 编号
	struct inode *inode;

	// 预取 inode 锁，优化接下来的锁操作
	spin_lock_prefetch(&inode_lock);

	inode = alloc_inode(sb);  // 分配一个新的 inode
	if (inode) {  // 如果 inode 成功分配
		spin_lock(&inode_lock);  // 加锁，保护 inode 的列表操作
		__inode_add_to_lists(sb, NULL, inode);  // 将 inode 添加到相关列表
		inode->i_ino = ++last_ino;  // 分配唯一的 inode 编号
		inode->i_state = 0;  // 初始化 inode 状态为 0
		spin_unlock(&inode_lock);  // 解锁
	}
	return inode;  // 返回新分配的 inode
}
EXPORT_SYMBOL(new_inode);

/**
 * unlock_new_inode - 解锁新创建的 inode
 * @inode: 要解锁的 inode
 *
 * 这个函数用于解锁一个新创建的 inode，主要操作是清除 inode 的 I_NEW 状态，并确保其他CPU可靠地
 * 观察到 I_NEW 状态的清除，此操作之后 inode 的其他初始化操作已经完成。
 */
void unlock_new_inode(struct inode *inode)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    // 如果 inode 是一个目录，并且当前文件系统类型的互斥锁还没有被更改
	if (inode->i_mode & S_IFDIR) {
		struct file_system_type *type = inode->i_sb->s_type;

		/* Set new key only if filesystem hasn't already changed it */
		// 如果互斥锁的类没有匹配，重新初始化互斥锁
		if (!lockdep_match_class(&inode->i_mutex,
		    &type->i_mutex_key)) {
			/*
			 * ensure nobody is actually holding i_mutex
			 */
			/*
			 * 确保没有人持有 i_mutex
			 */
			mutex_destroy(&inode->i_mutex);
			mutex_init(&inode->i_mutex);
			lockdep_set_class(&inode->i_mutex,
					  &type->i_mutex_dir_key);
		}
	}
#endif
	/*
	 * This is special!  We do not need the spinlock when clearing I_NEW,
	 * because we're guaranteed that nobody else tries to do anything about
	 * the state of the inode when it is locked, as we just created it (so
	 * there can be no old holders that haven't tested I_NEW).
	 * However we must emit the memory barrier so that other CPUs reliably
	 * see the clearing of I_NEW after the other inode initialisation has
	 * completed.
	 */
	/*
	 * 这是特殊情况！清除 I_NEW 时我们不需要自旋锁，
	 * 因为我们保证在锁定状态下没有其他人尝试对 inode 状态进行任何操作，
	 * 我们刚刚创建了 inode（因此不可能有没有测试过 I_NEW 的旧持有者）。
	 * 但是我们必须发出内存屏障，以确保其他 CPU 可靠地看到在其他 inode 初始化完成后
	 * 清除 I_NEW 的操作。
	 */
	smp_mb();  // 发出内存屏障
	WARN_ON(!(inode->i_state & I_NEW));  // 警告：如果 I_NEW 未被设置则发出警告
	inode->i_state &= ~I_NEW;  // 清除 inode 的 I_NEW 状态
	wake_up_inode(inode);  // 唤醒等待这个 inode 的进程
}
EXPORT_SYMBOL(unlock_new_inode);

/*
 * This is called without the inode lock held.. Be careful.
 *
 * We no longer cache the sb_flags in i_flags - see fs.h
 *	-- rmk@arm.uk.linux.org
 */
/**
 * get_new_inode - 获取一个新的 inode 或现有的相同 inode
 * @sb: 超级块
 * @head: 要搜索的哈希桶
 * @test: 函数指针，用于检查 inode 是否符合给定条件
 * @set: 函数指针，用于设置 inode 的初始化状态
 * @data: 传递给 test 和 set 函数的数据
 *
 * 这个函数被调用时不持有 inode 锁，请小心使用。
 * 我们不再在 i_flags 中缓存 sb_flags - 参见 fs.h
 *	-- rmk@arm.uk.linux.org
 */
static struct inode *get_new_inode(struct super_block *sb,
                                   struct hlist_head *head,
                                   int (*test)(struct inode *, void *),
                                   int (*set)(struct inode *, void *),
                                   void *data)
{
	struct inode *inode;

	inode = alloc_inode(sb);  // 分配一个新的 inode
	if (inode) {
		struct inode *old;

		spin_lock(&inode_lock);  // 加锁保护 inode 的操作
		/* We released the lock, so.. */
		/* 由于我们释放了锁，所以... */
		old = find_inode(sb, head, test, data);  // 查找是否已经存在符合条件的 inode
		if (!old) {  // 如果没有找到
			if (set(inode, data))  // 尝试设置 inode
				goto set_failed;  // 如果设置失败，跳转到错误处理

			__inode_add_to_lists(sb, head, inode);  // 将新 inode 添加到相关列表
			inode->i_state = I_NEW;  // 标记为新 inode
			spin_unlock(&inode_lock);  // 解锁

			/* Return the locked inode with I_NEW set, the
			 * caller is responsible for filling in the contents
			 */
			/* 返回锁定状态的 inode，并设置 I_NEW，调用者负责填充内容 */
			return inode;
		}

		/*
		 * Uhhuh, somebody else created the same inode under
		 * us. Use the old inode instead of the one we just
		 * allocated.
		 */
		/*
		 * 噢，有人在我们操作期间创建了相同的 inode。
		 * 使用旧的 inode 替代我们刚刚分配的那个。
		 */
		__iget(old);  // 增加找到的旧 inode 的引用计数
		spin_unlock(&inode_lock);  // 解锁
		destroy_inode(inode);  // 销毁新分配的 inode
		inode = old;  // 使用旧的 inode
		wait_on_inode(inode);  // 等待旧 inode 准备好
	}
	return inode;  // 返回 inode

set_failed:
	spin_unlock(&inode_lock);  // 解锁
	destroy_inode(inode);  // 销毁新分配但初始化失败的 inode
	return NULL;  // 返回 NULL 表示失败
}

/*
 * get_new_inode_fast is the fast path version of get_new_inode, see the
 * comment at iget_locked for details.
 */
/**
 * get_new_inode_fast - 获取新 inode 的快速路径版本
 * @sb: 超级块
 * @head: 哈希表的头节点
 * @ino: inode 号
 *
 * 这是 get_new_inode 的快速路径版本，详情请参见 iget_locked 的注释。
 */
static struct inode *get_new_inode_fast(struct super_block *sb,
                                        struct hlist_head *head, unsigned long ino)
{
	struct inode *inode;

	inode = alloc_inode(sb);  // 尝试为指定的超级块分配一个新的 inode
	if (inode) {
		struct inode *old;

		spin_lock(&inode_lock);  // 获取 inode 锁以保护接下来的操作
		/* We released the lock, so.. */
		/* 我们释放了锁，所以.. */
		old = find_inode_fast(sb, head, ino);  // 快速查找是否已存在具有相同 inode 号的 inode
		if (!old) {  // 如果没有找到已存在的 inode
			inode->i_ino = ino;  // 设置新 inode 的 inode 号
			__inode_add_to_lists(sb, head, inode);  // 将新 inode 添加到相关列表
			inode->i_state = I_NEW;  // 标记新 inode 的状态为 I_NEW
			spin_unlock(&inode_lock);  // 释放 inode 锁

			/* Return the locked inode with I_NEW set, the
			 * caller is responsible for filling in the contents
			 */
			/* 返回锁定状态的 inode，设置了 I_NEW，调用者负责填充内容 */
			return inode;
		}

		/*
		 * Uhhuh, somebody else created the same inode under
		 * us. Use the old inode instead of the one we just
		 * allocated.
		 */
		/*
		 * 哎呀，有人在我们之下创建了相同的 inode。
		 * 使用旧的 inode 而不是我们刚刚分配的那个。
		 */
		__iget(old);  // 增加找到的 inode 的引用计数
		spin_unlock(&inode_lock);  // 释放 inode 锁
		destroy_inode(inode);  // 销毁刚刚分配的新 inode
		inode = old;  // 使用找到的旧 inode
		wait_on_inode(inode);  // 等待 inode 准备好
	}
	return inode;  // 返回找到或创建的 inode
}

/**
 *	iunique - get a unique inode number
 *	@sb: superblock
 *	@max_reserved: highest reserved inode number
 *
 *	Obtain an inode number that is unique on the system for a given
 *	superblock. This is used by file systems that have no natural
 *	permanent inode numbering system. An inode number is returned that
 *	is higher than the reserved limit but unique.
 *
 *	BUGS:
 *	With a large number of inodes live on the file system this function
 *	currently becomes quite slow.
 */
/**
 * iunique - 获取一个独特的 inode 号码
 * @sb: 超级块
 * @max_reserved: 最高的保留 inode 号码
 *
 * 为给定的超级块获取一个在系统上唯一的 inode 号码。这适用于没有自然、永久 inode 编号系统的文件系统。
 * 返回一个高于保留限制但独特的 inode 号码。
 *
 * BUGS:
 * 在文件系统上有大量 inode 时，这个函数当前变得相当慢。
 */
ino_t iunique(struct super_block *sb, ino_t max_reserved)
{
	/*
	 * On a 32bit, non LFS stat() call, glibc will generate an EOVERFLOW
	 * error if st_ino won't fit in target struct field. Use 32bit counter
	 * here to attempt to avoid that.
	 */
	/*
	 * 在一个32位系统上，非LFS的 stat() 调用中，如果 st_ino 无法适应目标结构字段，
	 * glibc 将生成一个 EOVERFLOW 错误。这里使用32位计数器尝试避免该问题。
	 */
	static unsigned int counter;  // 静态计数器，用于生成 inode 号码
	struct inode *inode;
	struct hlist_head *head;
	ino_t res;

	spin_lock(&inode_lock);  // 锁定，以同步访问 inode
	do {
		if (counter <= max_reserved)
			counter = max_reserved + 1;  // 确保计数器高于保留的最大值
		res = counter++;  // 分配一个新的 inode 号码
		head = inode_hashtable + hash(sb, res);  // 计算这个 inode 号码的哈希值，得到相应的哈希桶
		inode = find_inode_fast(sb, head, res);  // 快速检查此 inode 号码是否已被使用
	} while (inode != NULL);  // 如果这个 inode 号码已存在，继续循环
	spin_unlock(&inode_lock);  // 解锁

	return res;  // 返回新的、唯一的 inode 号码
}
EXPORT_SYMBOL(iunique);

/**
 * igrab - 增加 inode 的引用计数
 * @inode: 要操作的 inode 对象
 *
 * 此函数尝试安全地增加 inode 的引用计数。如果 inode 正在被释放，则不增加计数并返回 NULL。
 * 这确保了调用者不会在 inode 释放过程中持有该 inode。
 */
struct inode *igrab(struct inode *inode)
{
	spin_lock(&inode_lock);  // 锁定，保证以下操作的原子性
	if (!(inode->i_state & (I_FREEING|I_CLEAR|I_WILL_FREE))) {
		__iget(inode);  // 如果 inode 不在释放状态，增加其引用计数
	} else {
		/*
		 * Handle the case where s_op->clear_inode is not been
		 * called yet, and somebody is calling igrab
		 * while the inode is getting freed.
		 */
    /*
     * 处理这样一种情况：s_op->clear_inode 还未被调用，
     * 而有人在 inode 正在被释放时调用 igrab。
     */
		inode = NULL;  // 如果 inode 正在被释放，将 inode 设置为 NULL
	}
	spin_unlock(&inode_lock);  // 解锁
	return inode;  // 返回 inode 或 NULL
}
EXPORT_SYMBOL(igrab);

/**
 * ifind - internal function, you want ilookup5() or iget5().
 * @sb:		super block of file system to search
 * @head:       the head of the list to search
 * @test:	callback used for comparisons between inodes
 * @data:	opaque data pointer to pass to @test
 * @wait:	if true wait for the inode to be unlocked, if false do not
 *
 * ifind() searches for the inode specified by @data in the inode
 * cache. This is a generalized version of ifind_fast() for file systems where
 * the inode number is not sufficient for unique identification of an inode.
 *
 * If the inode is in the cache, the inode is returned with an incremented
 * reference count.
 *
 * Otherwise NULL is returned.
 *
 * Note, @test is called with the inode_lock held, so can't sleep.
 */
/**
 * ifind - 内部函数，通常使用 ilookup5() 或 iget5()。
 * @sb: 要搜索的文件系统的超级块
 * @head: 要搜索的列表的头
 * @test: 用于比较 inode 的回调函数
 * @data: 传递给 @test 的不透明数据指针
 * @wait: 如果为真，则等待 inode 解锁；如果为假，则不等待
 *
 * ifind() 在 inode 缓存中搜索由 @data 指定的 inode。
 * 这是 ifind_fast() 的泛化版本，适用于 inode 编号不足以唯一识别 inode 的文件系统。
 *
 * 如果在缓存中找到 inode，返回的 inode 将增加引用计数。
 *
 * 否则返回 NULL。
 *
 * 注意，@test 调用时会持有 inode_lock，因此不能休眠。
 */
static struct inode *ifind(struct super_block *sb,
		struct hlist_head *head, int (*test)(struct inode *, void *),
		void *data, const int wait)
{
	struct inode *inode;

	spin_lock(&inode_lock);  // 加锁，以保护 inode 的查找过程
	inode = find_inode(sb, head, test, data);  // 在给定的哈希桶中搜索 inode
	if (inode) {
		__iget(inode);  // 如果找到，增加 inode 的引用计数
		spin_unlock(&inode_lock);  // 解锁
		if (likely(wait))  // 如果调用者指定等待 inode 解锁
			wait_on_inode(inode);  // 等待 inode 完全可用
		return inode;  // 返回找到的 inode
	}
	spin_unlock(&inode_lock);  // 如果未找到，释放锁并返回 NULL
	return NULL;
}

/**
 * ifind_fast - internal function, you want ilookup() or iget().
 * @sb:		super block of file system to search
 * @head:       head of the list to search
 * @ino:	inode number to search for
 *
 * ifind_fast() searches for the inode @ino in the inode cache. This is for
 * file systems where the inode number is sufficient for unique identification
 * of an inode.
 *
 * If the inode is in the cache, the inode is returned with an incremented
 * reference count.
 *
 * Otherwise NULL is returned.
 */
/**
 * ifind_fast - 内部函数，通常应使用 ilookup() 或 iget()。
 * @sb: 要搜索的文件系统的超级块
 * @head: 要搜索的列表的头部
 * @ino: 要搜索的 inode 号
 *
 * ifind_fast() 在 inode 缓存中搜索指定的 inode @ino。这适用于 inode 号
 * 足以唯一识别 inode 的文件系统。
 *
 * 如果 inode 在缓存中找到，返回的 inode 将增加引用计数。
 *
 * 否则返回 NULL。
 */
static struct inode *ifind_fast(struct super_block *sb,
                                struct hlist_head *head, unsigned long ino)
{
	struct inode *inode;

	spin_lock(&inode_lock);  // 加锁，保护 inode 的查找过程
	inode = find_inode_fast(sb, head, ino);  // 快速查找指定 inode 号的 inode
	if (inode) {
		__iget(inode);  // 如果找到，增加 inode 的引用计数
		spin_unlock(&inode_lock);  // 解锁
		wait_on_inode(inode);  // 等待 inode 解锁（如果之前被锁定）
		return inode;  // 返回找到的 inode
	}
	spin_unlock(&inode_lock);  // 如果未找到，解锁
	return NULL;  // 返回 NULL 表示没有找到对应的 inode
}

/**
 * ilookup5_nowait - search for an inode in the inode cache
 * @sb:		super block of file system to search
 * @hashval:	hash value (usually inode number) to search for
 * @test:	callback used for comparisons between inodes
 * @data:	opaque data pointer to pass to @test
 *
 * ilookup5() uses ifind() to search for the inode specified by @hashval and
 * @data in the inode cache. This is a generalized version of ilookup() for
 * file systems where the inode number is not sufficient for unique
 * identification of an inode.
 *
 * If the inode is in the cache, the inode is returned with an incremented
 * reference count.  Note, the inode lock is not waited upon so you have to be
 * very careful what you do with the returned inode.  You probably should be
 * using ilookup5() instead.
 *
 * Otherwise NULL is returned.
 *
 * Note, @test is called with the inode_lock held, so can't sleep.
 */
/**
 * ilookup5_nowait - 在 inode 缓存中搜索一个 inode
 * @sb: 要搜索的文件系统的超级块
 * @hashval: 要搜索的哈希值（通常是 inode 号）
 * @test: 用于比较 inode 的回调函数
 * @data: 传递给 @test 的不透明数据指针
 *
 * ilookup5() 使用 ifind() 来搜索由 @hashval 和 @data 指定的 inode 缓存。
 * 这是 ilookup() 的泛化版本，适用于 inode 编号不足以唯一识别 inode 的文件系统。
 *
 * 如果找到 inode，则返回的 inode 将增加引用计数。请注意，不会等待 inode 锁，
 * 因此你必须非常小心地处理返回的 inode。你可能应该使用 ilookup5() 而不是本函数。
 *
 * 否则返回 NULL。
 *
 * 注意，@test 在持有 inode_lock 的情况下调用，因此不能休眠。
 */
struct inode *ilookup5_nowait(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(sb, hashval);  // 根据哈希值获取哈希桶的头

	return ifind(sb, head, test, data, 0);  // 调用 ifind 函数搜索 inode，不等待锁
}
EXPORT_SYMBOL(ilookup5_nowait);

/**
 * ilookup5 - search for an inode in the inode cache
 * @sb:		super block of file system to search
 * @hashval:	hash value (usually inode number) to search for
 * @test:	callback used for comparisons between inodes
 * @data:	opaque data pointer to pass to @test
 *
 * ilookup5() uses ifind() to search for the inode specified by @hashval and
 * @data in the inode cache. This is a generalized version of ilookup() for
 * file systems where the inode number is not sufficient for unique
 * identification of an inode.
 *
 * If the inode is in the cache, the inode lock is waited upon and the inode is
 * returned with an incremented reference count.
 *
 * Otherwise NULL is returned.
 *
 * Note, @test is called with the inode_lock held, so can't sleep.
 */
/**
 * ilookup5 - 在 inode 缓存中搜索 inode
 * @sb: 要搜索的文件系统的超级块
 * @hashval: 要搜索的哈希值（通常是 inode 号）
 * @test: 用于比较 inode 的回调函数
 * @data: 传递给 @test 的不透明数据指针
 *
 * ilookup5() 使用 ifind() 来搜索由 @hashval 和 @data 指定的 inode 缓存。
 * 这是 ilookup() 的泛化版本，适用于 inode 编号不足以唯一识别 inode 的文件系统。
 *
 * 如果 inode 在缓存中找到，将等待 inode 锁，返回的 inode 将增加引用计数。
 *
 * 否则返回 NULL。
 *
 * 注意，@test 在持有 inode_lock 的情况下调用，因此不能休眠。
 */
struct inode *ilookup5(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(sb, hashval); // 计算哈希桶位置

	return ifind(sb, head, test, data, 1); // 调用 ifind 函数搜索 inode，并在找到时等待 inode 解锁
}
EXPORT_SYMBOL(ilookup5);

/**
 * ilookup - search for an inode in the inode cache
 * @sb:		super block of file system to search
 * @ino:	inode number to search for
 *
 * ilookup() uses ifind_fast() to search for the inode @ino in the inode cache.
 * This is for file systems where the inode number is sufficient for unique
 * identification of an inode.
 *
 * If the inode is in the cache, the inode is returned with an incremented
 * reference count.
 *
 * Otherwise NULL is returned.
 */
/**
 * ilookup - 在 inode 缓存中搜索 inode
 * @sb: 要搜索的文件系统的超级块
 * @ino: 要搜索的 inode 号
 *
 * ilookup() 使用 ifind_fast() 来搜索 inode 缓存中的 @ino。
 * 这适用于 inode 编号足以唯一识别 inode 的文件系统。
 *
 * 如果 inode 在缓存中找到，返回的 inode 将增加引用计数。
 *
 * 否则返回 NULL。
 */
struct inode *ilookup(struct super_block *sb, unsigned long ino)
{
	struct hlist_head *head = inode_hashtable + hash(sb, ino); // 根据超级块和 inode 号计算哈希桶位置

	return ifind_fast(sb, head, ino); // 调用 ifind_fast 函数快速搜索指定 inode 号的 inode
}
EXPORT_SYMBOL(ilookup);

/**
 * iget5_locked - obtain an inode from a mounted file system
 * @sb:		super block of file system
 * @hashval:	hash value (usually inode number) to get
 * @test:	callback used for comparisons between inodes
 * @set:	callback used to initialize a new struct inode
 * @data:	opaque data pointer to pass to @test and @set
 *
 * iget5_locked() uses ifind() to search for the inode specified by @hashval
 * and @data in the inode cache and if present it is returned with an increased
 * reference count. This is a generalized version of iget_locked() for file
 * systems where the inode number is not sufficient for unique identification
 * of an inode.
 *
 * If the inode is not in cache, get_new_inode() is called to allocate a new
 * inode and this is returned locked, hashed, and with the I_NEW flag set. The
 * file system gets to fill it in before unlocking it via unlock_new_inode().
 *
 * Note both @test and @set are called with the inode_lock held, so can't sleep.
 */
/**
 * iget5_locked - 从挂载的文件系统中获取 inode
 * @sb: 文件系统的超级块
 * @hashval: 要获取的哈希值（通常是 inode 号）
 * @test: 用于比较 inode 的回调函数
 * @set: 用于初始化新的 struct inode 的回调函数
 * @data: 传递给 @test 和 @set 的不透明数据指针
 *
 * iget5_locked() 使用 ifind() 来搜索由 @hashval 和 @data 指定的 inode 缓存，
 * 如果存在则返回并增加引用计数。这是 iget_locked() 的泛化版本，适用于 inode 编号不足以唯一
 * 识别 inode 的文件系统。
 *
 * 如果 inode 不在缓存中，调用 get_new_inode() 来分配一个新的 inode，并返回这个已锁定、
 * 已哈希化，并且设置了 I_NEW 标志的 inode。文件系统需要在通过 unlock_new_inode() 解锁之前填充它。
 *
 * 注意，@test 和 @set 都在持有 inode_lock 的情况下调用，因此不能休眠。
 */
struct inode *iget5_locked(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *),
		int (*set)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(sb, hashval);  // 计算哈希桶位置
	struct inode *inode;

	inode = ifind(sb, head, test, data, 1);  // 在 inode 缓存中搜索指定的 inode
	if (inode)
		return inode;  // 如果找到，直接返回 inode
	/*
	 * get_new_inode() will do the right thing, re-trying the search
	 * in case it had to block at any point.
	 */
	/*
	 * 如果在缓存中未找到 inode，get_new_inode() 会执行正确的操作，包括在必要时
	 * 重新尝试搜索，以处理可能在任何时点阻塞的情况。
	 */
	return get_new_inode(sb, head, test, set, data);  // 分配并返回一个新的 inode
}
EXPORT_SYMBOL(iget5_locked);

/**
 * iget_locked - obtain an inode from a mounted file system
 * @sb:		super block of file system
 * @ino:	inode number to get
 *
 * iget_locked() uses ifind_fast() to search for the inode specified by @ino in
 * the inode cache and if present it is returned with an increased reference
 * count. This is for file systems where the inode number is sufficient for
 * unique identification of an inode.
 *
 * If the inode is not in cache, get_new_inode_fast() is called to allocate a
 * new inode and this is returned locked, hashed, and with the I_NEW flag set.
 * The file system gets to fill it in before unlocking it via
 * unlock_new_inode().
 */
/**
 * iget_locked - 从挂载的文件系统获取 inode
 * @sb: 文件系统的超级块
 * @ino: 要获取的 inode 号
 *
 * iget_locked() 使用 ifind_fast() 来搜索由 @ino 指定的 inode 缓存，
 * 如果存在，则返回的 inode 将增加引用计数。这适用于 inode 号足以唯一识别 inode 的文件系统。
 *
 * 如果 inode 不在缓存中，调用 get_new_inode_fast() 来分配一个新的 inode，
 * 并且这个新的 inode 返回时将被锁定、加入哈希表，并设置了 I_NEW 标志。
 * 文件系统需要在通过 unlock_new_inode() 解锁之前填充它。
 */
struct inode *iget_locked(struct super_block *sb, unsigned long ino)
{
	struct hlist_head *head = inode_hashtable + hash(sb, ino);  // 根据超级块和 inode 号计算哈希桶的位置
	struct inode *inode;

	inode = ifind_fast(sb, head, ino);  // 快速在 inode 缓存中查找指定的 inode
	if (inode)
		return inode;  // 如果找到，返回该 inode

	/*
	 * get_new_inode_fast() will do the right thing, re-trying the search
	 * in case it had to block at any point.
	 */
  /*
   * 如果在缓存中未找到 inode，get_new_inode_fast() 会执行正确的操作，
   * 包括在必要时重新尝试搜索，以处理可能在任何时点阻塞的情况。
   */
	return get_new_inode_fast(sb, head, ino);  // 分配并返回一个新的 inode
}
EXPORT_SYMBOL(iget_locked);

/**
 * insert_inode_locked - 将一个 inode 锁定并插入到 inode 哈希表中
 * @inode: 要插入的 inode
 *
 * 这个函数尝试将一个新的 inode 插入到其对应的超级块的 inode 哈希表中。
 * 如果在哈希表中已经存在具有相同 inode 号和超级块的活跃 inode，则该函数
 * 等待该 inode 释放，并检查其状态。如果该 inode 仍然在哈希表中，则返回
 * -EBUSY 表示插入失败。否则，将新的 inode 插入哈希表。
 *
 * 返回值:
 * 如果成功插入，返回 0。
 * 如果因为哈希表中已存在相同的 inode 而失败，返回 -EBUSY。
 */
int insert_inode_locked(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;  // 获取 inode 所属的超级块
	ino_t ino = inode->i_ino;  // 获取 inode 的编号
	struct hlist_head *head = inode_hashtable + hash(sb, ino);  // 计算哈希位置

	inode->i_state |= I_NEW;  // 标记 inode 为新创建状态
	while (1) {
		struct hlist_node *node;
		struct inode *old = NULL;
		spin_lock(&inode_lock);  // 加锁保护 inode 哈希表
		// 遍历哈希链表查找冲突的 inode
		hlist_for_each_entry(old, node, head, i_hash) {
			// 跳过不匹配或正在释放的 inode
			if (old->i_ino != ino)
				continue;
			if (old->i_sb != sb)
				continue;
			if (old->i_state & (I_FREEING|I_CLEAR|I_WILL_FREE))
				continue;
			break;  // 找到匹配的 inode
		}
		if (likely(!node)) {  // 如果没有找到冲突的 inode
			hlist_add_head(&inode->i_hash, head);  // 将新 inode 加入哈希表
			spin_unlock(&inode_lock);  // 解锁
			return 0;  // 插入成功
		}
		__iget(old);  // 增加找到的 inode 的引用计数
		spin_unlock(&inode_lock);  // 解锁
		wait_on_inode(old);  // 等待 inode 解锁
		if (unlikely(!hlist_unhashed(&old->i_hash))) {  // 如果 old inode 仍在哈希表中
			iput(old);  // 释放引用
			return -EBUSY;  // 返回忙碌状态
		}
		iput(old);  // 释放引用
	}
}
EXPORT_SYMBOL(insert_inode_locked);

/**
 * insert_inode_locked4 - 将 inode 插入到 inode 缓存中
 * @inode: 要插入的 inode
 * @hashval: 用于哈希表的哈希值
 * @test: 用于检查 inode 唯一性的测试函数
 * @data: 传递给测试函数的数据
 *
 * 此函数尝试将一个 inode 插入到哈希表中。如果根据提供的测试函数确定没有冲突的 inode，
 * 则 inode 被插入。如果发现冲突的 inode 仍在使用中（未被释放），则函数返回 -EBUSY。
 *
 * 返回值:
 * 如果成功插入，返回 0。
 * 如果发现冲突的 inode，返回 -EBUSY。
 */
int insert_inode_locked4(struct inode *inode, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data)
{
	struct super_block *sb = inode->i_sb;  // inode 所属的超级块
	struct hlist_head *head = inode_hashtable + hash(sb, hashval);  // 根据哈希值计算哈希桶

	inode->i_state |= I_NEW;  // 标记 inode 为新创建状态

	while (1) {
		struct hlist_node *node;
		struct inode *old = NULL;

		spin_lock(&inode_lock);  // 加锁保护 inode 哈希表
		// 遍历哈希桶中的 inode
		hlist_for_each_entry(old, node, head, i_hash) {
			// 跳过不符合条件的 inode
			if (old->i_sb != sb)
				continue;
			if (!test(old, data))
				continue;
			if (old->i_state & (I_FREEING|I_CLEAR|I_WILL_FREE))
				continue;
			break;  // 找到冲突的 inode
		}
		if (likely(!node)) {  // 如果没有找到冲突的 inode
			hlist_add_head(&inode->i_hash, head);  // 将新 inode 加入哈希表
			spin_unlock(&inode_lock);  // 解锁
			return 0;  // 插入成功
		}
		__iget(old);  // 增加找到的 inode 的引用计数
		spin_unlock(&inode_lock);  // 解锁
		wait_on_inode(old);  // 等待 inode 解锁
		if (unlikely(!hlist_unhashed(&old->i_hash))) {  // 如果 old inode 仍在哈希表中
			iput(old);  // 释放引用
			return -EBUSY;  // 返回忙碌状态
		}
		iput(old);  // 释放引用
	}
}
EXPORT_SYMBOL(insert_inode_locked4);

/**
 *	__insert_inode_hash - hash an inode
 *	@inode: unhashed inode
 *	@hashval: unsigned long value used to locate this object in the
 *		inode_hashtable.
 *
 *	Add an inode to the inode hash for this superblock.
 */
/**
 * __insert_inode_hash - 将 inode 加入哈希表
 * @inode: 未被哈希的 inode
 * @hashval: 用于定位此对象在 inode_hashtable 中的位置的 unsigned long 值
 *
 * 将一个 inode 添加到这个超级块的 inode 哈希表中。
 */
void __insert_inode_hash(struct inode *inode, unsigned long hashval)
{
	struct hlist_head *head = inode_hashtable + hash(inode->i_sb, hashval); // 计算此 inode 应当加入的哈希桶位置
	spin_lock(&inode_lock); // 加锁以保护哈希表的修改
	hlist_add_head(&inode->i_hash, head); // 将 inode 加入到计算出的哈希桶的头部
	spin_unlock(&inode_lock); // 解锁
}
EXPORT_SYMBOL(__insert_inode_hash);

/**
 *	remove_inode_hash - remove an inode from the hash
 *	@inode: inode to unhash
 *
 *	Remove an inode from the superblock.
 */
/**
 * remove_inode_hash - 从哈希表中移除一个 inode
 * @inode: 要移除的 inode
 *
 * 从超级块中移除一个 inode。
 */
void remove_inode_hash(struct inode *inode)
{
	spin_lock(&inode_lock);  // 加锁以保护 inode 哈希表的修改
	hlist_del_init(&inode->i_hash);  // 从哈希表中移除 inode，并初始化 inode 的哈希节点
	spin_unlock(&inode_lock);  // 解锁
}
EXPORT_SYMBOL(remove_inode_hash);

/*
 * Tell the filesystem that this inode is no longer of any interest and should
 * be completely destroyed.
 *
 * We leave the inode in the inode hash table until *after* the filesystem's
 * ->delete_inode completes.  This ensures that an iget (such as nfsd might
 * instigate) will always find up-to-date information either in the hash or on
 * disk.
 *
 * I_FREEING is set so that no-one will take a new reference to the inode while
 * it is being deleted.
 */
/**
 * generic_delete_inode - 通知文件系统销毁指定的 inode
 * @inode: 要销毁的 inode
 *
 * 告诉文件系统这个 inode 不再有任何意义，应该被彻底销毁。
 *
 * 我们在文件系统的 ->delete_inode 完成之后才从 inode 哈希表中移除 inode。
 * 这确保了如 nfsd 所发起的 iget 总能在哈希表中或磁盘上找到最新信息。
 *
 * 设置 I_FREEING 状态以确保在删除过程中没有新的引用被加到这个 inode 上。
 */
void generic_delete_inode(struct inode *inode)
{
	const struct super_operations *op = inode->i_sb->s_op;

	list_del_init(&inode->i_list);  // 从 inode 列表中移除并初始化
	list_del_init(&inode->i_sb_list);  // 从超级块的 inode 列表中移除并初始化
	WARN_ON(inode->i_state & I_NEW);  // 如果 inode 状态为 I_NEW，则发出警告
	inode->i_state |= I_FREEING;  // 设置 inode 状态为 I_FREEING
	inodes_stat.nr_inodes--;  // 更新 inode 统计数量
	spin_unlock(&inode_lock);  // 解锁 inode 锁

	security_inode_delete(inode);  // 执行安全模块的 inode 删除操作

	if (op->delete_inode) {
		void (*delete)(struct inode *) = op->delete_inode;
		/* Filesystems implementing their own
		 * s_op->delete_inode are required to call
		 * truncate_inode_pages and clear_inode()
		 * internally */
		// 如果文件系统实现了自己的 delete_inode，它需要内部调用 truncate_inode_pages 和 clear_inode()
		delete(inode);
	} else {
		truncate_inode_pages(&inode->i_data, 0);  // 截断 inode 页面
		clear_inode(inode);  // 清除 inode
	}
	spin_lock(&inode_lock);  // 重新加锁 inode 锁
	hlist_del_init(&inode->i_hash);  // 从哈希表中移除并初始化 inode 哈希节点
	spin_unlock(&inode_lock);  // 再次解锁 inode 锁
	wake_up_inode(inode);  // 唤醒等待此 inode 的进程
	BUG_ON(inode->i_state != I_CLEAR);  // 如果 inode 状态不是 I_CLEAR，则触发 bug
	destroy_inode(inode);  // 销毁 inode
}
EXPORT_SYMBOL(generic_delete_inode);

/**
 *	generic_detach_inode - remove inode from inode lists
 *	@inode: inode to remove
 *
 *	Remove inode from inode lists, write it if it's dirty. This is just an
 *	internal VFS helper exported for hugetlbfs. Do not use!
 *
 *	Returns 1 if inode should be completely destroyed.
 */
/**
 * generic_detach_inode - 从 inode 列表中移除 inode
 * @inode: 要移除的 inode
 *
 * 从 inode 列表中移除 inode，如果它是脏的则写入磁盘。这只是一个内部 VFS 帮助函数，
 * 为 hugetlbfs 导出。不要使用！
 *
 * 如果 inode 应该被完全销毁，返回 1。
 */
int generic_detach_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;  // 获取 inode 所属的超级块

	if (!hlist_unhashed(&inode->i_hash)) {  // 如果 inode 还在哈希表中
		if (!(inode->i_state & (I_DIRTY|I_SYNC)))  // 如果 inode 不是脏的或不需要同步
			list_move(&inode->i_list, &inode_unused);  // 将 inode 移动到未使用列表
		inodes_stat.nr_unused++;  // 递增未使用的 inode 计数

		if (sb->s_flags & MS_ACTIVE) {  // 如果超级块仍然活跃
			spin_unlock(&inode_lock);  // 解锁
			return 0;  // 返回 0 表示不需要销毁
		}

		WARN_ON(inode->i_state & I_NEW);  // 警告：如果 inode 状态为新创建
		inode->i_state |= I_WILL_FREE;  // 标记 inode 即将释放
		spin_unlock(&inode_lock);  // 解锁
		write_inode_now(inode, 1);  // 立即写入 inode
		spin_lock(&inode_lock);  // 重新加锁
		WARN_ON(inode->i_state & I_NEW);  // 再次警告：如果 inode 状态为新创建
		inode->i_state &= ~I_WILL_FREE;  // 清除即将释放的标记
		inodes_stat.nr_unused--;  // 减少未使用的 inode 计数
		hlist_del_init(&inode->i_hash);  // 从哈希表中删除 inode
	}
	list_del_init(&inode->i_list);  // 从 inode 列表中删除 inode
	list_del_init(&inode->i_sb_list);  // 从超级块的 inode 列表中删除 inode
	WARN_ON(inode->i_state & I_NEW);  // 警告：如果 inode 状态为新创建
	inode->i_state |= I_FREEING;  // 标记 inode 为正在释放
	inodes_stat.nr_inodes--;  // 减少 inode 总数
	spin_unlock(&inode_lock);  // 解锁
	return 1;  // 返回 1 表示 inode 应该被完全销毁
}
EXPORT_SYMBOL_GPL(generic_detach_inode);

/**
 * generic_forget_inode - 忘记 inode
 * @inode: 要处理的 inode
 *
 * 如果 generic_detach_inode 返回非零值（表示 inode 需要完全销毁），
 * 则进行清理操作，包括截断 inode 页面、清理 inode 结构，并唤醒等待该 inode 的进程，
 * 最终销毁 inode。这通常在 inode 的引用计数降到零时调用。
 */
static void generic_forget_inode(struct inode *inode)
{
	if (!generic_detach_inode(inode))  // 尝试分离 inode，如果分离失败（即返回0），则直接返回
		return;

	if (inode->i_data.nrpages)  // 如果 inode 关联了页面
		truncate_inode_pages(&inode->i_data, 0);  // 截断这些页面，即删除所有页面

	clear_inode(inode);  // 清理 inode，为释放操作做准备
	wake_up_inode(inode);  // 唤醒所有等待此 inode 的进程
	destroy_inode(inode);  // 销毁 inode，释放其占用的资源
}

/*
 * Normal UNIX filesystem behaviour: delete the
 * inode when the usage count drops to zero, and
 * i_nlink is zero.
 */
/*
 * generic_drop_inode - 处理 inode 的删除逻辑
 * @inode: 要处理的 inode
 *
 * 常规的 UNIX 文件系统行为：当 inode 的使用计数降为零，且 i_nlink 为零时，
 * 删除该 inode。
 */
void generic_drop_inode(struct inode *inode)
{
	if (!inode->i_nlink)  // 如果 inode 的链接计数为零
		generic_delete_inode(inode);  // 调用 generic_delete_inode 删除 inode
	else
		generic_forget_inode(inode);  // 否则调用 generic_forget_inode 忘记（清理） inode
}
EXPORT_SYMBOL_GPL(generic_drop_inode);

/*
 * Called when we're dropping the last reference
 * to an inode.
 *
 * Call the FS "drop()" function, defaulting to
 * the legacy UNIX filesystem behaviour..
 *
 * NOTE! NOTE! NOTE! We're called with the inode lock
 * held, and the drop function is supposed to release
 * the lock!
 */
/*
 * iput_final - 在释放对 inode 的最后一个引用时调用
 * @inode: 要处理的 inode
 *
 * 调用文件系统的 "drop()" 函数，该函数默认实现为传统的 UNIX 文件系统行为。
 *
 * 注意！注意！注意！在调用时 inode 锁已被持有，
 * 并且 drop 函数应当负责释放这个锁！
 */
static inline void iput_final(struct inode *inode)
{
	const struct super_operations *op = inode->i_sb->s_op; // 获取与 inode 关联的超级块的操作
	void (*drop)(struct inode *) = generic_drop_inode; // 默认的 drop 函数

	if (op && op->drop_inode)  // 如果文件系统定义了自己的 drop_inode 函数
		drop = op->drop_inode; // 使用文件系统提供的 drop_inode 函数
	drop(inode); // 调用 drop 函数处理 inode
}

/**
 *	iput	- put an inode
 *	@inode: inode to put
 *
 *	Puts an inode, dropping its usage count. If the inode use count hits
 *	zero, the inode is then freed and may also be destroyed.
 *
 *	Consequently, iput() can sleep.
 */
/**
 * iput - 释放一个 inode
 * @inode: 要释放的 inode
 *
 * 释放一个 inode，减少其使用计数。如果 inode 的使用计数降至零，
 * 则释放 inode，并可能将其销毁。
 *
 * 因此，iput() 可能会导致睡眠。
 */
void iput(struct inode *inode)
{
	if (inode) {  // 如果 inode 不为 NULL
		BUG_ON(inode->i_state == I_CLEAR);  // 如果 inode 状态为 I_CLEAR，则触发一个 bug

		// 原子减少 inode 的引用计数，并尝试获取 inode 锁
		if (atomic_dec_and_lock(&inode->i_count, &inode_lock))
			iput_final(inode);  // 如果引用计数降至零，则调用 iput_final 处理 inode
	}
}
EXPORT_SYMBOL(iput);

/**
 *	bmap	- find a block number in a file
 *	@inode: inode of file
 *	@block: block to find
 *
 *	Returns the block number on the device holding the inode that
 *	is the disk block number for the block of the file requested.
 *	That is, asked for block 4 of inode 1 the function will return the
 *	disk block relative to the disk start that holds that block of the
 *	file.
 */
/**
 * bmap - 在文件中查找块号
 * @inode: 文件的 inode
 * @block: 要查找的块
 *
 * 返回设备上持有该 inode 的块号，即对于请求的文件块的磁盘块号。
 * 也就是说，如果询问 inode 1 的第 4 块，函数将返回保存该文件块的磁盘起始处的磁盘块号。
 */
sector_t bmap(struct inode *inode, sector_t block)
{
	sector_t res = 0;  // 初始化结果为 0
	if (inode->i_mapping->a_ops->bmap)  // 如果 inode 映射的地址操作中定义了 bmap 函数
		res = inode->i_mapping->a_ops->bmap(inode->i_mapping, block);  // 调用 bmap 函数获取磁盘块号
	return res;  // 返回获取的磁盘块号
}
EXPORT_SYMBOL(bmap);

/*
 * With relative atime, only update atime if the previous atime is
 * earlier than either the ctime or mtime or if at least a day has
 * passed since the last atime update.
 */
/**
 * relatime_need_update - 判断是否需要更新访问时间
 * @mnt: 文件系统挂载点
 * @inode: 文件的 inode
 * @now: 当前时间
 *
 * 在相对访问时间（relatime）模式下，只有在以下情况之一发生时才更新 atime：
 * - 上一个 atime 早于 ctime 或 mtime
 * - 自上次 atime 更新以来至少过去了一天
 */
static int relatime_need_update(struct vfsmount *mnt, struct inode *inode,
                                struct timespec now)
{
	if (!(mnt->mnt_flags & MNT_RELATIME))
		return 1;  // 如果没有启用 relatime，总是更新 atime

	/*
	 * Is mtime younger than atime? If yes, update atime:
	 */
  /*
   * 检查修改时间（mtime）是否比访问时间（atime）更新。
   * 如果是，更新 atime：
   */
	if (timespec_compare(&inode->i_mtime, &inode->i_atime) >= 0)
		return 1;  // 如果 mtime 晚于或等于 atime，需要更新 atime

	/*
	 * Is ctime younger than atime? If yes, update atime:
	 */
  /*
   * 检查状态改变时间（ctime）是否比访问时间（atime）更新。
   * 如果是，更新 atime：
   */
	if (timespec_compare(&inode->i_ctime, &inode->i_atime) >= 0)
		return 1;  // 如果 ctime 晚于或等于 atime，需要更新 atime

	/*
	 * Is the previous atime value older than a day? If yes,
	 * update atime:
	 */
  /*
   * 检查上一次的访问时间（atime）是否早于一天前。
   * 如果是，更新 atime：
   */
	if ((long)(now.tv_sec - inode->i_atime.tv_sec) >= 24*60*60)
		return 1;  // 如果自上次 atime 更新以来已经过去了至少一天，需要更新 atime

	/*
	 * Good, we can skip the atime update:
	 */
  /*
   * 如果以上条件都不满足，可以跳过 atime 更新：
   */
	return 0;  // 不需要更新 atime
}

/**
 *	touch_atime	-	update the access time
 *	@mnt: mount the inode is accessed on
 *	@dentry: dentry accessed
 *
 *	Update the accessed time on an inode and mark it for writeback.
 *	This function automatically handles read only file systems and media,
 *	as well as the "noatime" flag and inode specific "noatime" markers.
 */
/**
 * touch_atime - 更新访问时间
 * @mnt: inode 所在的挂载点
 * @dentry: 被访问的目录项
 *
 * 更新 inode 的访问时间并标记为待写回。
 * 此函数自动处理只读文件系统和媒体，以及“noatime”标志和特定于 inode 的“noatime”设置。
 */
void touch_atime(struct vfsmount *mnt, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;  // 从目录项获取 inode
	struct timespec now;

	// 检查是否设置了不更新访问时间的标志
	if (inode->i_flags & S_NOATIME)
		return;
	if (IS_NOATIME(inode))
		return;
	if ((inode->i_sb->s_flags & MS_NODIRATIME) && S_ISDIR(inode->i_mode))
		return;

	// 检查挂载点是否设置了不更新访问时间的标志
	if (mnt->mnt_flags & MNT_NOATIME)
		return;
	if ((mnt->mnt_flags & MNT_NODIRATIME) && S_ISDIR(inode->i_mode))
		return;

	now = current_fs_time(inode->i_sb);  // 获取当前文件系统时间

	// 检查是否需要更新访问时间
	if (!relatime_need_update(mnt, inode, now))
		return;

	// 检查 inode 的访问时间是否已经是最新
	if (timespec_equal(&inode->i_atime, &now))
		return;

	// 检查是否有权限写入此挂载点
	if (mnt_want_write(mnt))
		return;

	inode->i_atime = now;  // 更新访问时间
	mark_inode_dirty_sync(inode);  // 标记 inode 为脏，需要同步到磁盘
	mnt_drop_write(mnt);  // 释放写权限
}
EXPORT_SYMBOL(touch_atime);

/**
 *	file_update_time	-	update mtime and ctime time
 *	@file: file accessed
 *
 *	Update the mtime and ctime members of an inode and mark the inode
 *	for writeback.  Note that this function is meant exclusively for
 *	usage in the file write path of filesystems, and filesystems may
 *	choose to explicitly ignore update via this function with the
 *	S_NOCMTIME inode flag, e.g. for network filesystem where these
 *	timestamps are handled by the server.
 */
/**
 * file_update_time - 更新 mtime 和 ctime
 * @file: 被访问的文件
 *
 * 更新 inode 的 mtime 和 ctime 字段，并将 inode 标记为待写回。
 * 注意，此函数专门用于文件系统的文件写路径中，文件系统可以选择通过
 * S_NOCMTIME inode 标志显式忽略通过此函数的更新，例如，在网络文件系统中，
 * 这些时间戳由服务器处理。
 */
void file_update_time(struct file *file)
{
	struct inode *inode = file->f_path.dentry->d_inode;  // 从文件中获取 inode
	struct timespec now;  // 用于存储当前时间
	enum { S_MTIME = 1, S_CTIME = 2, S_VERSION = 4 } sync_it = 0;  // 定义标志位以决定同步哪些字段

	/* First try to exhaust all avenues to not sync */
	/* 首先尝试避免同步 */
	if (IS_NOCMTIME(inode))
		return;  // 如果 inode 设置了不更新 ctime/mtime 标志，则直接返回

	now = current_fs_time(inode->i_sb);  // 获取当前文件系统时间
	/* 检查是否需要更新 mtime */
	if (!timespec_equal(&inode->i_mtime, &now))
		sync_it = S_MTIME;

	/* 检查是否需要更新 ctime */
	if (!timespec_equal(&inode->i_ctime, &now))
		sync_it |= S_CTIME;

	/* 检查是否需要更新 inode 的版本号 */
	if (IS_I_VERSION(inode))
		sync_it |= S_VERSION;

	/* 如果没有需要更新的字段，直接返回 */
	if (!sync_it)
		return;

	/* Finally allowed to write? Takes lock. */
	/* 获取写权限，如果失败则返回 */
	if (mnt_want_write_file(file))
		return;

	/* Only change inode inside the lock region */
	/* 只在持锁区域内更改 inode */
	if (sync_it & S_VERSION)
		inode_inc_iversion(inode);  // 增加 inode 的版本号
	if (sync_it & S_CTIME)
		inode->i_ctime = now;  // 更新 ctime
	if (sync_it & S_MTIME)
		inode->i_mtime = now;  // 更新 mtime
	mark_inode_dirty_sync(inode);  // 标记 inode 为脏，需要同步
	mnt_drop_write(file->f_path.mnt);  // 释放写权限
}
EXPORT_SYMBOL(file_update_time);

/**
 * inode_needs_sync - 判断 inode 是否需要同步
 * @inode: 要检查的 inode
 *
 * 判断给定的 inode 是否需要立即同步到磁盘。
 * 如果 inode 被标记为同步（IS_SYNC），或者如果 inode 是一个目录且设置了目录同步标志（IS_DIRSYNC），
 * 则返回 1 表示需要同步，否则返回 0。
 */
int inode_needs_sync(struct inode *inode)
{
	if (IS_SYNC(inode))  // 如果 inode 设置了同步标志
		return 1;
	if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode))  // 如果 inode 是一个目录且设置了目录同步标志
		return 1;
	return 0;  // 如果上述条件都不满足，则无需同步
}
EXPORT_SYMBOL(inode_needs_sync);

/**
 * inode_wait - 等待某个条件
 * @word: 等待的条件变量
 *
 * 当文件系统或内核需要等待某些条件（例如等待 I/O 操作完成）时调用。
 * 这个函数简单地调用 schedule() 来让出 CPU，等待调度器再次调度当前进程。
 * 返回 0 表示等待完成。
 */
int inode_wait(void *word)
{
	schedule();  // 让出 CPU，使其他进程可以运行
	return 0;  // 返回 0 表示等待完成
}
EXPORT_SYMBOL(inode_wait);

/*
 * If we try to find an inode in the inode hash while it is being
 * deleted, we have to wait until the filesystem completes its
 * deletion before reporting that it isn't found.  This function waits
 * until the deletion _might_ have completed.  Callers are responsible
 * to recheck inode state.
 *
 * It doesn't matter if I_NEW is not set initially, a call to
 * wake_up_inode() after removing from the hash list will DTRT.
 *
 * This is called with inode_lock held.
 */
/**
 * __wait_on_freeing_inode - 在 inode 正在被删除时等待
 * @inode: 正在等待的 inode
 *
 * 如果我们试图在 inode 正在从哈希表中被删除时找到它，我们必须等待文件系统完成删除操作，
 * 然后才报告它未被找到。此函数等待直到删除“可能”已经完成。调用者负责重新检查 inode 状态。
 *
 * 如果一开始 I_NEW 没有被设置，调用 wake_up_inode() 后从哈希表中移除将做正确的事情。
 *
 * 调用此函数时，inode_lock 被持有。
 */
static void __wait_on_freeing_inode(struct inode *inode)
{
	wait_queue_head_t *wq;
	DEFINE_WAIT_BIT(wait, &inode->i_state, __I_NEW);  // 定义一个等待位，关联到 inode 的状态中的 I_NEW 位
	wq = bit_waitqueue(&inode->i_state, __I_NEW);  // 获取与 I_NEW 位相关的等待队列头
	prepare_to_wait(wq, &wait.wait, TASK_UNINTERRUPTIBLE);  // 准备等待，设置进程为不可中断的等待状态
	spin_unlock(&inode_lock);  // 释放 inode 锁，允许其他进程操作
	schedule();  // 调度其他进程运行，让出 CPU
	finish_wait(wq, &wait.wait);  // 完成等待，从等待队列中移除
	spin_lock(&inode_lock);  // 重新获取 inode 锁
}

/* 定义一个静态的、仅在初始化时使用的变量 ihash_entries，用于存储 inode 哈希表的大小 */
static __initdata unsigned long ihash_entries;
/**
 * set_ihash_entries - 设置 inode 哈希表的大小
 * @str: 从内核启动参数中传入的字符串，包含哈希表的大小
 *
 * 解析内核启动参数中的 ihash_entries 值，并设置 inode 哈希表的大小。
 * 如果没有提供这个参数，则返回 0。
 *
 * 返回值:
 * 如果成功解析并设置了 ihash_entries，返回 1；
 * 如果 str 为空，返回 0，表示没有设置哈希表大小。
 */
static int __init set_ihash_entries(char *str)
{
	if (!str)
		return 0;  // 如果传入的字符串为空，直接返回 0

	ihash_entries = simple_strtoul(str, &str, 0);  // 使用 simple_strtoul 函数从字符串中解析无符号长整型数
	return 1;  // 解析成功，返回 1
}

/* 使用 __setup 宏注册 set_ihash_entries 函数，以便在内核启动时处理 "ihash_entries=" 启动参数 */
__setup("ihash_entries=", set_ihash_entries);

/*
 * Initialize the waitqueues and inode hash table.
 */
/**
 * inode_init_early - 初始化等待队列和 inode 哈希表
 *
 * 初始化过程中设置 inode 哈希表，如果哈希分布在 NUMA 节点上，推迟哈希分配
 * 直到 vmalloc 空间可用。
 */
void __init inode_init_early(void)
{
	int loop;

	/* If hashes are distributed across NUMA nodes, defer
	 * hash allocation until vmalloc space is available.
	 */
	/* 如果哈希在 NUMA 节点上分布，则推迟哈希表分配，直到 vmalloc 空间可用。 */
	if (hashdist)
		return;

	// 为了提高查找效率，将正在使用的和脏的 inode 放入一个哈希表中，
	// 不同的 inode 的哈希值可能相等，hash 值相等的 inode 通过 i_hash 成员连接。
	inode_hashtable =
		alloc_large_system_hash("Inode-cache",
					sizeof(struct hlist_head),
					ihash_entries,
					14,
					HASH_EARLY,
					&i_hash_shift,
					&i_hash_mask,
					0);

	// 初始化哈希表中的每个链表头
	for (loop = 0; loop < (1 << i_hash_shift); loop++)
		INIT_HLIST_HEAD(&inode_hashtable[loop]);
}

/**
 * inode_init - 初始化 inode 相关的数据结构
 *
 * 这个函数在系统启动时被调用，用于初始化 inode 的缓存和哈希表。
 */
void __init inode_init(void)
{
	int loop;

	/* inode slab cache */
	/* inode slab 缓存 */
	inode_cachep = kmem_cache_create("inode_cache",
					 sizeof(struct inode),
					 0,
					 (SLAB_RECLAIM_ACCOUNT|SLAB_PANIC|
					 SLAB_MEM_SPREAD),
					 init_once);
	/* 注册一个缩减器，用于在内存压力时减少缓存的使用 */
	register_shrinker(&icache_shrinker);

	/* Hash may have been set up in inode_init_early */
	/* 哈希表可能已在 inode_init_early 中设置 */
	if (!hashdist)
		return;

	/* 如果没有在 inode_init_early 中设置，这里进行哈希表的分配 */
	inode_hashtable =
		alloc_large_system_hash("Inode-cache",
					sizeof(struct hlist_head),
					ihash_entries,
					14,
					0,
					&i_hash_shift,
					&i_hash_mask,
					0);

	/* 初始化哈希表中的每个链表头 */
	for (loop = 0; loop < (1 << i_hash_shift); loop++)
		INIT_HLIST_HEAD(&inode_hashtable[loop]);
}

/**
 * init_special_inode - 初始化特殊类型的 inode
 * @inode: 要初始化的 inode
 * @mode: inode 的模式（文件类型和权限）
 * @rdev: 设备的设备号
 *
 * 根据指定的模式初始化特殊类型的 inode，为其设置文件操作指针和设备号。
 * 支持字符设备、块设备、FIFO 和套接字。
 */
void init_special_inode(struct inode *inode, umode_t mode, dev_t rdev)
{
	inode->i_mode = mode;  // 设置 inode 的模式
	if (S_ISCHR(mode)) {  // 如果是字符设备
		inode->i_fop = &def_chr_fops;  // 设置默认的字符设备操作
		inode->i_rdev = rdev;  // 设置设备号
	} else if (S_ISBLK(mode)) {  // 如果是块设备
		inode->i_fop = &def_blk_fops;  // 设置默认的块设备操作
		inode->i_rdev = rdev;  // 设置设备号
	} else if (S_ISFIFO(mode)) {  // 如果是 FIFO
		inode->i_fop = &def_fifo_fops;  // 设置默认的 FIFO 操作
	} else if (S_ISSOCK(mode)) {  // 如果是套接字
		inode->i_fop = &bad_sock_fops;  // 设置不支持的套接字操作
	} else {
		printk(KERN_DEBUG "init_special_inode: bogus i_mode (%o) for"
					" inode %s:%lu\n", mode, inode->i_sb->s_id,
					inode->i_ino);  // 如果模式无效，打印调试信息
	}
}
EXPORT_SYMBOL(init_special_inode);
