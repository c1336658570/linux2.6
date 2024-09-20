/*
 * fs/dcache.c
 *
 * Complete reimplementation
 * (C) 1997 Thomas Schoebel-Theuer,
 * with heavy changes by Linus Torvalds
 */

/*
 * Notes on the allocation strategy:
 *
 * The dcache is a master of the icache - whenever a dcache entry
 * exists, the inode will always exist. "iput()" is done either when
 * the dcache entry is deleted or garbage collected.
 */

#include <linux/syscalls.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/fsnotify.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/hash.h>
#include <linux/cache.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/file.h>
#include <asm/uaccess.h>
#include <linux/security.h>
#include <linux/seqlock.h>
#include <linux/swap.h>
#include <linux/bootmem.h>
#include <linux/fs_struct.h>
#include <linux/hardirq.h>
#include "internal.h"

/* 系统控制变量，用于调整虚拟文件系统缓存压力，默认值为100 */
int sysctl_vfs_cache_pressure __read_mostly = 100;
EXPORT_SYMBOL_GPL(sysctl_vfs_cache_pressure);

/* 在SMP（对称多处理）环境中内存对齐的目录项缓存锁 */
__cacheline_aligned_in_smp DEFINE_SPINLOCK(dcache_lock);
/* 在SMP环境中内存对齐的重命名序列锁 */
__cacheline_aligned_in_smp DEFINE_SEQLOCK(rename_lock);

EXPORT_SYMBOL(dcache_lock);

/* 主要用于目录项的内存缓存 */
static struct kmem_cache *dentry_cache __read_mostly;

/* 计算内嵌d_iname字段的最大长度，即dentry结构大小与d_iname偏移量的差 */
#define DNAME_INLINE_LEN (sizeof(struct dentry)-offsetof(struct dentry,d_iname))

/*
 * This is the single most critical data structure when it comes
 * to the dcache: the hashtable for lookups. Somebody should try
 * to make this good - I've just made it work.
 *
 * This hash-function tries to avoid losing too many bits of hash
 * information, yet avoid using a prime hash-size or similar.
 */
/* 这是dcache中最关键的数据结构：用于查找的哈希表。应该有人试图改进它——我只是让它能工作。
 * 这个哈希函数尝试避免丢失太多的哈希信息，同时避免使用质数哈希大小或类似的技术。
 */
#define D_HASHBITS     d_hash_shift  // 定义哈希位，用于调整哈希表大小
#define D_HASHMASK     d_hash_mask   // 定义哈希掩码，用于计算哈希值

/* d_hash_mask 和 d_hash_shift 用于配置目录项哈希表的大小和索引计算 */
static unsigned int d_hash_mask __read_mostly;
static unsigned int d_hash_shift __read_mostly;
// 哈希表维护在内存中的所有目录项，哈希表中每个元素都是一个双向循环链表，用于维护哈希值相等的目录项，
// 这个双向循环链表是通过dentry中的d_hash成员来链接在一起的。
static struct hlist_head *dentry_hashtable __read_mostly;

/* Statistics gathering. */
/* 收集统计信息 */
struct dentry_stat_t dentry_stat = {
	.age_limit = 45, // 目录项的最大年龄限制，用于缓存管理
};

/* 释放dentry时调用的内部函数 */
static void __d_free(struct dentry *dentry)
{
	WARN_ON(!list_empty(&dentry->d_alias)); // 如果dentry还有别名链表不为空，则发出警告
	if (dname_external(dentry))             // 如果dentry名称存储在外部
		kfree(dentry->d_name.name);         // 释放外部存储的名称
	kmem_cache_free(dentry_cache, dentry);  // 释放dentry对象至缓存
}

/* 用于RCU回调的函数，确保dentry在正确的上下文中被释放 */
static void d_callback(struct rcu_head *head)
{
	struct dentry * dentry = container_of(head, struct dentry, d_u.d_rcu);
	__d_free(dentry); // 调用__d_free释放dentry
}

/*
 * no dcache_lock, please.  The caller must decrement dentry_stat.nr_dentry
 * inside dcache_lock.
 */
/* 请不要使用dcache_lock。调用者必须在持有dcache_lock的情况下减少dentry_stat.nr_dentry的计数。 */
static void d_free(struct dentry *dentry)
{
	if (dentry->d_op && dentry->d_op->d_release)
		dentry->d_op->d_release(dentry); // 如果存在，调用dentry的释放操作
	/* if dentry was never inserted into hash, immediate free is OK */
	/* 如果目录项从未被插入到哈希表中，则可以立即释放 */
	if (hlist_unhashed(&dentry->d_hash))
		__d_free(dentry); // 如果目录项未被哈希，则直接调用__d_free进行释放
	else
		call_rcu(&dentry->d_u.d_rcu, d_callback); // 否则，通过RCU机制延迟释放
}

/*
 * Release the dentry's inode, using the filesystem
 * d_iput() operation if defined.
 */
/*
 * Release the dentry's inode, using the filesystem
 * d_iput() operation if defined.
 */
/* 释放dentry的inode，如果定义了，使用文件系统的d_iput()操作。 */
static void dentry_iput(struct dentry * dentry)
	__releases(dentry->d_lock)
	__releases(dcache_lock)
{
	struct inode *inode = dentry->d_inode; // 从dentry中获取inode指针
	if (inode) {
		dentry->d_inode = NULL; // 清除dentry中的inode指针
		list_del_init(&dentry->d_alias); // 从别名列表中删除dentry，并重新初始化列表
		spin_unlock(&dentry->d_lock); // 解锁dentry
		spin_unlock(&dcache_lock); // 解锁目录项缓存
		if (!inode->i_nlink)
			fsnotify_inoderemove(inode); // 如果inode的链接计数为0，通知文件系统inode被移除
		if (dentry->d_op && dentry->d_op->d_iput)
			dentry->d_op->d_iput(dentry, inode); // 如果定义了d_iput操作，调用它
		else
			iput(inode); // 否则，调用iput释放inode
	} else {
		spin_unlock(&dentry->d_lock); // 解锁dentry
		spin_unlock(&dcache_lock); // 解锁目录项缓存
	}
}


/*
 * dentry_lru_(add|add_tail|del|del_init) must be called with dcache_lock held.
 */
/* dentry_lru_(add|add_tail|del|del_init) 必须在持有dcache_lock的情况下调用。 */
static void dentry_lru_add(struct dentry *dentry)
{
	list_add(&dentry->d_lru, &dentry->d_sb->s_dentry_lru); // 将dentry添加到其超级块的LRU列表的头部
	dentry->d_sb->s_nr_dentry_unused++; // 增加超级块统计中未使用的dentry计数
	dentry_stat.nr_unused++; // 全局统计中未使用的dentry计数增加
}

static void dentry_lru_add_tail(struct dentry *dentry)
{
	list_add_tail(&dentry->d_lru, &dentry->d_sb->s_dentry_lru); // 将dentry添加到其超级块的LRU列表的尾部
	dentry->d_sb->s_nr_dentry_unused++; // 增加超级块统计中未使用的dentry计数
	dentry_stat.nr_unused++; // 全局统计中未使用的dentry计数增加
}

static void dentry_lru_del(struct dentry *dentry)
{
	if (!list_empty(&dentry->d_lru)) { // 检查LRU列表是否为空
		list_del(&dentry->d_lru); // 从LRU列表中移除dentry
		dentry->d_sb->s_nr_dentry_unused--; // 减少超级块统计中未使用的dentry计数
		dentry_stat.nr_unused--; // 全局统计中未使用的dentry计数减少
	}
}

static void dentry_lru_del_init(struct dentry *dentry)
{
	if (likely(!list_empty(&dentry->d_lru))) { // 如果LRU列表不为空
		list_del_init(&dentry->d_lru); // 从LRU列表中移除dentry并重新初始化d_lru
		dentry->d_sb->s_nr_dentry_unused--; // 减少超级块统计中未使用的dentry计数
		dentry_stat.nr_unused--; // 全局统计中未使用的dentry计数减少
	}
}

/**
 * d_kill - kill dentry and return parent
 * @dentry: dentry to kill
 *
 * The dentry must already be unhashed and removed from the LRU.
 *
 * If this is the root of the dentry tree, return NULL.
 */
/* d_kill - 销毁dentry并返回其父目录项
 * @dentry: 要销毁的dentry
 *
 * 该dentry必须已经从哈希表和LRU列表中移除。
 *
 * 如果这是目录树的根，则返回NULL。
 */
static struct dentry *d_kill(struct dentry *dentry)
	__releases(dentry->d_lock)
	__releases(dcache_lock)
{
	struct dentry *parent;

	/* For d_free, below */
	list_del(&dentry->d_u.d_child); // 从其父目录项的子列表中删除此dentry
	/*drops the locks, at that point nobody can reach this dentry */
	dentry_stat.nr_dentry--;	       // 全局目录项计数减少

	/* 在释放锁之后，没有人可以访问这个dentry */
	dentry_iput(dentry); // 释放与此dentry相关联的inode并执行清理工作
	if (IS_ROOT(dentry)) // 如果dentry是根目录项
		parent = NULL;
	else
		parent = dentry->d_parent; // 获取父目录项

	d_free(dentry); // 释放dentry
	return parent; // 返回父目录项
}

/* 
 * This is dput
 *
 * This is complicated by the fact that we do not want to put
 * dentries that are no longer on any hash chain on the unused
 * list: we'd much rather just get rid of them immediately.
 *
 * However, that implies that we have to traverse the dentry
 * tree upwards to the parents which might _also_ now be
 * scheduled for deletion (it may have been only waiting for
 * its last child to go away).
 *
 * This tail recursion is done by hand as we don't want to depend
 * on the compiler to always get this right (gcc generally doesn't).
 * Real recursion would eat up our stack space.
 */
/*
 * 这是 dput 函数的说明

 * 这个过程的复杂之处在于，我们不希望将不再位于任何哈希链上的目录项放到未使用的列表中：
 * 我们更愿意直接将它们立即删除。

 * 然而，这意味着我们必须向上遍历目录项树，到可能也正准备删除的父目录项（它可能只是在等待
 * 它的最后一个子项被删除）。

 * 这种尾递归是手动完成的，因为我们不想依赖编译器总能做对这件事（通常gcc做不到）。
 * 真正的递归会消耗我们的堆栈空间。
 */

/*
 * dput - release a dentry
 * @dentry: dentry to release 
 *
 * Release a dentry. This will drop the usage count and if appropriate
 * call the dentry unlink method as well as removing it from the queues and
 * releasing its resources. If the parent dentries were scheduled for release
 * they too may now get deleted.
 *
 * no dcache lock, please.
 */
/*
 * dput - 释放一个dentry
 * @dentry: 要释放的dentry
 *
 * 释放一个dentry。这将减少使用计数，并在适当的情况下调用dentry的解链方法，同时将其从队列中移除
 * 并释放其资源。如果父dentry也被计划释放，它们也可能现在被删除。
 *
 * 请不要持有dcache_lock。
 */
void dput(struct dentry *dentry)
{
	if (!dentry)
		return; // 如果dentry为空，直接返回

repeat:
	if (atomic_read(&dentry->d_count) == 1)
		might_sleep(); // 如果引用计数为1，可能会休眠
	if (!atomic_dec_and_lock(&dentry->d_count, &dcache_lock))
		return; // 原子地减少引用计数并尝试获取dcache_lock，如果不成功则返回

	spin_lock(&dentry->d_lock); // 获取dentry的自旋锁
	if (atomic_read(&dentry->d_count)) {
		spin_unlock(&dentry->d_lock); // 如果引用计数不为0，则释放锁并返回
		spin_unlock(&dcache_lock);
		return;
	}

	/*
	 * AV: ->d_delete() is _NOT_ allowed to block now.
	 */
	if (dentry->d_op && dentry->d_op->d_delete) {
		if (dentry->d_op->d_delete(dentry))
			goto unhash_it; // 如果定义了d_delete方法并返回true，进行unhash操作
	}
	/* Unreachable? Get rid of it */
	if (d_unhashed(dentry))
		goto kill_it; // 如果dentry已经未被哈希，准备销毁它
		if (list_empty(&dentry->d_lru)) {
			dentry->d_flags |= DCACHE_REFERENCED; // 标记dentry为已引用
			dentry_lru_add(dentry); // 将dentry添加到LRU列表
		}
	spin_unlock(&dentry->d_lock); // 释放dentry的自旋锁
	spin_unlock(&dcache_lock);
	return;

unhash_it:
	__d_drop(dentry); // 从哈希表中移除dentry
kill_it:
	/* if dentry was on the d_lru list delete it from there */
	dentry_lru_del(dentry); // 如果dentry在LRU列表中，从中删除
	dentry = d_kill(dentry); // 销毁dentry并返回其父dentry
	if (dentry)
		goto repeat; // 如果父dentry存在，重复这个过程
}
EXPORT_SYMBOL(dput);

/**
 * d_invalidate - invalidate a dentry
 * @dentry: dentry to invalidate
 *
 * Try to invalidate the dentry if it turns out to be
 * possible. If there are other dentries that can be
 * reached through this one we can't delete it and we
 * return -EBUSY. On success we return 0.
 *
 * no dcache lock.
 */
/* d_invalidate - 使一个目录项失效
 * @dentry: 要使失效的目录项
 *
 * 如果可能的话，尝试使目录项失效。如果通过这个目录项可以访问到其他目录项，
 * 我们不能删除它，并且会返回 -EBUSY。成功时返回 0。
 *
 * 请不要持有dcache锁。
 */
int d_invalidate(struct dentry * dentry)
{
	/*
	 * If it's already been dropped, return OK.
	 */
	/* 如果它已经被丢弃，返回OK。 */
	spin_lock(&dcache_lock);  // 获取dcache锁
	if (d_unhashed(dentry)) { // 检查目录项是否已从哈希表中移除
		spin_unlock(&dcache_lock);
		return 0;              // 如果已移除，返回0
	}
	/*
	 * Check whether to do a partial shrink_dcache
	 * to get rid of unused child entries.
	 */
	/* 检查是否进行部分收缩dcache，以摆脱未使用的子目录项。 */
	if (!list_empty(&dentry->d_subdirs)) { // 检查是否有子目录项
		spin_unlock(&dcache_lock);
		shrink_dcache_parent(dentry); // 收缩父目录项的子目录项缓存
		spin_lock(&dcache_lock);      // 重新获取dcache锁
	}

	/*
	 * Somebody else still using it?
	 *
	 * If it's a directory, we can't drop it
	 * for fear of somebody re-populating it
	 * with children (even though dropping it
	 * would make it unreachable from the root,
	 * we might still populate it if it was a
	 * working directory or similar).
	 */
	/* 还有其他人在使用它吗？
   *
   * 如果它是一个目录，我们不能删除它，
   * 因为担心有人会重新填充它的子目录
   * （尽管删除它会使它从根目录变得不可达，
   * 但如果它是一个工作目录或类似的东西，我们仍可能填充它）。
   */
	spin_lock(&dentry->d_lock);  // 获取目录项的锁
	if (atomic_read(&dentry->d_count) > 1) { // 检查是否还有其他引用
		if (dentry->d_inode && S_ISDIR(dentry->d_inode->i_mode)) {
			spin_unlock(&dentry->d_lock);	// 释放目录项锁
			spin_unlock(&dcache_lock);		// 释放dcache锁
			return -EBUSY;  // 如果是目录且有其他引用，返回-EBUSY
		}
	}

	__d_drop(dentry);  // 从哈希表中移除目录项
	spin_unlock(&dentry->d_lock); // 释放目录项的锁
	spin_unlock(&dcache_lock);    // 释放dcache锁
	return 0;
}
EXPORT_SYMBOL(d_invalidate);

/* This should be called _only_ with dcache_lock held */
/* 这个函数只能在持有dcache_lock的情况下调用 */
static inline struct dentry * __dget_locked(struct dentry *dentry)
{
	atomic_inc(&dentry->d_count); // 原子增加目录项的引用计数
	dentry_lru_del_init(dentry); // 从LRU列表中移除目录项，并重新初始化它
	return dentry; // 返回目录项
}

struct dentry * dget_locked(struct dentry *dentry)
{
	return __dget_locked(dentry); // 调用 __dget_locked 函数
}
EXPORT_SYMBOL(dget_locked);

/**
 * d_find_alias - grab a hashed alias of inode
 * @inode: inode in question
 * @want_discon:  flag, used by d_splice_alias, to request
 *          that only a DISCONNECTED alias be returned.
 *
 * If inode has a hashed alias, or is a directory and has any alias,
 * acquire the reference to alias and return it. Otherwise return NULL.
 * Notice that if inode is a directory there can be only one alias and
 * it can be unhashed only if it has no children, or if it is the root
 * of a filesystem.
 *
 * If the inode has an IS_ROOT, DCACHE_DISCONNECTED alias, then prefer
 * any other hashed alias over that one unless @want_discon is set,
 * in which case only return an IS_ROOT, DCACHE_DISCONNECTED alias.
 */
/* d_find_alias - 获取一个inode的哈希别名
 * @inode: 讨论中的inode
 * @want_discon: 标志，由d_splice_alias使用，请求只返回一个断开连接的别名。
 *
 * 如果inode有一个哈希别名，或是一个目录并且有任何别名，
 * 获取别名的引用并返回它。否则返回NULL。
 * 注意，如果inode是一个目录，那么只能有一个别名，
 * 并且只有在没有子节点或它是文件系统的根目录时，它才可能未被哈希。
 *
 * 如果inode有一个IS_ROOT, DCACHE_DISCONNECTED别名，
 * 除非设置了@want_discon，否则优先选择其他任何哈希别名，
 * 在设置了@want_discon的情况下，只返回一个IS_ROOT, DCACHE_DISCONNECTED别名。
 */

static struct dentry * __d_find_alias(struct inode *inode, int want_discon)
{
	struct list_head *head, *next, *tmp;
	struct dentry *alias, *discon_alias=NULL;

	head = &inode->i_dentry; // 指向inode的dentry链表头
	next = inode->i_dentry.next; // 指向链表的第一个元素
	while (next != head) { // 遍历链表
		tmp = next;
		next = tmp->next; // 移动到下一个元素
		prefetch(next); // 提前取出下一个元素，以提高访问效率
		alias = list_entry(tmp, struct dentry, d_alias); // 从链表元素获取dentry
		if (S_ISDIR(inode->i_mode) || !d_unhashed(alias)) { // 如果是目录或别名被哈希
			if (IS_ROOT(alias) &&
			    (alias->d_flags & DCACHE_DISCONNECTED)) // 如果是根且断开连接
				discon_alias = alias; // 记录断开连接的别名
			else if (!want_discon) {
				__dget_locked(alias); // 获取别名的引用
				return alias; // 返回找到的别名
			}
		}
	}
	if (discon_alias) // 如果存在断开连接的别名
		__dget_locked(discon_alias); // 获取其引用
	return discon_alias; // 返回断开连接的别名或NULL
}

struct dentry * d_find_alias(struct inode *inode)
{
	struct dentry *de = NULL; // 初始化目录项指针为 NULL

	/* 检查 inode 关联的 dentry 链表是否为空 */
	if (!list_empty(&inode->i_dentry)) { 
		spin_lock(&dcache_lock); // 获取 dcache 锁以保护对 dentry 链表的访问
		de = __d_find_alias(inode, 0); // 调用内部函数 __d_find_alias 来查找别名，不寻找断开连接的别名
		spin_unlock(&dcache_lock); // 释放 dcache 锁
	}
	return de; // 返回找到的目录项或 NULL
}
EXPORT_SYMBOL(d_find_alias);

/*
 *	Try to kill dentries associated with this inode.
 * WARNING: you must own a reference to inode.
 */
/* 
 * 尝试删除与此inode关联的dentries。
 * 警告：你必须拥有对inode的引用。
 */
void d_prune_aliases(struct inode *inode)
{
	struct dentry *dentry;
restart: // 标签用于重新开始过程
	spin_lock(&dcache_lock); // 获取dcache锁以保护对dentry列表的访问
	/* 遍历与inode关联的所有dentry */
	list_for_each_entry(dentry, &inode->i_dentry, d_alias) {
		spin_lock(&dentry->d_lock); // 获取每个dentry的锁
		/* 如果dentry没有其他引用 */
		if (!atomic_read(&dentry->d_count)) {
			__dget_locked(dentry); // 增加dentry的引用计数
			__d_drop(dentry); // 将dentry从哈希表中移除
			spin_unlock(&dentry->d_lock); // 释放dentry锁
			spin_unlock(&dcache_lock); // 释放dcache锁
			dput(dentry); // 减少引用计数，可能会导致dentry被释放
			goto restart; // 从头开始，因为列表可能已经改变
		}
		spin_unlock(&dentry->d_lock); // 如果dentry有其他引用，释放锁
	}
	spin_unlock(&dcache_lock); // 完成遍历后释放dcache锁
}
EXPORT_SYMBOL(d_prune_aliases);

/*
 * Throw away a dentry - free the inode, dput the parent.  This requires that
 * the LRU list has already been removed.
 *
 * Try to prune ancestors as well.  This is necessary to prevent
 * quadratic behavior of shrink_dcache_parent(), but is also expected
 * to be beneficial in reducing dentry cache fragmentation.
 */
/*
 * 丢弃一个目录项 - 释放inode，减少父目录项的引用。这要求LRU列表已经被移除。
 *
 * 同时尝试修剪祖先目录项。这是为了防止shrink_dcache_parent()的二次行为，
 * 也有助于减少目录项缓存的碎片化。
 */
static void prune_one_dentry(struct dentry * dentry)
	__releases(dentry->d_lock)
	__releases(dcache_lock)
	__acquires(dcache_lock)
{
	__d_drop(dentry);	// 从哈希表中移除目录项
	dentry = d_kill(dentry);	// 杀死目录项并返回其父目录项

	/*
	 * Prune ancestors.  Locking is simpler than in dput(),
	 * because dcache_lock needs to be taken anyway.
	 */
	/*
	 * 修剪祖先目录项。加锁比在dput()中简单，
	 * 因为无论如何都需要获取dcache_lock。
	 */
	spin_lock(&dcache_lock); // 获取dcache_lock
	while (dentry) {
		if (!atomic_dec_and_lock(&dentry->d_count, &dentry->d_lock))
			return;  // 原子减少目录项的引用计数并尝试获取锁，如果失败则返回

		if (dentry->d_op && dentry->d_op->d_delete)
			dentry->d_op->d_delete(dentry);  // 如果定义了d_delete操作，调用之
		dentry_lru_del_init(dentry);  // 从LRU列表中删除并重新初始化目录项
		__d_drop(dentry);  // 从哈希表中移除目录项
		dentry = d_kill(dentry);  // 杀死目录项并获取其父目录项
		spin_lock(&dcache_lock);  // 重新获取dcache_lock
	}
}

/*
 * Shrink the dentry LRU on a given superblock.
 * @sb   : superblock to shrink dentry LRU.
 * @count: If count is NULL, we prune all dentries on superblock.
 * @flags: If flags is non-zero, we need to do special processing based on
 * which flags are set. This means we don't need to maintain multiple
 * similar copies of this loop.
 */
/*
 * 在给定的超级块上缩减目录项LRU。
 * @sb   : 要缩减目录项LRU的超级块。
 * @count: 如果count为NULL，我们会修剪超级块上的所有目录项。
 * @flags: 如果flags非零，我们需要根据设置的标志进行特殊处理。
 *         这意味着我们不需要维护这个循环的多个类似副本。
 */
static void __shrink_dcache_sb(struct super_block *sb, int *count, int flags)
{
	// 初始化一个用来存放标记为已引用的目录项的列表
	LIST_HEAD(referenced); // 用于保存有DCACHE_REFERENCED标志的目录项
	// 初始化一个临时列表用于处理
	LIST_HEAD(tmp);        // 临时列表，用于处理目录项
	struct dentry *dentry;
	int cnt = 0;

	BUG_ON(!sb); // 检查超级块是否为空，如果是则触发bug
	BUG_ON((flags & DCACHE_REFERENCED) && count == NULL); // 检查是否在设置了DCACHE_REFERENCED标志的情况下count为NULL
	spin_lock(&dcache_lock); // 加锁dcache_lock以保护目录项列表的修改
	if (count != NULL)
		/* called from prune_dcache() and shrink_dcache_parent() */
		/* 被prune_dcache()和shrink_dcache_parent()调用时 */
		cnt = *count; // 从count参数获取需要处理的目录项数量
restart:	// 重启标签，用于重新开始缩减过程
	if (count == NULL)
		list_splice_init(&sb->s_dentry_lru, &tmp);	 // 如果count为NULL，将整个LRU列表移到临时列表tmp
	else {
		while (!list_empty(&sb->s_dentry_lru)) {	// 如果LRU列表不为空，继续处理
			dentry = list_entry(sb->s_dentry_lru.prev,	// 获取列表中最后一个目录项
					struct dentry, d_lru);
			BUG_ON(dentry->d_sb != sb);	// 检查目录项是否属于正确的superblock

			spin_lock(&dentry->d_lock);	// 获取目录项锁
			/*
			 * If we are honouring the DCACHE_REFERENCED flag and
			 * the dentry has this flag set, don't free it. Clear
			 * the flag and put it back on the LRU.
			 */
			/*
       * 如果我们正在处理DCACHE_REFERENCED标志并且目录项设置了此标志，
       * 不释放它。清除标志并将其放回LRU列表。
       */
			if ((flags & DCACHE_REFERENCED)
				&& (dentry->d_flags & DCACHE_REFERENCED)) {
				dentry->d_flags &= ~DCACHE_REFERENCED;	// 清除DCACHE_REFERENCED标志
				list_move(&dentry->d_lru, &referenced);	// 将目录项移动到referenced列表
				spin_unlock(&dentry->d_lock);	// 解锁目录项
			} else {
				list_move_tail(&dentry->d_lru, &tmp);	// 移动到tmp列表
				spin_unlock(&dentry->d_lock);	// 解锁目录项
				cnt--;	// 减少计数
				if (!cnt)
					break;	// 如果计数器为零，跳出循环
			}
			cond_resched_lock(&dcache_lock);	// 可能让出CPU，以避免长时间占用锁
		}
	}
	while (!list_empty(&tmp)) {	// 循环直到临时列表为空
		dentry = list_entry(tmp.prev, struct dentry, d_lru);	// 从列表尾部获取一个目录项
		dentry_lru_del_init(dentry);	// 从LRU列表中删除目录项，并重新初始化列表节点
		spin_lock(&dentry->d_lock);		// 对目录项加锁
		/*
		 * We found an inuse dentry which was not removed from
		 * the LRU because of laziness during lookup.  Do not free
		 * it - just keep it off the LRU list.
		 */
		/*
		 * 我们找到一个正在使用的目录项，之前因为查找时的惰性没有从LRU列表中移除。
		 * 不释放它 - 只是将其保持在LRU列表之外。
		 */
		if (atomic_read(&dentry->d_count)) {	// 检查目录项是否仍在使用中
			spin_unlock(&dentry->d_lock);	// 解锁目录项
			continue;	// 如果在使用中，继续下一个
		}
		prune_one_dentry(dentry);	// 清理目录项
		/* dentry->d_lock was dropped in prune_one_dentry() */
		/* 在prune_one_dentry()中已经释放了dentry的锁 */
		cond_resched_lock(&dcache_lock);	// 条件性地让出CPU并重新检查锁
	}
	if (count == NULL && !list_empty(&sb->s_dentry_lru))
		goto restart;	// 如果count为空且s_dentry_lru非空，重新开始
	if (count != NULL)
		*count = cnt;	// 如果count非空，更新外部变量以反映处理的目录项数量
	if (!list_empty(&referenced))	// 如果有被引用过但现在不需要删除的目录项
		list_splice(&referenced, &sb->s_dentry_lru);	// 将这些目录项重新放回LRU列表
	spin_unlock(&dcache_lock);	// 释放dcache锁
}

/**
 * prune_dcache - shrink the dcache
 * @count: number of entries to try to free
 *
 * Shrink the dcache. This is done when we need more memory, or simply when we
 * need to unmount something (at which point we need to unuse all dentries).
 *
 * This function may fail to free any resources if all the dentries are in use.
 */
/*
 * prune_dcache - 缩减目录项缓存
 * @count: 尝试释放的条目数量
 *
 * 缩减目录项缓存。当我们需要更多内存或需要卸载某些内容时执行此操作
 * （此时我们需要停止使用所有目录项）。
 *
 * 如果所有目录项都在使用中，此函数可能无法释放任何资源。
 */
static void prune_dcache(int count)
{
	struct super_block *sb;	// 定义一个指向超级块结构的指针
	int w_count;	// 用于存储当前超级块需要处理的目录项数量
	int unused = dentry_stat.nr_unused; // 获取全局未使用目录项的数量
	int prune_ratio; // 定义一个缩减比例
	int pruned; // 已缩减的数量

	if (unused == 0 || count == 0)
		return; // 如果没有未使用的目录项或者没有需要释放的数量，直接返回
	spin_lock(&dcache_lock); // 加锁dcache_lock
restart:	// 标签，用于重新开始缩减过程
	if (count >= unused)
		prune_ratio = 1; // 如果需要释放的数量大于或等于未使用的数量，设置比例为1
	else
		prune_ratio = unused / count; // 否则，计算缩减比例
	spin_lock(&sb_lock); // 获取超级块列表的锁
	list_for_each_entry(sb, &super_blocks, s_list) { // 遍历所有超级块
		if (sb->s_nr_dentry_unused == 0)
			continue; // 如果当前超级块没有未使用的目录项，跳过该超级块
		sb->s_count++; // 增加超级块的引用计数

		/* Now, we reclaim unused dentrins with fairness.
		 * We reclaim them same percentage from each superblock.
		 * We calculate number of dentries to scan on this sb
		 * as follows, but the implementation is arranged to avoid
		 * overflows:
		 * number of dentries to scan on this sb =
		 * count * (number of dentries on this sb /
		 * number of dentries in the machine)
		 */
		/* 现在，我们以公平的方式回收未使用的目录项。
     * 我们从每个超级块中回收相同比例的目录项。
     * 我们计算这个超级块上需要扫描的目录项数量
     * 如下所示，但实现是为了避免溢出：
     * 需要扫描的目录项数量 =
     * count * (此超级块上的目录项数量 /
     * 机器中的总目录项数量)
     */
		spin_unlock(&sb_lock);	// 释放超级块列表的锁
		if (prune_ratio != 1)
			w_count = (sb->s_nr_dentry_unused / prune_ratio) + 1;	// 计算当前超级块需要处理的目录项数量，确保至少处理一个
		else
			w_count = sb->s_nr_dentry_unused;	// 如果比例为1，处理所有未使用的目录项
		pruned = w_count;	// 设置已经处理的目录项计数
		/*
		 * We need to be sure this filesystem isn't being unmounted,
		 * otherwise we could race with generic_shutdown_super(), and
		 * end up holding a reference to an inode while the filesystem
		 * is unmounted.  So we try to get s_umount, and make sure
		 * s_root isn't NULL.
		 */
		/*
		 * 我们需要确保这个文件系统没有正在被卸载，
		 * 否则我们可能与generic_shutdown_super()发生竞争，
		 * 并且最终在文件系统被卸载时仍持有对inode的引用。
		 * 因此我们尝试获取s_umount锁，并确保s_root不是NULL。
		 */
		if (down_read_trylock(&sb->s_umount)) {	// 尝试获取文件系统的卸载锁
			if ((sb->s_root != NULL) &&	// 确保文件系统的根目录项存在
			    (!list_empty(&sb->s_dentry_lru))) {	// 并且目录项LRU列表不为空
				spin_unlock(&dcache_lock);	// 释放dcache锁以避免死锁
				__shrink_dcache_sb(sb, &w_count,
						DCACHE_REFERENCED);	// 调用函数缩减超级块的目录项缓存
				pruned -= w_count;	// 更新已处理的数量
				spin_lock(&dcache_lock);	// 重新获得dcache锁
			}
			up_read(&sb->s_umount);	// 释放文件系统卸载读锁
		}
		spin_lock(&sb_lock);	// 重新获得超级块列表锁
		count -= pruned;	// 更新全局还需处理的目录项数量
		/*
		 * restart only when sb is no longer on the list and
		 * we have more work to do.
		 */
		/*
		 * 仅当超级块不再在列表上并且我们还有更多工作要做时重启。
		 */
		if (__put_super_and_need_restart(sb) && count > 0) {	// 如果需要重启并且还有目录项需要处理
			spin_unlock(&sb_lock);	// 释放超级块列表锁
			goto restart;	// 跳转到restart重新开始处理
		}
	}
	spin_unlock(&sb_lock);	// 释放超级块列表锁
	spin_unlock(&dcache_lock);	// 释放dcache锁
}

/**
 * shrink_dcache_sb - shrink dcache for a superblock
 * @sb: superblock
 *
 * Shrink the dcache for the specified super block. This
 * is used to free the dcache before unmounting a file
 * system
 */
/*
 * shrink_dcache_sb - 缩减指定超级块的目录项缓存
 * @sb: 超级块
 *
 * 为指定的超级块缩减目录项缓存。这用于在卸载文件系统之前释放目录项缓存。
 */
void shrink_dcache_sb(struct super_block * sb)
{
	__shrink_dcache_sb(sb, NULL, 0); // 调用 __shrink_dcache_sb 函数进行实际的缩减操作
}
EXPORT_SYMBOL(shrink_dcache_sb);

/*
 * destroy a single subtree of dentries for unmount
 * - see the comments on shrink_dcache_for_umount() for a description of the
 *   locking
 */
/*
 * 为卸载销毁一个目录项子树
 * - 有关锁定的描述，请参见shrink_dcache_for_umount()上的注释
 */
static void shrink_dcache_for_umount_subtree(struct dentry *dentry)
{
	struct dentry *parent;	// 定义一个指针，用于存储目录项的父目录项
	unsigned detached = 0;	// 计数器，用于追踪已经从系统中分离的目录项数量

	BUG_ON(!IS_ROOT(dentry));	// 确保传入的dentry是根目录项

	/* detach this root from the system */
	// 从系统中分离这个根目录项
	spin_lock(&dcache_lock); // 锁定dcache_lock，保护目录项缓存
	dentry_lru_del_init(dentry); // 从LRU列表中移除并初始化dentry
	__d_drop(dentry); // 从哈希表中删除dentry
	spin_unlock(&dcache_lock); // 解锁dcache_lock

	for (;;) {
		/* descend to the first leaf in the current subtree */
		// 下降到当前子树的第一个叶子节点
		while (!list_empty(&dentry->d_subdirs)) {
			struct dentry *loop;

			/* this is a branch with children - detach all of them
			 * from the system in one go */
			// 这是一个有子节点的分支 - 一次性从系统中分离所有子节点
			spin_lock(&dcache_lock);	// 再次锁定dcache_lock
			list_for_each_entry(loop, &dentry->d_subdirs,
					    d_u.d_child) {	// 遍历所有子目录项
				dentry_lru_del_init(loop);	// 从LRU列表中移除并初始化这些子目录项
				__d_drop(loop);	// 从哈希表中移除这些子目录项
				cond_resched_lock(&dcache_lock);	// 条件调度以防止长时间占用锁
			}
			spin_unlock(&dcache_lock);	// 解锁

			/* move to the first child */
			// 移动到第一个子目录项
			dentry = list_entry(dentry->d_subdirs.next,
					    struct dentry, d_u.d_child);	// 移动到第一个子目录项
		}

		/* consume the dentries from this leaf up through its parents
		 * until we find one with children or run out altogether */
		/*
		 * 从这个叶子节点开始，向上消费目录项直到我们找到一个有子节点的目录项
		 * 或者完全没有目录项为止
		 */
		do {
			struct inode *inode;	// 定义inode结构体指针，用于后续处理关联的inode

			if (atomic_read(&dentry->d_count) != 0) {	// 检查dentry是否还在使用中
				printk(KERN_ERR
				       "BUG: Dentry %p{i=%lx,n=%s}"
				       " still in use (%d)"
				       " [unmount of %s %s]\n",
				       dentry,
				       dentry->d_inode ?
				       dentry->d_inode->i_ino : 0UL,
				       dentry->d_name.name,
				       atomic_read(&dentry->d_count),
				       dentry->d_sb->s_type->name,
				       dentry->d_sb->s_id);
				BUG();	// 如果目录项还在使用中，则触发BUG并打印错误信息
			}

			if (IS_ROOT(dentry))
				parent = NULL;	// 如果dentry是根，则其父目录项为NULL
			else {
				parent = dentry->d_parent; // 获取当前目录项的父目录项
				atomic_dec(&parent->d_count); // 减少父目录项的引用计数
			}

			list_del(&dentry->d_u.d_child); // 从其父目录项的子目录项列表中删除当前目录项
			detached++; // 增加已处理的目录项计数

			inode = dentry->d_inode; // 获取dentry的inode
			if (inode) {
				dentry->d_inode = NULL; // 清除dentry的inode指针
				list_del_init(&dentry->d_alias); // 删除并初始化inode的别名链表
				if (dentry->d_op && dentry->d_op->d_iput)
					dentry->d_op->d_iput(dentry, inode); // 如果定义了d_iput操作，则调用之
				else
					iput(inode); // 否则调用iput释放inode
			}

			d_free(dentry); // 释放当前目录项

			/* finished when we fall off the top of the tree,
			 * otherwise we ascend to the parent and move to the
			 * next sibling if there is one */
			/*
			 * 当我们到达树的顶部时完成，
			 * 否则我们上升到父目录项并移动到
			 * 下一个兄弟目录项（如果有的话）
			 */
			if (!parent)
				goto out;	// 如果没有父目录项，退出循环

			dentry = parent;	// 将当前目录项更新为其父目录项

		} while (list_empty(&dentry->d_subdirs));	// 继续循环直到找到一个有子目录项的目录项

		dentry = list_entry(dentry->d_subdirs.next,
				    struct dentry, d_u.d_child);	// 移动到下一个子目录项
	}
out:
	/* several dentries were freed, need to correct nr_dentry */
	/* 释放了多个目录项，需要纠正nr_dentry */
	spin_lock(&dcache_lock); // 锁定dcache_lock
	dentry_stat.nr_dentry -= detached; // 更新全局目录项统计数据，减去已释放的目录项数量
	spin_unlock(&dcache_lock); // 解锁dcache_lock
}

/*
 * destroy the dentries attached to a superblock on unmounting
 * - we don't need to use dentry->d_lock, and only need dcache_lock when
 *   removing the dentry from the system lists and hashes because:
 *   - the superblock is detached from all mountings and open files, so the
 *     dentry trees will not be rearranged by the VFS
 *   - s_umount is write-locked, so the memory pressure shrinker will ignore
 *     any dentries belonging to this superblock that it comes across
 *   - the filesystem itself is no longer permitted to rearrange the dentries
 *     in this superblock
 */
/*
 * 在卸载时销毁附加到超级块的目录项
 * - 我们不需要使用dentry->d_lock，只在从系统列表和哈希中移除目录项时需要dcache_lock，因为：
 *   - 超级块已从所有挂载和打开的文件中分离，因此VFS不会重新排列目录项树
 *   - s_umount已写锁定，因此内存压力缩减器会忽略它遇到的属于此超级块的任何目录项
 *   - 文件系统本身不再允许重新排列此超级块中的目录项
 */
void shrink_dcache_for_umount(struct super_block *sb)
{
	struct dentry *dentry; // 定义目录项指针

	if (down_read_trylock(&sb->s_umount))
		BUG(); // 如果无法获取s_umount的读锁，则触发BUG

	dentry = sb->s_root; // 获取超级块的根目录项
	sb->s_root = NULL; // 将超级块的根目录项置为NULL
	atomic_dec(&dentry->d_count); // 减少根目录项的引用计数
	shrink_dcache_for_umount_subtree(dentry); // 对根目录项的子树进行缩减处理

	while (!hlist_empty(&sb->s_anon)) { // 当超级块的匿名目录项列表不为空时
		dentry = hlist_entry(sb->s_anon.first, struct dentry, d_hash); // 获取第一个匿名目录项
		shrink_dcache_for_umount_subtree(dentry); // 对该匿名目录项的子树进行缩减处理
	}
}

/*
 * Search for at least 1 mount point in the dentry's subdirs.
 * We descend to the next level whenever the d_subdirs
 * list is non-empty and continue searching.
 */
/*
 * 在目录项的子目录中搜索至少一个挂载点。
 * 当 d_subdirs 列表非空时，我们下降到下一级并继续搜索。
 */
 
/**
 * have_submounts - check for mounts over a dentry
 * @parent: dentry to check.
 *
 * Return true if the parent or its subdirectories contain
 * a mount point
 */
/*
 * have_submounts - 检查目录项上是否有挂载点
 * @parent: 要检查的目录项。
 *
 * 如果父目录项或其子目录包含挂载点则返回 true
 */
int have_submounts(struct dentry *parent)
{
	struct dentry *this_parent = parent; // 当前处理的目录项
	struct list_head *next;

	spin_lock(&dcache_lock); // 锁定目录项缓存
	if (d_mountpoint(parent))
		goto positive; // 如果当前目录项是一个挂载点，则直接返回 true
repeat:
	next = this_parent->d_subdirs.next; // 获取子目录列表的下一个元素
resume:
	while (next != &this_parent->d_subdirs) { // 遍历子目录列表
		struct list_head *tmp = next;
		struct dentry *dentry = list_entry(tmp, struct dentry, d_u.d_child); // 从列表元素中获取目录项
		next = tmp->next; // 移动到下一个元素
		/* Have we found a mount point ? */
		/* 我们找到一个挂载点了吗？ */
		if (d_mountpoint(dentry))
			goto positive; // 如果找到挂载点，则返回 true
		if (!list_empty(&dentry->d_subdirs)) { // 如果当前目录项有子目录
			this_parent = dentry; // 将当前目录项设置为这个有子目录的目录项
			goto repeat; // 从这个新的父目录项开始重复搜索
		}
	}
	/*
	 * All done at this level ... ascend and resume the search.
	 */
	/*
	 * 这一级搜索完成... 上升并继续搜索。
	 */
	if (this_parent != parent) { // 如果当前目录项不是最初的父目录项
		next = this_parent->d_u.d_child.next; // 获取下一个兄弟目录项
		this_parent = this_parent->d_parent; // 返回到父目录项
		goto resume; // 继续在父目录项级别搜索
	}
	spin_unlock(&dcache_lock); // 解锁目录项缓存
	return 0; /* No mount points found in tree */
positive:
	spin_unlock(&dcache_lock); // 解锁目录项缓存
	return 1; // 找到挂载点，返回 true
}
EXPORT_SYMBOL(have_submounts);

/*
 * Search the dentry child list for the specified parent,
 * and move any unused dentries to the end of the unused
 * list for prune_dcache(). We descend to the next level
 * whenever the d_subdirs list is non-empty and continue
 * searching.
 *
 * It returns zero iff there are no unused children,
 * otherwise  it returns the number of children moved to
 * the end of the unused list. This may not be the total
 * number of unused children, because select_parent can
 * drop the lock and return early due to latency
 * constraints.
 */
/*
 * 搜索指定父目录项的子目录项列表，
 * 并将所有未使用的目录项移动到未使用列表的末尾，供 prune_dcache() 使用。
 * 每当 d_subdirs 列表非空时，我们向下一级深入并继续搜索。
 *
 * 如果没有未使用的子目录项，返回零；
 * 否则，返回移动到未使用列表末尾的子目录项数量。
 * 这可能不是未使用子目录项的总数，因为 select_parent 可能会
 * 由于延迟限制提前释放锁并返回。
 */
static int select_parent(struct dentry * parent)
{
	struct dentry *this_parent = parent; // 当前处理的父目录项
	struct list_head *next;
	int found = 0; // 计数器，记录发现的未使用目录项数量

	spin_lock(&dcache_lock); // 加锁，保护目录项列表
repeat:
	next = this_parent->d_subdirs.next; // 获取子目录项列表的下一个元素
resume:
	while (next != &this_parent->d_subdirs) { // 遍历子目录项列表
		struct list_head *tmp = next;
		struct dentry *dentry = list_entry(tmp, struct dentry, d_u.d_child); // 从列表元素中获取目录项
		next = tmp->next; // 移动到下一个元素

		dentry_lru_del_init(dentry); // 从LRU列表中移除并初始化此目录项
		/* 
		 * move only zero ref count dentries to the end 
		 * of the unused list for prune_dcache
		 */
		/*
		 * 只将引用计数为零的目录项移动到
		 * 未使用列表的末尾，供 prune_dcache 使用
		 */
		if (!atomic_read(&dentry->d_count)) {
			dentry_lru_add_tail(dentry); // 将目录项添加到LRU列表的末尾
			found++; // 更新找到的未使用目录项的计数
		}

		/*
		 * We can return to the caller if we have found some (this
		 * ensures forward progress). We'll be coming back to find
		 * the rest.
		 */
		/*
		 * 如果我们找到一些未使用的目录项，可以返回给调用者
		 * （这确保了向前的进展）。我们将返回来找到其余的。
		 */
		if (found && need_resched())
			goto out; // 如果需要调度，跳到出口

		/*
		 * Descend a level if the d_subdirs list is non-empty.
		 */
		/*
		 * 如果 d_subdirs 列表非空，向下一级深入。
		 */
		if (!list_empty(&dentry->d_subdirs)) {
			this_parent = dentry; // 将当前目录项设置为这个有子目录的目录项
			goto repeat; // 从这个新的父目录项开始重复搜索
		}
	}
	/*
	 * All done at this level ... ascend and resume the search.
	 */
	/*
	 * 这一级搜索完成... 上升并继续搜索。
	 */
	if (this_parent != parent) {
		next = this_parent->d_u.d_child.next; // 获取下一个兄弟目录项
		this_parent = this_parent->d_parent; // 返回到父目录项
		goto resume; // 继续在父目录项级别搜索
	}
out:
	spin_unlock(&dcache_lock); // 解锁
	return found; // 返回找到的未使用目录项数量
}

/**
 * shrink_dcache_parent - prune dcache
 * @parent: parent of entries to prune
 *
 * Prune the dcache to remove unused children of the parent dentry.
 */
/*
 * shrink_dcache_parent - 修剪目录缓存
 * @parent: 需要修剪子目录项的父目录项
 *
 * 修剪目录缓存以移除父目录项的未使用子目录项。
 */
void shrink_dcache_parent(struct dentry * parent)
{
	struct super_block *sb = parent->d_sb; // 获取父目录项所属的超级块
	int found; // 用于记录select_parent函数找到的未使用目录项数量

	while ((found = select_parent(parent)) != 0) // 循环调用select_parent函数直到没有未使用的子目录项
		__shrink_dcache_sb(sb, &found, 0); // 调用__shrink_dcache_sb函数修剪找到的未使用目录项
}
EXPORT_SYMBOL(shrink_dcache_parent);

/*
 * Scan `nr' dentries and return the number which remain.
 *
 * We need to avoid reentering the filesystem if the caller is performing a
 * GFP_NOFS allocation attempt.  One example deadlock is:
 *
 * ext2_new_block->getblk->GFP->shrink_dcache_memory->prune_dcache->
 * prune_one_dentry->dput->dentry_iput->iput->inode->i_sb->s_op->put_inode->
 * ext2_discard_prealloc->ext2_free_blocks->lock_super->DEADLOCK.
 *
 * In this case we return -1 to tell the caller that we baled.
 */
/*
 * 扫描 `nr` 个目录项并返回剩余的数量。
 *
 * 如果调用者正在执行 GFP_NOFS 分配尝试，我们需要避免重新进入文件系统。
 * 一个死锁的例子是：
 *
 * ext2_new_block->getblk->GFP->shrink_dcache_memory->prune_dcache->
 * prune_one_dentry->dput->dentry_iput->iput->inode->i_sb->s_op->put_inode->
 * ext2_discard_prealloc->ext2_free_blocks->lock_super->DEADLOCK.
 *
 * 在这种情况下，我们返回 -1 告诉调用者我们退出了。
 */
static int shrink_dcache_memory(int nr, gfp_t gfp_mask)
{
	if (nr) {
		if (!(gfp_mask & __GFP_FS)) // 检查 GFP 标志是否允许文件系统重新进入
			return -1; // 如果不允许，则返回 -1 表示退出
		prune_dcache(nr); // 调用 prune_dcache 函数以尝试释放指定数量的目录项
	}
	return (dentry_stat.nr_unused / 100) * sysctl_vfs_cache_pressure; // 计算并返回基于系统 VFS 缓存压力调整的未使用目录项数量
}

static struct shrinker dcache_shrinker = {
	.shrink = shrink_dcache_memory, // 设置 shrink 函数为 shrink_dcache_memory
	.seeks = DEFAULT_SEEKS, // 设置 seeks 值为默认
};

/**
 * d_alloc	-	allocate a dcache entry
 * @parent: parent of entry to allocate
 * @name: qstr of the name
 *
 * Allocates a dentry. It returns %NULL if there is insufficient memory
 * available. On a success the dentry is returned. The name passed in is
 * copied and the copy passed in may be reused after this call.
 */
/*
 * d_alloc - 分配一个目录缓存条目
 * @parent: 要分配的条目的父条目
 * @name: 名称的 qstr 结构
 *
 * 分配一个目录项。如果内存不足则返回 %NULL。
 * 成功时返回目录项。传入的名称将被复制，复制后的名称可在此调用后重用。
 */
struct dentry *d_alloc(struct dentry * parent, const struct qstr *name)
{
	struct dentry *dentry;
	char *dname;

	dentry = kmem_cache_alloc(dentry_cache, GFP_KERNEL); // 从目录项缓存中分配一个目录项
	if (!dentry)
		return NULL; // 如果分配失败，返回 NULL

	if (name->len > DNAME_INLINE_LEN-1) {
		dname = kmalloc(name->len + 1, GFP_KERNEL); // 如果名称长度超过内联长度，分配新的内存
		if (!dname) {
			kmem_cache_free(dentry_cache, dentry); // 如果分配失败，释放之前分配的目录项并返回 NULL
			return NULL;
		}
	} else  {
		dname = dentry->d_iname; // 如果名称可以内联存储，直接使用目录项内部的数组
	}	
	dentry->d_name.name = dname; // 设置目录项的名称指针

	dentry->d_name.len = name->len; // 设置名称长度
	dentry->d_name.hash = name->hash; // 设置名称哈希值
	memcpy(dname, name->name, name->len); // 复制名称字符串
	dname[name->len] = 0; // 确保名称字符串以 NULL 结尾

	atomic_set(&dentry->d_count, 1); // 初始化目录项的引用计数为1
	dentry->d_flags = DCACHE_UNHASHED; // 设置目录项标志为未哈希
	spin_lock_init(&dentry->d_lock); // 初始化目录项的自旋锁
	dentry->d_inode = NULL; // 初始化inode指针为NULL
	dentry->d_parent = NULL; // 初始化父目录项为NULL
	dentry->d_sb = NULL; // 初始化所属超级块为NULL
	dentry->d_op = NULL; // 初始化目录项操作为NULL
	dentry->d_fsdata = NULL; // 初始化文件系统私有数据为NULL
	dentry->d_mounted = 0; // 初始化挂载点标志为0
	INIT_HLIST_NODE(&dentry->d_hash); // 初始化哈希节点
	INIT_LIST_HEAD(&dentry->d_lru); // 初始化LRU列表
	INIT_LIST_HEAD(&dentry->d_subdirs); // 初始化子目录列表
	INIT_LIST_HEAD(&dentry->d_alias); // 初始化别名列表

	if (parent) {
		dentry->d_parent = dget(parent); // 如果有父目录项，增加父目录项的引用计数
		dentry->d_sb = parent->d_sb; // 设置目录项所属的超级块
	} else {
		INIT_LIST_HEAD(&dentry->d_u.d_child); // 如果没有父目录项，初始化目录项的子节点列表
	}

	spin_lock(&dcache_lock); // 加锁
	if (parent)
		list_add(&dentry->d_u.d_child, &parent->d_subdirs); // 将新目录项添加到父目录项的子目录列表中
	dentry_stat.nr_dentry++; // 增加全局目录项计数
	spin_unlock(&dcache_lock); // 解锁

	return dentry; // 返回新分配的目录项
}
EXPORT_SYMBOL(d_alloc);

// 基于给定的名称字符串分配一个新的目录项（dentry）
struct dentry *d_alloc_name(struct dentry *parent, const char *name)
{
	struct qstr q;  // 定义一个 qstr 结构体，用于存储名称信息

	q.name = name;  // 设置 qstr 的 name 字段为传入的名称
	q.len = strlen(name);  // 计算并设置名称的长度
	q.hash = full_name_hash(q.name, q.len);  // 计算并设置名称的哈希值
	return d_alloc(parent, &q);  // 调用 d_alloc 函数，基于提供的父目录项和名称信息分配一个新的目录项
}
EXPORT_SYMBOL(d_alloc_name);

/* the caller must hold dcache_lock */
/* 调用者必须持有 dcache_lock */
// 是将一个 dentry（目录项）与一个 inode（索引节点）相关联
static void __d_instantiate(struct dentry *dentry, struct inode *inode)
{
	if (inode)
		list_add(&dentry->d_alias, &inode->i_dentry);  // 如果 inode 存在，将 dentry 添加到 inode 的别名列表中
	dentry->d_inode = inode;  // 将传入的 inode 设置为 dentry 的 inode 字段
	fsnotify_d_instantiate(dentry, inode);  // 通知文件系统相关事件，可能用于触发特定的钩子或更新
}

/**
 * d_instantiate - fill in inode information for a dentry
 * @entry: dentry to complete
 * @inode: inode to attach to this dentry
 *
 * Fill in inode information in the entry.
 *
 * This turns negative dentries into productive full members
 * of society.
 *
 * NOTE! This assumes that the inode count has been incremented
 * (or otherwise set) by the caller to indicate that it is now
 * in use by the dcache.
 */
/*
 * d_instantiate - 为目录项填充 inode 信息
 * @entry: 需要完成的目录项
 * @inode: 要附加到此目录项的 inode
 *
 * 在目录项中填充 inode 信息。
 *
 * 这将负目录项转变为社会的有效成员。
 *
 * 注意！这假设 inode 的计数已经被调用者增加
 * （或以其他方式设置）以表明它现在被 dcache 使用。
 */
// 负责将一个索引节点 (inode) 与一个目录项 (dentry) 相关联，使之成为一个完全有效的文件系统对象
void d_instantiate(struct dentry *entry, struct inode * inode)
{
	BUG_ON(!list_empty(&entry->d_alias)); // 断言：目录项的别名列表应为空，否则触发BUG
	spin_lock(&dcache_lock); // 加锁，保护目录项缓存
	__d_instantiate(entry, inode); // 调用内部函数 __d_instantiate 实际完成 inode 与 dentry 的关联
	spin_unlock(&dcache_lock); // 解锁
	security_d_instantiate(entry, inode); // 调用安全模块相关的处理函数，可能进行权限检查或记录
}
EXPORT_SYMBOL(d_instantiate);

/**
 * d_instantiate_unique - instantiate a non-aliased dentry
 * @entry: dentry to instantiate
 * @inode: inode to attach to this dentry
 *
 * Fill in inode information in the entry. On success, it returns NULL.
 * If an unhashed alias of "entry" already exists, then we return the
 * aliased dentry instead and drop one reference to inode.
 *
 * Note that in order to avoid conflicts with rename() etc, the caller
 * had better be holding the parent directory semaphore.
 *
 * This also assumes that the inode count has been incremented
 * (or otherwise set) by the caller to indicate that it is now
 * in use by the dcache.
 */
/*
 * d_instantiate_unique - 实例化一个无别名的目录项
 * @entry: 要实例化的目录项
 * @inode: 要附加到这个目录项的索引节点
 *
 * 在目录项中填充索引节点信息。成功时返回 NULL。
 * 如果“entry”的未哈希别名已经存在，那么返回别名目录项，
 * 并减少对 inode 的一个引用。
 *
 * 注意，为了避免与 rename() 等操作冲突，调用者最好持有父目录的信号量。
 *
 * 这同样假设 inode 的计数已由调用者增加（或以其他方式设置），
 * 以表明它现在被 dcache 使用。
 */
// 实例化一个不会产生别名冲突的目录项（dentry）。如果存在未哈希的别名（未加入哈希表的相同名称的其他目录项），则返回现有的别名目录项并减少一个对 inode 的引用。
static struct dentry *__d_instantiate_unique(struct dentry *entry,
					     struct inode *inode)
{
	struct dentry *alias;  // 用于迭代的别名目录项变量
	int len = entry->d_name.len;  // 目录项名称长度
	const char *name = entry->d_name.name;  // 目录项名称
	unsigned int hash = entry->d_name.hash;  // 目录项名称哈希值

	if (!inode) {
		__d_instantiate(entry, NULL);  // 如果 inode 为空，实例化目录项但不关联 inode
		return NULL;
	}

	list_for_each_entry(alias, &inode->i_dentry, d_alias) {  // 遍历 inode 的别名列表
		struct qstr *qstr = &alias->d_name;  // 获取当前别名目录项的名称结构

		if (qstr->hash != hash)  // 如果哈希值不匹配，跳过
			continue;
		if (alias->d_parent != entry->d_parent)  // 如果父目录项不匹配，跳过
			continue;
		if (qstr->len != len)  // 如果长度不匹配，跳过
			continue;
		if (memcmp(qstr->name, name, len))  // 如果名称不完全匹配，跳过
			continue;
		dget_locked(alias);  // 增加找到的目录项的引用计数
		return alias;  // 返回找到的匹配的别名目录项
	}

	__d_instantiate(entry, inode);  // 如果没有找到匹配的别名，正常实例化目录项
	return NULL;  // 返回 NULL 表示没有别名冲突
}

// 实例化一个目录项（dentry），同时确保不会创建任何不必要的别名。如果存在一个未哈希的别名，则该函数返回现有的别名目录项并释放传入的 inode。
struct dentry *d_instantiate_unique(struct dentry *entry, struct inode *inode)
{
	struct dentry *result;  // 用于存储函数返回结果的变量

	BUG_ON(!list_empty(&entry->d_alias)); // 断言：确保传入的目录项没有已经存在的别名，否则触发BUG

	spin_lock(&dcache_lock); // 加锁，保护目录项缓存操作
	result = __d_instantiate_unique(entry, inode); // 调用内部函数，检查是否存在未哈希的别名，并尝试实例化目录项
	spin_unlock(&dcache_lock); // 解锁

	if (!result) { // 如果没有返回现有的别名目录项（即成功实例化新目录项或更新现有目录项）
		security_d_instantiate(entry, inode); // 调用安全模块的实例化钩子，可能用于安全审计或其他处理
		return NULL; // 返回 NULL 表示成功实例化且没有现有别名
	}

	BUG_ON(!d_unhashed(result)); // 断言：确保返回的目录项是未加入哈希表的，否则触发BUG
	iput(inode); // 释放传入的 inode，因为已存在一个别名使用它
	return result; // 返回找到的别名目录项
}
EXPORT_SYMBOL(d_instantiate_unique);

/**
 * d_alloc_root - allocate root dentry
 * @root_inode: inode to allocate the root for
 *
 * Allocate a root ("/") dentry for the inode given. The inode is
 * instantiated and returned. %NULL is returned if there is insufficient
 * memory or the inode passed is %NULL.
 */
/*
 * d_alloc_root - 分配根目录项
 * @root_inode: 要为其分配根目录的 inode
 *
 * 为给定的 inode 分配一个根（"/"）目录项。这个 inode 会被实例化并返回。
 * 如果内存不足或传入的 inode 为 %NULL，则返回 %NULL。
 */
// 用于为给定的 inode 分配一个根目录项（"/"）。
struct dentry * d_alloc_root(struct inode * root_inode)
{
	struct dentry *res = NULL;  // 初始化结果为 NULL

	if (root_inode) {  // 如果传入的 inode 非空
		static const struct qstr name = { .name = "/", .len = 1 };  // 定义根目录项的名称为 "/"

		res = d_alloc(NULL, &name);  // 调用 d_alloc 分配一个根目录项，没有父目录
		if (res) {  // 如果分配成功
			res->d_sb = root_inode->i_sb;  // 设置目录项所属的超级块
			res->d_parent = res;  // 设置目录项的父目录项为其自身，因为它是根目录项
			d_instantiate(res, root_inode);  // 实例化目录项，关联它与传入的 inode
		}
	}
	return res;  // 返回分配的目录项，或在失败时返回 NULL
}
EXPORT_SYMBOL(d_alloc_root);

// 用于计算目录项的哈希值，从而将其正确地插入到目录项哈希表中。
static inline struct hlist_head *d_hash(struct dentry *parent,
					unsigned long hash)
{
	// 计算基于父目录项指针和传入的哈希值的新哈希值。
	// GOLDEN_RATIO_PRIME 是一个质数乘数，用于提高哈希散列的分布性质。
	hash += ((unsigned long) parent ^ GOLDEN_RATIO_PRIME) / L1_CACHE_BYTES;
	// 通过将哈希值与其自身右移 D_HASHBITS 位后的值进行异或操作，进一步混合和散列。
	hash = hash ^ ((hash ^ GOLDEN_RATIO_PRIME) >> D_HASHBITS);
	// 根据 D_HASHMASK 掩码，将计算出的哈希值限定在哈希表的大小范围内，
	// 并返回对应的哈希桶的头节点指针。
	return dentry_hashtable + (hash & D_HASHMASK);
}

/**
 * d_obtain_alias - find or allocate a dentry for a given inode
 * @inode: inode to allocate the dentry for
 *
 * Obtain a dentry for an inode resulting from NFS filehandle conversion or
 * similar open by handle operations.  The returned dentry may be anonymous,
 * or may have a full name (if the inode was already in the cache).
 *
 * When called on a directory inode, we must ensure that the inode only ever
 * has one dentry.  If a dentry is found, that is returned instead of
 * allocating a new one.
 *
 * On successful return, the reference to the inode has been transferred
 * to the dentry.  In case of an error the reference on the inode is released.
 * To make it easier to use in export operations a %NULL or IS_ERR inode may
 * be passed in and will be the error will be propagate to the return value,
 * with a %NULL @inode replaced by ERR_PTR(-ESTALE).
 */
/*
 * d_obtain_alias - 为给定 inode 查找或分配一个目录项
 * @inode: 要为其分配目录项的 inode
 *
 * 为来自 NFS 文件句柄转换或类似的通过句柄打开操作的 inode 获取一个目录项。
 * 返回的目录项可能是匿名的，或者如果 inode 已经在缓存中，则可能有完整名称。
 *
 * 当对目录 inode 调用时，必须确保 inode 只有一个目录项。
 * 如果找到了一个目录项，就返回它，而不是分配一个新的。
 *
 * 在成功返回时，对 inode 的引用已转移到目录项。
 * 如果出现错误，则释放对 inode 的引用。
 * 为了便于在导出操作中使用，可以传入一个 %NULL 或 IS_ERR 的 inode，
 * 错误将传播到返回值，@inode 为 %NULL 时将被替换为 ERR_PTR(-ESTALE)。
 */
// 用于获取或分配一个与给定 inode 关联的目录项（dentry）。这种场景常见于NFS文件句柄转换或类似的通过句柄打开操作。
struct dentry *d_obtain_alias(struct inode *inode)
{
	static const struct qstr anonstring = { .name = "" };  // 创建一个匿名 qstr 结构
	struct dentry *tmp;
	struct dentry *res;

	if (!inode)
		return ERR_PTR(-ESTALE);  // 如果 inode 为 NULL，返回错误
	if (IS_ERR(inode))
		return ERR_CAST(inode);  // 如果 inode 是错误指针，返回错误

	res = d_find_alias(inode);  // 尝试在缓存中找到一个已有的与 inode 关联的目录项
	if (res)
		goto out_iput;  // 如果找到，直接返回这个目录项

	tmp = d_alloc(NULL, &anonstring);  // 分配一个匿名目录项
	if (!tmp) {
		res = ERR_PTR(-ENOMEM);  // 如果分配失败，返回内存不足错误
		goto out_iput;
	}
	tmp->d_parent = tmp; // 确保 dput 操作不会出错

	/* make sure dput doesn't croak */
	spin_lock(&dcache_lock);  // 加锁
	res = __d_find_alias(inode, 0);  // 再次检查是否已存在与 inode 关联的目录项
	if (res) {
		spin_unlock(&dcache_lock);
		dput(tmp);  // 释放新分配的匿名目录项
		goto out_iput;
	}

	/* attach a disconnected dentry */
	// 为新目录项设置属性
	spin_lock(&tmp->d_lock);
	tmp->d_sb = inode->i_sb;  // 设置超级块
	tmp->d_inode = inode;  // 设置 inode
	tmp->d_flags |= DCACHE_DISCONNECTED;  // 设置为断开连接状态
	tmp->d_flags &= ~DCACHE_UNHASHED;  // 确保目录项不是未哈希的
	list_add(&tmp->d_alias, &inode->i_dentry);  // 将目录项添加到 inode 的别名列表
	hlist_add_head(&tmp->d_hash, &inode->i_sb->s_anon);  // 将目录项添加到哈希表
	spin_unlock(&tmp->d_lock);

	spin_unlock(&dcache_lock);  // 解锁
	return tmp;  // 返回新创建的目录项

 out_iput:
	iput(inode);  // 释放 inode 引用
	return res;  // 返回结果
}
EXPORT_SYMBOL(d_obtain_alias);

/**
 * d_splice_alias - splice a disconnected dentry into the tree if one exists
 * @inode:  the inode which may have a disconnected dentry
 * @dentry: a negative dentry which we want to point to the inode.
 *
 * If inode is a directory and has a 'disconnected' dentry (i.e. IS_ROOT and
 * DCACHE_DISCONNECTED), then d_move that in place of the given dentry
 * and return it, else simply d_add the inode to the dentry and return NULL.
 *
 * This is needed in the lookup routine of any filesystem that is exportable
 * (via knfsd) so that we can build dcache paths to directories effectively.
 *
 * If a dentry was found and moved, then it is returned.  Otherwise NULL
 * is returned.  This matches the expected return value of ->lookup.
 *
 */
/*
 * d_splice_alias - 如果存在，将一个断开连接的目录项接入到树中
 * @inode: 可能有一个断开连接的目录项的 inode
 * @dentry: 一个我们希望指向该 inode 的负目录项。
 *
 * 如果 inode 是一个目录并且拥有一个断开连接的目录项（即 IS_ROOT 和
 * DCACHE_DISCONNECTED），那么将其移动到给定的目录项的位置并返回它，
 * 否则只是简单地将 inode 添加到目录项并返回 NULL。
 *
 * 这在任何可导出的文件系统（通过 knfsd）的查找例程中是必需的，
 * 以便我们可以有效地构建到目录的 dcache 路径。
 *
 * 如果找到并移动了一个目录项，那么它将被返回。否则返回 NULL。
 * 这与 ->lookup 的预期返回值相匹配。
 */
// 将一个与特定 inode 关联的断开连接的目录项（dentry）接入到目录树中，如果存在的话
struct dentry *d_splice_alias(struct inode *inode, struct dentry *dentry)
{
	struct dentry *new = NULL;  // 初始化新目录项指针

	if (inode && S_ISDIR(inode->i_mode)) {  // 如果 inode 存在且是一个目录
		spin_lock(&dcache_lock);  // 加锁，保护目录项缓存
		new = __d_find_alias(inode, 1);  // 查找 inode 的断开连接的别名目录项
		if (new) {
			BUG_ON(!(new->d_flags & DCACHE_DISCONNECTED));  // 确保找到的目录项是断开连接的
			spin_unlock(&dcache_lock);  // 解锁
			security_d_instantiate(new, inode);  // 调用安全模块的实例化钩子
			d_move(new, dentry);  // 将找到的目录项移动到给定的目录项位置
			iput(inode);  // 释放 inode 引用
		} else {
			/* already taking dcache_lock, so d_add() by hand */
			__d_instantiate(dentry, inode);  // 手动实例化目录项
			spin_unlock(&dcache_lock);  // 解锁
			security_d_instantiate(dentry, inode);  // 调用安全模块的实例化钩子
			d_rehash(dentry);  // 重新哈希目录项
		}
	} else
		d_add(dentry, inode);  // 如果 inode 不是目录或不存在，则简单地添加目录项

	return new;  // 返回新的或移动的目录项
}
EXPORT_SYMBOL(d_splice_alias);

/**
 * d_add_ci - lookup or allocate new dentry with case-exact name
 * @inode:  the inode case-insensitive lookup has found
 * @dentry: the negative dentry that was passed to the parent's lookup func
 * @name:   the case-exact name to be associated with the returned dentry
 *
 * This is to avoid filling the dcache with case-insensitive names to the
 * same inode, only the actual correct case is stored in the dcache for
 * case-insensitive filesystems.
 *
 * For a case-insensitive lookup match and if the the case-exact dentry
 * already exists in in the dcache, use it and return it.
 *
 * If no entry exists with the exact case name, allocate new dentry with
 * the exact case, and return the spliced entry.
 */
/*
 * d_add_ci - 使用确切大小写的名称查找或分配新的目录项
 * @inode:  大小写不敏感查找找到的 inode
 * @dentry: 传递给父目录查找函数的负目录项
 * @name:   与返回的目录项关联的确切大小写名称
 *
 * 这是为了避免在 dcache 中填充大小写不敏感的名称到同一个 inode，
 * 只有实际正确的大小写被存储在用于大小写不敏感文件系统的 dcache 中。
 *
 * 如果大小写不敏感查找匹配，并且确切大小写的目录项已经存在于 dcache 中，
 * 使用它并返回它。
 *
 * 如果没有与确切大小写名称匹配的条目，分配一个新的具有确切大小写的目录项，
 * 并返回接入的条目。
 */
// 用于处理文件系统中大小写不敏感的查找，确保大小写准确的名称被存储在目录项缓存（dcache）中。
struct dentry *d_add_ci(struct dentry *dentry, struct inode *inode,
			struct qstr *name)
{
	int error;  // 用于存储错误代码
	struct dentry *found;  // 用于存储找到的目录项
	struct dentry *new;  // 用于存储新分配的目录项

	/*
	 * First check if a dentry matching the name already exists,
	 * if not go ahead and create it now.
	 */
	/*
	 * 首先检查是否已存在一个与给定名称匹配的目录项，
	 * 如果不存在，则立即创建一个新的目录项。
	 */
	found = d_hash_and_lookup(dentry->d_parent, name);
	if (!found) {
		new = d_alloc(dentry->d_parent, name);  // 如果未找到，在父目录下分配一个新的目录项
		if (!new) {	// 分配失败处理
			error = -ENOMEM;  // 分配失败，返回内存不足错误
			goto err_out;			// 跳转到错误处理代码
		}

		found = d_splice_alias(inode, new);	// 尝试将新目录项与对应的 inode 关联
		if (found) {	// 如果已经存在一个相应的目录项
			dput(new);  // 如果已存在一个别名，释放新分配的目录项
			return found;  // 返回已存在的目录项
		}
		return new;  // 如果不存在已有的目录项，返回新创建的目录项
	}

	/*
	 * If a matching dentry exists, and it's not negative use it.
	 *
	 * Decrement the reference count to balance the iget() done
	 * earlier on.
	 */
	/*
	 * 如果存在一个匹配的目录项，并且它不是负目录项（即已经与一个 inode 关联），则使用它。
	 *
	 * 减少 inode 的引用计数，以平衡之前对 inode 的获取（iget()）。
	 */
	// 如果找到匹配的目录项，并且它不是负目录项，则使用它
	if (found->d_inode) {
		if (unlikely(found->d_inode != inode)) {
			/* This can't happen because bad inodes are unhashed. */
			/* 这种情况不应该发生，因为损坏的 inode 是不会被哈希处理的。 */
			// 确保不会发生 inode 不匹配的情况
			BUG_ON(!is_bad_inode(inode));  // 确保 inode 不是坏的
			BUG_ON(!is_bad_inode(found->d_inode));  // 确保找到的 inode 也不是坏的
		}
		iput(inode);  // 减少 inode 的引用计数，以平衡之前的 iget()
		return found;  // 返回找到的目录项
	}

	/*
	 * Negative dentry: instantiate it unless the inode is a directory and
	 * already has a dentry.
	 */
	/* 负目录项：实例化它，除非该 inode 是目录并且已经有了目录项。 */
	spin_lock(&dcache_lock);	// 锁定目录项缓存
	if (!S_ISDIR(inode->i_mode) || list_empty(&inode->i_dentry)) {	// 如果 inode 不是目录或目录下没有目录项
		__d_instantiate(found, inode);  // 实例化找到的目录项
		spin_unlock(&dcache_lock);	// 解锁目录项缓存
		security_d_instantiate(found, inode);  // 调用安全模块相关函数处理实例化后的目录项
		return found;	// 返回实例化的目录项
	}

	/*
	 * In case a directory already has a (disconnected) entry grab a
	 * reference to it, move it in place and use it.
	 */
	/*
	 * 如果一个目录已经有一个（断开连接的）目录项，获取对它的引用，
	 * 移动它到位并使用它。
	 */
	new = list_entry(inode->i_dentry.next, struct dentry, d_alias); // 获取 inode 的下一个目录项
	dget_locked(new); // 增加新目录项的引用计数，确保线程安全
	spin_unlock(&dcache_lock); // 解锁目录项缓存
	security_d_instantiate(found, inode); // 再次调用安全模块处理实例化
	d_move(new, found); // 将新的目录项移动到找到的目录项位置
	iput(inode); // 减少 inode 的引用计数
	dput(found); // 减少找到的目录项的引用计数
	return new; // 返回移动后的目录项

err_out:
	iput(inode); // 如果发生错误，减少 inode 的引用计数
	return ERR_PTR(error); // 返回错误指针
}
EXPORT_SYMBOL(d_add_ci);

/**
 * d_lookup - search for a dentry
 * @parent: parent dentry
 * @name: qstr of name we wish to find
 *
 * Searches the children of the parent dentry for the name in question. If
 * the dentry is found its reference count is incremented and the dentry
 * is returned. The caller must use dput to free the entry when it has
 * finished using it. %NULL is returned on failure.
 *
 * __d_lookup is dcache_lock free. The hash list is protected using RCU.
 * Memory barriers are used while updating and doing lockless traversal. 
 * To avoid races with d_move while rename is happening, d_lock is used.
 *
 * Overflows in memcmp(), while d_move, are avoided by keeping the length
 * and name pointer in one structure pointed by d_qstr.
 *
 * rcu_read_lock() and rcu_read_unlock() are used to disable preemption while
 * lookup is going on.
 *
 * The dentry unused LRU is not updated even if lookup finds the required dentry
 * in there. It is updated in places such as prune_dcache, shrink_dcache_sb,
 * select_parent and __dget_locked. This laziness saves lookup from dcache_lock
 * acquisition.
 *
 * d_lookup() is protected against the concurrent renames in some unrelated
 * directory using the seqlockt_t rename_lock.
 */
/*
 * d_lookup - 搜索目录项
 * @parent: 父目录项
 * @name: 我们希望找到的名称的 qstr
 *
 * 在父目录项的子项中搜索指定的名称。如果找到了目录项，其引用计数将被增加，并返回该目录项。
 * 调用者必须在使用完目录项后使用 dput 来释放该条目。失败时返回 %NULL。
 *
 * __d_lookup 不使用 dcache_lock。哈希列表使用 RCU 保护。
 * 在更新和无锁遍历时使用内存屏障。为了避免在重命名时与 d_move 竞争，使用 d_lock。
 *
 * 通过将长度和名称指针保持在由 d_qstr 指向的一个结构中，避免了在 d_move 时 memcmp() 的溢出。
 *
 * 在查找进行时，使用 rcu_read_lock() 和 rcu_read_unlock() 禁用抢占。
 *
 * 即使查找找到所需的目录项，未使用的 dentry LRU 也不会更新。它在如 prune_dcache、shrink_dcache_sb、
 * select_parent 和 __dget_locked 等地方更新。这种懒惰保存了查找免于获取 dcache_lock。
 *
 * d_lookup() 使用 seqlockt_t rename_lock 保护，防止在某些无关目录中进行并发重命名。
 */
// 如果在dcache（缓存，即dentry_hashtable）中发现了与其匹配的目录项对象，则匹配的对象被返回，否则返回NULL。
// 用于在目录项缓存中搜索特定的目录项。如果找到了目录项，则增加其引用计数并返回。
struct dentry * d_lookup(struct dentry * parent, struct qstr * name)
{
	struct dentry * dentry = NULL; // 初始化要返回的目录项指针
	unsigned long seq; // 用于序列锁

        do {
                seq = read_seqbegin(&rename_lock); // 开始读取序列，获取当前的序列号
                dentry = __d_lookup(parent, name); // 在父目录项中查找给定名称的目录项
                if (dentry)
			break; // 如果找到目录项，跳出循环
	} while (read_seqretry(&rename_lock, seq)); // 如果序列号改变（即数据变动），重试读取
	return dentry; // 返回找到的或 NULL 的目录项
}
EXPORT_SYMBOL(d_lookup);

// 用于在目录项缓存中查找与给定名称和父目录项匹配的目录项
struct dentry * __d_lookup(struct dentry * parent, struct qstr * name)
{
	unsigned int len = name->len;  // 名称长度
	unsigned int hash = name->hash;  // 名称哈希值
	const unsigned char *str = name->name;  // 指向名称字符串
	// 获取dentry_hashtable其中的一个元素，这个元素本身又是一个链表
	// 获取与给定哈希值对应的哈希桶
	struct hlist_head *head = d_hash(parent, hash); 
	struct dentry *found = NULL;  // 用于存储找到的目录项
	struct hlist_node *node;  // 用于遍历链表
	struct dentry *dentry;  // 当前遍历的目录项

	rcu_read_lock();  // 使用RCU锁保护读取操作
	
	// 通过便利dentry_hashtable中的链表来完成查找
	hlist_for_each_entry_rcu(dentry, node, head, d_hash) {// 遍历哈希桶中的所有目录项
		struct qstr *qstr;

		// 检查哈希值和父目录项是否匹配
		if (dentry->d_name.hash != hash)
			continue;
		if (dentry->d_parent != parent)
			continue;

		spin_lock(&dentry->d_lock);	// 对目录项加锁以进行更详细的检查

		/*
		 * Recheck the dentry after taking the lock - d_move may have
		 * changed things.  Don't bother checking the hash because we're
		 * about to compare the whole name anyway.
		 */
		/*
		 * 在获取锁之后重新检查父目录项 - d_move 可能已经更改了父目录项。
		 * 不需要再检查哈希值，因为我们接下来会比较整个名称。
		 */
		if (dentry->d_parent != parent)
			goto next;

		/* non-existing due to RCU? */
		// 检查目录项是否因为RCU而不存在
		if (d_unhashed(dentry))
			goto next;

		/*
		 * It is safe to compare names since d_move() cannot
		 * change the qstr (protected by d_lock).
		 */
		/*
		 * 在保护下比较名称是安全的，因为 d_move() 不能更改 qstr（由 d_lock 保护）。
		 */
		qstr = &dentry->d_name;
		if (parent->d_op && parent->d_op->d_compare) {
			// 使用父目录项的比较函数进行比较
			if (parent->d_op->d_compare(parent, qstr, name))
				goto next;
		} else {
			// 使用默认的比较逻辑
			if (qstr->len != len)
				goto next;
			if (memcmp(qstr->name, str, len))
				goto next;
		}

		atomic_inc(&dentry->d_count);  // 增加目录项的引用计数
		found = dentry;  // 标记找到的目录项
		spin_unlock(&dentry->d_lock);  // 解锁
		break;
next:
		spin_unlock(&dentry->d_lock);  // 解锁继续下一个循环
 	}
 	rcu_read_unlock();  // 释放RCU锁

 	return found;  // 返回找到的目录项，如果没有找到则为NULL
}

/**
 * d_hash_and_lookup - hash the qstr then search for a dentry
 * @dir: Directory to search in
 * @name: qstr of name we wish to find
 *
 * On hash failure or on lookup failure NULL is returned.
 */
/*
 * d_hash_and_lookup - 对 qstr 进行哈希处理然后搜索目录项
 * @dir: 要搜索的目录
 * @name: 我们希望找到的名称的 qstr
 *
 * 哈希失败或查找失败时返回 NULL。
 */
struct dentry *d_hash_and_lookup(struct dentry *dir, struct qstr *name)
{
	struct dentry *dentry = NULL;  // 初始化目录项指针

	/*
	 * Check for a fs-specific hash function. Note that we must
	 * calculate the standard hash first, as the d_op->d_hash()
	 * routine may choose to leave the hash value unchanged.
	 */
	/*
	 * 检查是否有文件系统特定的哈希函数。注意，我们必须首先计算标准哈希，
	 * 因为 d_op->d_hash() 函数可能选择保留未更改的哈希值。
	 */
	name->hash = full_name_hash(name->name, name->len);  // 计算名称的标准哈希值
	if (dir->d_op && dir->d_op->d_hash) {  // 如果定义了目录操作并且存在哈希函数
		if (dir->d_op->d_hash(dir, name) < 0)  // 执行特定的哈希函数
			goto out;  // 如果哈希函数返回错误，则跳转到函数末尾
	}
	dentry = d_lookup(dir, name);  // 使用标准的查找函数搜索目录项
out:
	return dentry;  // 返回找到的目录项或 NULL
}

/**
 * d_validate - verify dentry provided from insecure source
 * @dentry: The dentry alleged to be valid child of @dparent
 * @dparent: The parent dentry (known to be valid)
 *
 * An insecure source has sent us a dentry, here we verify it and dget() it.
 * This is used by ncpfs in its readdir implementation.
 * Zero is returned in the dentry is invalid.
 */
/*
 * d_validate - 验证来自不安全来源的目录项
 * @dentry: 声称是 @dparent 的有效子目录项
 * @dparent: 已知有效的父目录项
 *
 * 一个不安全来源提供了一个目录项，这里我们验证它并对它进行 dget() 操作。
 * 这在 ncpfs 的 readdir 实现中使用。
 * 如果目录项无效，则返回零。
 */
int d_validate(struct dentry *dentry, struct dentry *dparent)
{
	struct hlist_head *base;  // 哈希链表头指针
	struct hlist_node *lhp;  // 哈希链表节点指针

	/* Check whether the ptr might be valid at all.. */
	// 检查指针是否可能完全有效
	if (!kmem_ptr_validate(dentry_cache, dentry))
		goto out;  // 如果内存指针无效，则跳转到函数末尾

	if (dentry->d_parent != dparent)
		goto out;  // 如果目录项的父目录项不匹配，则跳转到函数末尾

	spin_lock(&dcache_lock);  // 加锁目录项缓存
	base = d_hash(dparent, dentry->d_name.hash);  // 获取哈希桶
	hlist_for_each(lhp, base) {  // 遍历哈希链表
		/* hlist_for_each_entry_rcu() not required for d_hash list
		 * as it is parsed under dcache_lock
		 */
		/*
		 * d_hash 列表不需要 hlist_for_each_entry_rcu()，
		 * 因为它在 dcache_lock 下解析
		 */
		if (dentry == hlist_entry(lhp, struct dentry, d_hash)) {  // 如果找到匹配的目录项
			__dget_locked(dentry);  // 增加目录项的引用计数
			spin_unlock(&dcache_lock);  // 解锁目录项缓存
			return 1;  // 返回 1 表示成功
		}
	}
	spin_unlock(&dcache_lock);  // 解锁目录项缓存
out:
	return 0;  // 返回 0 表示目录项无效
}
EXPORT_SYMBOL(d_validate);

/*
 * When a file is deleted, we have two options:
 * - turn this dentry into a negative dentry
 * - unhash this dentry and free it.
 *
 * Usually, we want to just turn this into
 * a negative dentry, but if anybody else is
 * currently using the dentry or the inode
 * we can't do that and we fall back on removing
 * it from the hash queues and waiting for
 * it to be deleted later when it has no users
 */
/*
 * 当一个文件被删除时，我们有两个选择：
 * - 将此目录项转换为负目录项
 * - 从哈希表中移除此目录项并释放它。
 *
 * 通常，我们希望将其转换为负目录项，但如果其他用户
 * 正在使用该目录项或inode，我们无法这样做，只能
 * 从哈希队列中移除它，并等待它在没有用户时被删除
 */

/**
 * d_delete - delete a dentry
 * @dentry: The dentry to delete
 *
 * Turn the dentry into a negative dentry if possible, otherwise
 * remove it from the hash queues so it can be deleted later
 */
/*
 * d_delete - 删除一个目录项
 * @dentry: 要删除的目录项
 *
 * 如果可能，将目录项转换为负目录项，否则从哈希队列中移除，
 * 以便稍后删除
 */
// 删除一个目录项。该函数会尝试将目录项转换为一个负目录项，如果不可能（例如，有其他用户正在使用此目录项或 inode），则将其从哈希队列中移除，以便稍后在没有用户使用时删除它。
void d_delete(struct dentry * dentry)
{
	int isdir = 0;
	/*
	 * Are we the only user?
	 */
	/*
	 * 我们是唯一的用户吗？
	 */
	spin_lock(&dcache_lock);  // 锁定目录项缓存
	spin_lock(&dentry->d_lock);  // 锁定目标目录项
	isdir = S_ISDIR(dentry->d_inode->i_mode);  // 检查目录项是否为目录
	if (atomic_read(&dentry->d_count) == 1) {  // 如果目录项的引用计数为1
		dentry_iput(dentry);  // 释放目录项和关联的inode
		fsnotify_nameremove(dentry, isdir);  // 发送文件系统事件通知
		return;
	}

	if (!d_unhashed(dentry))
		__d_drop(dentry);  // 如果目录项未从哈希表中移除，那么移除它

	spin_unlock(&dentry->d_lock);  // 解锁目录项
	spin_unlock(&dcache_lock);  // 解锁目录项缓存

	fsnotify_nameremove(dentry, isdir);  // 发送文件系统事件通知
}
EXPORT_SYMBOL(d_delete);

// __d_rehash - 将目录项重新加入到哈希链表
// @entry: 要重新哈希的目录项
// @list: 目录项应该被加入的哈希链表
static void __d_rehash(struct dentry * entry, struct hlist_head *list)
{
 	entry->d_flags &= ~DCACHE_UNHASHED;  // 清除目录项的未哈希标记
 	hlist_add_head_rcu(&entry->d_hash, list);  // 将目录项添加到指定的哈希链表的头部，使用 RCU 保护
}

// _d_rehash - 通过目录项的父目录项和名称哈希值确定哈希链表，然后调用 __d_rehash 函数
// @entry: 要重新哈希的目录项
static void _d_rehash(struct dentry * entry)
{
	__d_rehash(entry, d_hash(entry->d_parent, entry->d_name.hash));  // 计算哈希链表并调用 __d_rehash 函数
}

/**
 * d_rehash	- add an entry back to the hash
 * @entry: dentry to add to the hash
 *
 * Adds a dentry to the hash according to its name.
 */
/*
 * d_rehash - 将一个条目重新加入到哈希表
 * @entry: 要加入哈希表的目录项
 *
 * 根据其名称将目录项加入到哈希表。
 */
void d_rehash(struct dentry * entry)
{
	spin_lock(&dcache_lock);  // 获取dcache的锁，防止在操作哈希表时发生并发修改
	spin_lock(&entry->d_lock);  // 锁定具体的目录项，确保对目录项的操作是安全的
	_d_rehash(entry);  // 调用_d_rehash函数，根据目录项的父目录和名称哈希重新计算其位置，并添加到哈希表中
	spin_unlock(&entry->d_lock);  // 解锁目录项
	spin_unlock(&dcache_lock);  // 释放dcache的锁
}
EXPORT_SYMBOL(d_rehash);

/*
 * When switching names, the actual string doesn't strictly have to
 * be preserved in the target - because we're dropping the target
 * anyway. As such, we can just do a simple memcpy() to copy over
 * the new name before we switch.
 *
 * Note that we have to be a lot more careful about getting the hash
 * switched - we have to switch the hash value properly even if it
 * then no longer matches the actual (corrupted) string of the target.
 * The hash value has to match the hash queue that the dentry is on..
 */
/*
 * 在交换名称时，目标中的实际字符串并不需要严格保持不变——因为我们反正要丢弃目标。
 * 因此，我们可以简单地使用 memcpy() 来复制新名称，然后再进行交换。
 *
 * 需要注意的是，我们必须更加小心地处理哈希值的交换——即使这可能导致哈希值不再匹配
 * 实际的（可能已损坏的）字符串，我们也必须正确地交换哈希值。
 * 哈希值必须匹配目录项所在的哈希队列。
 */
// 用于在两个目录项（dentry 和 target）之间交换名称。该函数处理名称存储的不同情况，确保即使在交换后名称字符串可能被损坏的情况下，哈希值也正确无误地匹配目录项所在的哈希队列。
static void switch_names(struct dentry *dentry, struct dentry *target)
{
	if (dname_external(target)) {  // 如果 target 的名称是外部存储的
		if (dname_external(dentry)) {
			/*
			 * Both external: swap the pointers
			 */
			/*
			 * 两者均为外部存储：交换指针
			 */
			swap(target->d_name.name, dentry->d_name.name);
		} else {
			/*
			 * dentry:internal, target:external.  Steal target's
			 * storage and make target internal.
			 */
			/*
			 * dentry 是内部存储，target 是外部存储。窃取 target 的
			 * 存储空间，并将 target 转为内部存储。
			 */
			memcpy(target->d_iname, dentry->d_name.name, dentry->d_name.len + 1);
			dentry->d_name.name = target->d_name.name;
			target->d_name.name = target->d_iname;
		}
	} else {
		if (dname_external(dentry)) {
			/*
			 * dentry:external, target:internal.  Give dentry's
			 * storage to target and make dentry internal
			 */
			/*
			 * dentry 是外部存储，target 是内部存储。将 dentry 的
			 * 存储空间给 target，并将 dentry 转为内部存储。
			 */
			memcpy(dentry->d_iname, target->d_name.name, target->d_name.len + 1);
			target->d_name.name = dentry->d_name.name;
			dentry->d_name.name = dentry->d_iname;
		} else {
			/*
			 * Both are internal.  Just copy target to dentry
			 */
			/*
			 * 两者均为内部存储：只需将 target 的内容复制到 dentry
			 */
			memcpy(dentry->d_iname, target->d_name.name, target->d_name.len + 1);
			dentry->d_name.len = target->d_name.len;
			return;
		}
	}
	swap(dentry->d_name.len, target->d_name.len);  // 交换名称长度
}

/*
 * We cannibalize "target" when moving dentry on top of it,
 * because it's going to be thrown away anyway. We could be more
 * polite about it, though.
 *
 * This forceful removal will result in ugly /proc output if
 * somebody holds a file open that got deleted due to a rename.
 * We could be nicer about the deleted file, and let it show
 * up under the name it had before it was deleted rather than
 * under the original name of the file that was moved on top of it.
 */
/*
 * 在将 dentry 移动到它上面时，我们会损坏“target”，因为它无论如何都将被丢弃。
 * 不过，我们可以更礼貌一些。
 *
 * 这种强制移除将导致丑陋的 /proc 输出，如果有人持有一个由于重命名而被删除的文件打开。
 * 我们可以对被删除的文件更好一些，让它显示在它被删除前的名称下，而不是被移动到其上的文件的原始名称下。
 */
 
/*
 * d_move_locked - move a dentry
 * @dentry: entry to move
 * @target: new dentry
 *
 * Update the dcache to reflect the move of a file name. Negative
 * dcache entries should not be moved in this way.
 */
/*
 * d_move_locked - 移动一个目录项
 * @dentry: 要移动的目录项
 * @target: 新的目录项
 *
 * 更新目录项缓存以反映文件名的移动。负目录项缓存不应该以这种方式移动。
 */
static void d_move_locked(struct dentry * dentry, struct dentry * target)
{
	struct hlist_head *list;  // 哈希列表头部指针

	if (!dentry->d_inode)	// 如果尝试移动一个没有inode的目录项（即负缓存目录项）
		printk(KERN_WARNING "VFS: moving negative dcache entry\n");  // 如果尝试移动一个负缓存目录项，打印警告

	write_seqlock(&rename_lock);  // 获取重命名操作的序列锁
	/*
	 * XXXX: do we really need to take target->d_lock?
	 */
	// 根据目录项和目标的内存地址顺序来决定锁的顺序，避免死锁
	// 获取 dentry 和 target 的锁，确保操作的原子性
	if (target < dentry) {
		spin_lock(&target->d_lock);
		spin_lock_nested(&dentry->d_lock, DENTRY_D_LOCK_NESTED);
	} else {
		spin_lock(&dentry->d_lock);
		spin_lock_nested(&target->d_lock, DENTRY_D_LOCK_NESTED);
	}

	/* Move the dentry to the target hash queue, if on different bucket */
	// 如果 dentry 未被哈希，则跳过删除哈希条目的步骤
	if (d_unhashed(dentry))	// 如果目录项未在哈希表中
		goto already_unhashed;

	hlist_del_rcu(&dentry->d_hash);	// 从哈希表中删除 dentry

already_unhashed:
	list = d_hash(target->d_parent, target->d_name.hash);  // 计算目标目录项在哈希表中的位置
	__d_rehash(dentry, list);  // 重新哈希目录项到新位置

	/* Unhash the target: dput() will then get rid of it */
	// 将 target 从哈希表中移除
	__d_drop(target);	// 将目标目录项从哈希表中移除

	list_del(&dentry->d_u.d_child);  // 从其父目录的子列表中删除 dentry
	list_del(&target->d_u.d_child);  // 从其父目录的子列表中删除 target

	/* Switch the names.. */
	switch_names(dentry, target);	// 交换两个目录项的名称
	swap(dentry->d_name.hash, target->d_name.hash);	// 交换哈希值

	/* ... and switch the parents */
	// 交换父目录项
	if (IS_ROOT(dentry)) {	// 如果目录项是根目录项
		dentry->d_parent = target->d_parent;
		target->d_parent = target;
		INIT_LIST_HEAD(&target->d_u.d_child);
	} else {
		swap(dentry->d_parent, target->d_parent);	// 交换两个目录项的父目录项

		/* And add them back to the (new) parent lists */
		// 将它们重新添加到新的父目录项列表中
		list_add(&target->d_u.d_child, &target->d_parent->d_subdirs);	// 将目标添加回其父目录项的子目录列表
	}

	list_add(&dentry->d_u.d_child, &dentry->d_parent->d_subdirs);	// 将目录项添加回其父目录项的子目录列表
	spin_unlock(&target->d_lock);  // 解锁
	fsnotify_d_move(dentry);  // 通知文件系统目录项已经移动
	spin_unlock(&dentry->d_lock);  // 解锁
	write_sequnlock(&rename_lock);  // 释放重命名操作的序列锁
}

/**
 * d_move - move a dentry
 * @dentry: entry to move
 * @target: new dentry
 *
 * Update the dcache to reflect the move of a file name. Negative
 * dcache entries should not be moved in this way.
 */
/*
 * d_move - 移动一个目录项
 * @dentry: 要移动的目录项
 * @target: 新的目录项位置
 *
 * 更新目录项缓存以反映文件名的移动。不应以这种方式移动负目录项缓存。
 */
void d_move(struct dentry * dentry, struct dentry * target)
{
	spin_lock(&dcache_lock);  // 获取全局目录项缓存锁，确保操作的线程安全
	d_move_locked(dentry, target);  // 调用锁定版本的移动函数进行实际的移动操作
	spin_unlock(&dcache_lock);  // 释放全局目录项缓存锁
}
EXPORT_SYMBOL(d_move);

/**
 * d_ancestor - search for an ancestor
 * @p1: ancestor dentry
 * @p2: child dentry
 *
 * Returns the ancestor dentry of p2 which is a child of p1, if p1 is
 * an ancestor of p2, else NULL.
 */
/*
 * d_ancestor - 搜索一个祖先
 * @p1: 祖先目录项
 * @p2: 子目录项
 *
 * 如果 p1 是 p2 的一个祖先，则返回 p2 的一个作为 p1 子项的祖先目录项，否则返回 NULL。
 */
struct dentry *d_ancestor(struct dentry *p1, struct dentry *p2)
{
	struct dentry *p;

	for (p = p2; !IS_ROOT(p); p = p->d_parent) {  // 从 p2 开始向上遍历，直到到达根目录项
		if (p->d_parent == p1)  // 如果找到 p1 作为 p 的父目录项
			return p;  // 返回该目录项
	}
	return NULL;  // 如果没有找到，返回 NULL
}

/*
 * This helper attempts to cope with remotely renamed directories
 *
 * It assumes that the caller is already holding
 * dentry->d_parent->d_inode->i_mutex and the dcache_lock
 *
 * Note: If ever the locking in lock_rename() changes, then please
 * remember to update this too...
 */
/*
 * 此助手尝试处理远程重命名的目录
 *
 * 它假设调用者已经持有
 * dentry->d_parent->d_inode->i_mutex 和 dcache_lock
 *
 * 注意：如果 lock_rename() 中的锁定机制发生变化，
 * 请记得也更新这里...
 */
static struct dentry *__d_unalias(struct dentry *dentry, struct dentry *alias)
	__releases(dcache_lock)  // 标记该函数会释放 dcache_lock
{
	struct mutex *m1 = NULL, *m2 = NULL;
	struct dentry *ret;

	/* If alias and dentry share a parent, then no extra locks required */
	/* 如果 alias 和 dentry 共享同一个父目录，那么不需要额外的锁 */
	if (alias->d_parent == dentry->d_parent)
		goto out_unalias;

	/* Check for loops */
	/* 检查是否存在循环引用 */
	ret = ERR_PTR(-ELOOP);
	if (d_ancestor(alias, dentry))
		goto out_err;

	/* See lock_rename() */
	/* 参考 lock_rename() */
	ret = ERR_PTR(-EBUSY);
	if (!mutex_trylock(&dentry->d_sb->s_vfs_rename_mutex))
		goto out_err;
	m1 = &dentry->d_sb->s_vfs_rename_mutex;
	if (!mutex_trylock(&alias->d_parent->d_inode->i_mutex))
		goto out_err;
	m2 = &alias->d_parent->d_inode->i_mutex;

out_unalias:
	d_move_locked(alias, dentry);  // 移动 alias 到 dentry 的位置
	ret = alias;

out_err:
	spin_unlock(&dcache_lock);  // 释放 dcache_lock
	if (m2)
		mutex_unlock(m2);  // 释放第二个互斥锁
	if (m1)
		mutex_unlock(m1);  // 释放第一个互斥锁
	return ret;
}

/*
 * Prepare an anonymous dentry for life in the superblock's dentry tree as a
 * named dentry in place of the dentry to be replaced.
 */
/*
 * 准备一个匿名目录项，让它在超级块的目录项树中作为一个具名目录项生存，
 * 取代将被替换的目录项。
 */
// 用于将一个匿名目录项（anon）准备成为一个具名目录项，并取代将被替换的目录项（dentry）。这通常发生在文件系统操作中，如临时文件的实际创建或某些特定类型的重命名操作
static void __d_materialise_dentry(struct dentry *dentry, struct dentry *anon)
{
	struct dentry *dparent, *aparent;  // 定义用于存储父目录项的变量

	switch_names(dentry, anon);  // 交换 dentry 和 anon 的名称
	swap(dentry->d_name.hash, anon->d_name.hash);  // 交换它们的哈希值

	dparent = dentry->d_parent;  // 获取 dentry 的父目录项
	aparent = anon->d_parent;  // 获取 anon 的父目录项

	// 更新 dentry 的父目录项指针
	dentry->d_parent = (aparent == anon) ? dentry : aparent;
	list_del(&dentry->d_u.d_child);  // 从其父目录项的子目录列表中删除 dentry
	if (!IS_ROOT(dentry))  // 如果 dentry 不是根目录项
		list_add(&dentry->d_u.d_child, &dentry->d_parent->d_subdirs);  // 将 dentry 添加到新父目录项的子目录列表中
	else
		INIT_LIST_HEAD(&dentry->d_u.d_child);  // 初始化 dentry 的子目录列表头部

	// 更新 anon 的父目录项指针
	anon->d_parent = (dparent == dentry) ? anon : dparent;
	list_del(&anon->d_u.d_child);  // 从其父目录项的子目录列表中删除 anon
	if (!IS_ROOT(anon))  // 如果 anon 不是根目录项
		list_add(&anon->d_u.d_child, &anon->d_parent->d_subdirs);  // 将 anon 添加到新父目录项的子目录列表中
	else
		INIT_LIST_HEAD(&anon->d_u.d_child);  // 初始化 anon 的子目录列表头部

	anon->d_flags &= ~DCACHE_DISCONNECTED;  // 清除 anon 的断开连接标志
}

/**
 * d_materialise_unique - introduce an inode into the tree
 * @dentry: candidate dentry
 * @inode: inode to bind to the dentry, to which aliases may be attached
 *
 * Introduces an dentry into the tree, substituting an extant disconnected
 * root directory alias in its place if there is one
 */
/*
 * d_materialise_unique - 将一个inode引入树中
 * @dentry: 候选的目录项
 * @inode: 要绑定到目录项的inode，可能已经有别名附加
 *
 * 将一个目录项引入目录项树中，如果存在，用一个现有的断开连接的根目录别名替换它
 */
// 用于将一个目录项（dentry）以及关联的 inode 引入到目录项树中，如果存在已断开连接的根目录别名，将其替换。
struct dentry *d_materialise_unique(struct dentry *dentry, struct inode *inode)
{
	struct dentry *actual;

	BUG_ON(!d_unhashed(dentry));	// 确保dentry未被哈希

	spin_lock(&dcache_lock);	// 加锁目录项缓存锁以保护目录项树的结构

	if (!inode) {	// 如果inode为空
		actual = dentry;	// 如果inode为空，使用现有的dentry
		__d_instantiate(dentry, NULL);	// 实例化目录项但不关联任何inode
		goto found_lock;	// 跳转到锁定找到的目录项
	}

	if (S_ISDIR(inode->i_mode)) {	// 如果inode是一个目录
		struct dentry *alias;

		/* Does an aliased dentry already exist? */
		/* 是否已经存在别名的dentry？ */
		alias = __d_find_alias(inode, 0);	// 查找是否存在指向inode的别名目录项
		if (alias) {
			actual = alias;	// 如果找到别名，使用别名目录项
			/* Is this an anonymous mountpoint that we could splice
			 * into our tree? */
			/* 这是一个匿名挂载点，我们可以将其接入到我们的树中吗？ */
			if (IS_ROOT(alias)) {	// 如果alias是根目录
				spin_lock(&alias->d_lock);	// 加锁别名目录项
				__d_materialise_dentry(dentry, alias);	// 实体化dentry到alias
				__d_drop(alias);	// 从哈希表中删除别名目录项
				goto found;	// 跳转到处理找到的目录项
			}
			/* Nope, but we must(!) avoid directory aliasing */
			/* 不是的话，我们必须避免目录别名 */
			// 不是匿名挂载点，但我们必须避免目录别名
			actual = __d_unalias(dentry, alias);	// 处理目录别名
			if (IS_ERR(actual))	// 处理错误，释放别名目录项的引用
				dput(alias);
			goto out_nolock;	// 跳过锁定，处理结束
		}
	}

	/* Add a unique reference */
	/* 添加一个唯一引用 */
	actual = __d_instantiate_unique(dentry, inode);	// 实例化唯一的dentry
	if (!actual)
		actual = dentry;	// 如果实例化失败，使用原始dentry
	else if (unlikely(!d_unhashed(actual)))
		goto shouldnt_be_hashed;	// 如果实例化的dentry已经被哈希，跳到错误处理

found_lock:
	spin_lock(&actual->d_lock);	// 获取实例化后的dentry的锁
found:
	_d_rehash(actual);	// 将实例化的dentry重新哈希到目录项哈希表
	spin_unlock(&actual->d_lock);	// 解锁实例化后的dentry
	spin_unlock(&dcache_lock);	// 解锁目录项缓存锁
out_nolock:
	if (actual == dentry) {	// 如果实际的dentry等于候选的dentry
		security_d_instantiate(dentry, inode);	// 安全性处理实例化dentry
		return NULL;	// 返回NULL，因为dentry已经被处理
	}

	iput(inode);	// 减少inode的引用计数
	return actual;	// 返回处理后的dentry

shouldnt_be_hashed:
	// 错误处理：解锁目录项缓存锁
	spin_unlock(&dcache_lock);	// 解锁目录项树
	// 如果实例化的dentry已经被哈希，这是一个BUG
	BUG();	// 如果状态不一致，触发BUG
}
EXPORT_SYMBOL_GPL(d_materialise_unique);

/**
 * prepend - 将字符串复制到buffer的当前位置，并更新buffer指针和buflen
 * @buffer: 指向buffer当前开始位置的指针的地址
 * @buflen: 指向剩余buffer长度的指针
 * @str: 要复制的字符串
 * @namelen: 字符串的长度
 *
 * 此函数用于将字符串str复制到buffer指向的位置，并更新buffer指针以指向新的起始位置。
 * 如果buffer空间不足，返回-ENAMETOOLONG。
 * 返回0表示成功。
 */
static int prepend(char **buffer, int *buflen, const char *str, int namelen)
{
	*buflen -= namelen;  // 更新剩余buffer长度
	if (*buflen < 0)
		return -ENAMETOOLONG;  // 如果空间不足，返回错误
	*buffer -= namelen;  // 移动buffer指针
	memcpy(*buffer, str, namelen);  // 复制字符串到buffer
	return 0;  // 成功完成操作
}

/**
 * prepend_name - 使用qstr结构的名称调用prepend函数
 * @buffer: 指向buffer当前开始位置的指针的地址
 * @buflen: 指向剩余buffer长度的指针
 * @name: 指向qstr结构的指针，其中包含名称和长度
 *
 * 此函数是prepend的包装，专门处理qstr结构，便于从qstr中获取名称和长度，并调用prepend。
 * 返回prepend的返回值。
 */
static int prepend_name(char **buffer, int *buflen, struct qstr *name)
{
	return prepend(buffer, buflen, name->name, name->len);  // 调用prepend函数处理qstr
}

/**
 * __d_path - return the path of a dentry
 * @path: the dentry/vfsmount to report
 * @root: root vfsmnt/dentry (may be modified by this function)
 * @buffer: buffer to return value in
 * @buflen: buffer length
 *
 * Convert a dentry into an ASCII path name. If the entry has been deleted
 * the string " (deleted)" is appended. Note that this is ambiguous.
 *
 * Returns a pointer into the buffer or an error code if the
 * path was too long.
 *
 * "buflen" should be positive. Caller holds the dcache_lock.
 *
 * If path is not reachable from the supplied root, then the value of
 * root is changed (without modifying refcounts).
 */
/**
 * __d_path - 返回目录项的路径
 * @path: 要报告的目录项/vfsmount
 * @root: root vfsmnt/dentry（可能会被此函数修改）
 * @buffer: 返回值的缓冲区
 * @buflen: 缓冲区长度
 *
 * 将目录项转换为ASCII路径名。如果条目已被删除，则字符串 " (deleted)" 被追加。
 * 注意，这可能会引起歧义。
 *
 * 返回缓冲区中的指针，或者如果路径过长则返回错误代码。
 *
 * "buflen" 应该为正值。调用者持有dcache_lock。
 *
 * 如果路径从提供的根开始无法到达，则root的值将被更改（不修改引用计数）。
 */
// 将给定的目录项和挂载点转换为文件系统的路径字符串。它会检查目录项是否已删除，并逐级向上构建路径，直至到达定义的根目录或文件系统的全局根。如果遇到路径长度超过提供的缓冲区限制，函数将返回一个错误。
char *__d_path(const struct path *path, struct path *root,
	       char *buffer, int buflen)
{
	struct dentry *dentry = path->dentry; // 当前目录项
	struct vfsmount *vfsmnt = path->mnt;  // 当前挂载点
	char *end = buffer + buflen;	// 缓冲区的结束位置
	char *retval;	// 将用于返回生成的路径或错误代码

	spin_lock(&vfsmount_lock);  // 锁定挂载点锁，确保vfsmount结构不被并发修改
	prepend(&end, &buflen, "\0", 1);  // 将字符串终结符添加到路径末尾，确保字符串结束
	if (d_unlinked(dentry) &&	// 检查目录项是否已经从目录树中断开
		(prepend(&end, &buflen, " (deleted)", 10) != 0))	// 如果已断开，则在路径末尾追加 " (deleted)"
			goto Elong;  // 如果追加失败（缓冲区空间不足），跳转到错误处理

	if (buflen < 1)	// 如果缓冲区长度不足，即使没有要追加的内容
		goto Elong;	// 跳转到错误处理
	/* Get '/' right */
	// 确保路径以 '/' 开头
	retval = end-1;	// 将retval设置为缓冲区中的最后一个字符的位置
	*retval = '/';	// 设置这个位置为 '/'

	for (;;) {
		struct dentry * parent;	// 定义指向父目录项的指针

		// 如果当前目录项是根目录项并且挂载点也是根挂载点，退出循环
		if (dentry == root->dentry && vfsmnt == root->mnt)
			break;	// 如果已达到根目录，退出循环
		// 如果当前目录项是挂载点的根或是根目录项
		if (dentry == vfsmnt->mnt_root || IS_ROOT(dentry)) {
			/* Global root? */
			// 如果到达了文件系统的根目录
			// 如果是全局根，即自己是自己的父挂载点
			if (vfsmnt->mnt_parent == vfsmnt) {
				goto global_root;	// 如果这是全局根目录
			}
			// 跳转到父挂载点的挂载点目录项
			dentry = vfsmnt->mnt_mountpoint;
			vfsmnt = vfsmnt->mnt_parent;
			continue;	// 继续循环
		}
		parent = dentry->d_parent;	// 获取当前目录项的父目录项
		prefetch(parent);	 // 提前获取父目录项的数据，提高访问效率
		// 向缓冲区中添加当前目录项的名称，并在前面加上斜杠('/')
		if ((prepend_name(&end, &buflen, &dentry->d_name) != 0) ||
		    (prepend(&end, &buflen, "/", 1) != 0))
			goto Elong;	// 如果添加失败，处理路径过长的情况
		retval = end;	// 更新返回值指针
		dentry = parent;	// 将当前目录项设置为其父目录项，准备下一次循环
	}

out:
	spin_unlock(&vfsmount_lock);	// 解锁挂载点锁
	return retval;	// 返回路径字符串

global_root:
	// 处理斜杠
	retval += 1;	/* hit the slash */	// 由于路径包含斜杠('/')，调整返回值指针
	// 添加全局根目录项的名称
	if (prepend_name(&retval, &buflen, &dentry->d_name) != 0)
		goto Elong;	// 如果添加失败，处理路径过长的情况
	root->mnt = vfsmnt;	// 更新根挂载点
	root->dentry = dentry;	// 更新根目录项
	goto out;	// 完成处理，退出

Elong:
	retval = ERR_PTR(-ENAMETOOLONG);	// 路径过长错误
	goto out;	// 完成处理，退出
}

/**
 * d_path - return the path of a dentry
 * @path: path to report
 * @buf: buffer to return value in
 * @buflen: buffer length
 *
 * Convert a dentry into an ASCII path name. If the entry has been deleted
 * the string " (deleted)" is appended. Note that this is ambiguous.
 *
 * Returns a pointer into the buffer or an error code if the path was
 * too long. Note: Callers should use the returned pointer, not the passed
 * in buffer, to use the name! The implementation often starts at an offset
 * into the buffer, and may leave 0 bytes at the start.
 *
 * "buflen" should be positive.
 */
/**
 * d_path - 返回目录项的路径
 * @path: 报告的路径
 * @buf: 用来返回值的缓冲区
 * @buflen: 缓冲区长度
 *
 * 将目录项转换成 ASCII 路径名。如果条目已被删除，则会追加字符串 " (deleted)"。
 * 注意，这可能会造成歧义。
 *
 * 如果路径过长，则返回指向缓冲区中某位置的指针或错误代码。注意：调用者应使用返回的指针，
 * 而不是传入的缓冲区，来使用这个名称！实现通常会从缓冲区的某个偏移量开始，
 * 并且可能在开始处留下 0 字节。
 *
 * "buflen" 应该是正值。
 */
char *d_path(const struct path *path, char *buf, int buflen)
{
	char *res;  // 用于存放最终路径的结果
	struct path root;  // 用于存放当前进程的根目录路径
	struct path tmp;  // 临时路径变量，用于调用 __d_path

	/*
	 * We have various synthetic filesystems that never get mounted.  On
	 * these filesystems dentries are never used for lookup purposes, and
	 * thus don't need to be hashed.  They also don't need a name until a
	 * user wants to identify the object in /proc/pid/fd/.  The little hack
	 * below allows us to generate a name for these objects on demand:
	 */
	/*
	 * 我们有一些合成文件系统从未被挂载。在这些文件系统上，
	 * 目录项从未用于查找目的，因此不需要被哈希化。它们也不需要一个名字，
	 * 直到用户想要在 /proc/pid/fd/ 中标识这个对象。
	 * 下面的小技巧允许我们按需为这些对象生成一个名字：
	 */
	if (path->dentry->d_op && path->dentry->d_op->d_dname)
		return path->dentry->d_op->d_dname(path->dentry, buf, buflen);  // 特殊的文件系统可以直接返回路径

	read_lock(&current->fs->lock);  // 读取当前进程的文件系统相关锁
	root = current->fs->root;  // 获取当前进程的根目录
	path_get(&root);  // 增加路径的引用计数
	read_unlock(&current->fs->lock);  // 解锁

	spin_lock(&dcache_lock);  // 获取目录项缓存锁
	tmp = root;  // 将 root 路径复制到临时变量 tmp
	res = __d_path(path, &tmp, buf, buflen);  // 获取路径字符串
	spin_unlock(&dcache_lock);  // 释放目录项缓存锁

	path_put(&root);  // 减少之前获取的路径的引用计数
	return res;  // 返回构造的路径或错误代码
}
EXPORT_SYMBOL(d_path);

/*
 * Helper function for dentry_operations.d_dname() members
 */
/* dentry_operations.d_dname() 成员的辅助函数 */
// 函数用于为目录项操作 (dentry_operations) 的 d_dname() 成员函数生成动态名称。这通常用于需要根据特定格式动态生成目录项名称的文件系统。
char *dynamic_dname(struct dentry *dentry, char *buffer, int buflen,
			const char *fmt, ...)
{
	va_list args;  // 定义一个 va_list 类型的变量，用于处理可变参数
	char temp[64];  // 临时缓冲区，用于存储格式化后的字符串
	int sz;  // 用于存储格式化字符串的长度

	va_start(args, fmt);  // 初始化 args，指向第一个可变参数
	sz = vsnprintf(temp, sizeof(temp), fmt, args) + 1;  // 格式化字符串，并计算格式化后的长度（包括终止符）
	va_end(args);  // 结束可变参数的获取

	if (sz > sizeof(temp) || sz > buflen)  // 如果格式化后的字符串长度超过临时缓冲区大小或目标缓冲区大小
		return ERR_PTR(-ENAMETOOLONG);  // 返回错误代码

	buffer += buflen - sz;  // 移动目标缓冲区指针，预留足够的空间来复制字符串
	return memcpy(buffer, temp, sz);  // 将格式化后的字符串复制到目标缓冲区的适当位置，并返回指向该字符串的指针
}

/*
 * Write full pathname from the root of the filesystem into the buffer.
 */
/*
 * 从文件系统的根目录开始将完整的路径名写入缓冲区。
 */
// 用于将从指定的目录项 (dentry) 开始到文件系统根目录的完整路径写入给定的缓冲区。
char *dentry_path(struct dentry *dentry, char *buf, int buflen)
{
	char *end = buf + buflen;  // 缓冲区的结束指针
	char *retval;  // 最终返回的字符串的起始位置

	spin_lock(&dcache_lock);  // 锁定dcache锁以保护目录项结构
	prepend(&end, &buflen, "\0", 1);  // 在缓冲区的末尾添加字符串结束符
	if (d_unlinked(dentry) &&
		(prepend(&end, &buflen, "//deleted", 9) != 0))  // 如果目录项已删除，尝试在路径前添加“//deleted”
			goto Elong;
	if (buflen < 1)  // 检查缓冲区是否还有空间
		goto Elong;
	/* Get '/' right */
	/* 确保路径以'/'字符开始 */
	retval = end-1;  // 设置返回值的开始位置在最后一个'/'字符处
	*retval = '/';

	while (!IS_ROOT(dentry)) {  // 遍历所有父目录项直到根目录项
		struct dentry *parent = dentry->d_parent;  // 获取当前目录项的父目录项

		prefetch(parent);  // 预取父目录项数据以提高访问效率
		if ((prepend_name(&end, &buflen, &dentry->d_name) != 0) ||  // 将目录项名称添加到路径中
		    (prepend(&end, &buflen, "/", 1) != 0))  // 在每个目录名称前添加'/'
			goto Elong;

		retval = end;  // 更新返回值的开始位置
		dentry = parent;  // 向上移动到父目录项
	}
	spin_unlock(&dcache_lock);  // 解锁dcache锁
	return retval;  // 返回生成的路径
Elong:
	spin_unlock(&dcache_lock);  // 如果路径太长，解锁dcache锁并返回错误
	return ERR_PTR(-ENAMETOOLONG);  // 返回路径太长的错误
}

/*
 * NOTE! The user-level library version returns a
 * character pointer. The kernel system call just
 * returns the length of the buffer filled (which
 * includes the ending '\0' character), or a negative
 * error value. So libc would do something like
 *
 *	char *getcwd(char * buf, size_t size)
 *	{
 *		int retval;
 *
 *		retval = sys_getcwd(buf, size);
 *		if (retval >= 0)
 *			return buf;
 *		errno = -retval;
 *		return NULL;
 *	}
 */
/*
 * 注意！用户级库版本返回一个字符指针。内核系统调用仅返回填充的缓冲区长度（包括结束的 '\0' 字符），
 * 或一个负的错误值。因此，libc可能会执行类似下面的操作：
 *
 *	char *getcwd(char * buf, size_t size)
 *	{
 *		int retval;
 *
 *		retval = sys_getcwd(buf, size);
 *		if (retval >= 0)
 *			return buf;
 *		errno = -retval;
 *		return NULL;
 *	}
 */
// 系统调用用于获取当前工作目录的完整路径并复制到用户空间提供的缓冲区中。
SYSCALL_DEFINE2(getcwd, char __user *, buf, unsigned long, size)
{
	int error;
	struct path pwd, root;
	char *page = (char *) __get_free_page(GFP_USER);  // 分配一个页面大小的内存

	if (!page)
		return -ENOMEM;  // 如果内存分配失败，则返回内存不足的错误

	read_lock(&current->fs->lock);  // 读锁当前进程的文件系统对象，以保护它的成员不被同时访问
	pwd = current->fs->pwd;  // 获取当前工作目录
	path_get(&pwd);  // 增加工作目录路径的引用计数
	root = current->fs->root;  // 获取根目录
	path_get(&root);  // 增加根目录路径的引用计数
	read_unlock(&current->fs->lock);  // 解锁

	error = -ENOENT;  // 默认错误为“无此文件或目录”
	spin_lock(&dcache_lock);  // 锁定目录项缓存
	if (!d_unlinked(pwd.dentry)) {  // 如果工作目录的目录项没有被删除
		unsigned long len;
		struct path tmp = root;
		char *cwd;

		cwd = __d_path(&pwd, &tmp, page, PAGE_SIZE);  // 获取当前工作目录的完整路径
		spin_unlock(&dcache_lock);  // 解锁目录项缓存

		error = PTR_ERR(cwd);  // 如果获取路径失败，设置错误代码
		if (IS_ERR(cwd))
			goto out;

		error = -ERANGE;  // 默认错误为“结果太大”
		len = PAGE_SIZE + page - cwd;  // 计算路径长度
		if (len <= size) {  // 如果用户缓冲区足够大
			error = len;  // 返回路径长度
			if (copy_to_user(buf, cwd, len))  // 将路径复制到用户缓冲区
				error = -EFAULT;  // 如果复制失败，设置错误代码
		}
	} else
		spin_unlock(&dcache_lock);  // 如果工作目录的目录项已被删除，解锁目录项缓存

out:
	path_put(&pwd);  // 释放工作目录路径
	path_put(&root);  // 释放根目录路径
	free_page((unsigned long) page);  // 释放之前分配的页面
	return error;  // 返回操作结果
}

/*
 * Test whether new_dentry is a subdirectory of old_dentry.
 *
 * Trivially implemented using the dcache structure
 */
/*
 * 测试 new_dentry 是否是 old_dentry 的子目录。
 *
 * 使用 dcache 结构进行简单实现
 */

/**
 * is_subdir - is new dentry a subdirectory of old_dentry
 * @new_dentry: new dentry
 * @old_dentry: old dentry
 *
 * Returns 1 if new_dentry is a subdirectory of the parent (at any depth).
 * Returns 0 otherwise.
 * Caller must ensure that "new_dentry" is pinned before calling is_subdir()
 */
/**
 * is_subdir - 检查新目录项是否是旧目录项的子目录
 * @new_dentry: 新目录项
 * @old_dentry: 旧目录项
 *
 * 如果 new_dentry 是父目录 (old_dentry) 的子目录（在任何深度上），则返回 1。
 * 否则返回 0。
 * 调用者必须确保在调用 is_subdir() 之前已经固定了 "new_dentry"
 */
// 用来检测一个目录项是否是另一个目录项的子目录
int is_subdir(struct dentry *new_dentry, struct dentry *old_dentry)
{
	int result;
	unsigned long seq;

	if (new_dentry == old_dentry)
		return 1;  // 如果新旧目录项是相同的，直接返回 1

	/*
	 * Need rcu_readlock to protect against the d_parent trashing
	 * due to d_move
	 */
	/*
	 * 需要 rcu_read_lock 来防止因 d_move 操作而导致的 d_parent 变化
	 */
	rcu_read_lock();
	do {
		/* for restarting inner loop in case of seq retry */
		/* 如果重试序列检查失败，重新开始内部循环 */
		seq = read_seqbegin(&rename_lock);
		if (d_ancestor(old_dentry, new_dentry))
			result = 1;  // 如果 old_dentry 是 new_dentry 的祖先，则返回 1
		else
			result = 0;  // 否则返回 0
	} while (read_seqretry(&rename_lock, seq));  // 如果检测到序列更改，重试
	rcu_read_unlock();

	return result;  // 返回检测结果
}

/**
 * path_is_under - 判断一个路径是否在另一个路径下
 * @path1: 待检查的路径
 * @path2: 参照路径
 *
 * 检查 path1 是否位于 path2 的目录结构下。
 * 如果 path1 在 path2 的目录树下，返回 1，否则返回 0。
 */
// 用来判断一个文件系统路径（path1）是否位于另一个路径（path2）的目录结构下。
int path_is_under(struct path *path1, struct path *path2)
{
	struct vfsmount *mnt = path1->mnt;
	struct dentry *dentry = path1->dentry;
	int res;

	spin_lock(&vfsmount_lock); // 锁定挂载点，保证挂载点的安全访问

	if (mnt != path2->mnt) { // 如果 path1 和 path2 不在同一个挂载点
		for (;;) { // 进行循环，向上查找父挂载点
			if (mnt->mnt_parent == mnt) { // 如果父挂载点是其自身，说明到达了挂载树的根部
				spin_unlock(&vfsmount_lock); // 解锁
				return 0; // 返回 0，表示 path1 不在 path2 下
			}
			if (mnt->mnt_parent == path2->mnt) // 如果找到了 path2 的挂载点
				break; // 跳出循环
			mnt = mnt->mnt_parent; // 向上继续查找
		}
		dentry = mnt->mnt_mountpoint; // 更新 dentry 为找到的挂载点的挂载目录
	}
	res = is_subdir(dentry, path2->dentry); // 调用 is_subdir 检查 dentry 是否是 path2->dentry 的子目录
	spin_unlock(&vfsmount_lock); // 解锁
	return res; // 返回检查结果
}
EXPORT_SYMBOL(path_is_under);

/**
 * d_genocide - 递归删除一个目录项及其所有子目录项
 * @root: 根目录项
 *
 * 这个函数递归地遍历给定的目录项下的所有子目录项，并且递减它们的引用计数，
 * 准备它们被释放。这通常在卸载文件系统或者清理子目录树时使用。
 */
// 用于递归遍历并减少给定目录项 root 及其所有子目录项的引用计数，为目录项的释放做准备
void d_genocide(struct dentry *root)
{
	struct dentry *this_parent = root;  // 开始的父目录项
	struct list_head *next;  // 用于迭代目录项的子目录项列表

	spin_lock(&dcache_lock);  // 加锁，确保目录项缓存的完整性
repeat:
	next = this_parent->d_subdirs.next;  // 获取子目录项列表的下一个元素
resume:
	while (next != &this_parent->d_subdirs) {  // 遍历所有子目录项
		struct list_head *tmp = next;
		struct dentry *dentry = list_entry(tmp, struct dentry, d_u.d_child);  // 获取当前子目录项
		next = tmp->next;  // 移动到下一个子目录项

		if (d_unhashed(dentry) || !dentry->d_inode)  // 如果目录项未被哈希或没有关联的inode，则跳过
			continue;

		if (!list_empty(&dentry->d_subdirs)) {  // 如果当前目录项有子目录项
			this_parent = dentry;  // 设定当前目录项为新的父目录项
			goto repeat;  // 递归处理
		}

		atomic_dec(&dentry->d_count);  // 减少当前目录项的引用计数
	}
	if (this_parent != root) {  // 如果当前父目录项不是起始的根目录项
		next = this_parent->d_u.d_child.next;  // 获取父目录项的下一个子目录项
		atomic_dec(&this_parent->d_count);  // 减少父目录项的引用计数
		this_parent = this_parent->d_parent;  // 回溯到上一级目录项
		goto resume;  // 继续处理
	}
	spin_unlock(&dcache_lock);  // 解锁
}

/**
 * find_inode_number - check for dentry with name
 * @dir: directory to check
 * @name: Name to find.
 *
 * Check whether a dentry already exists for the given name,
 * and return the inode number if it has an inode. Otherwise
 * 0 is returned.
 *
 * This routine is used to post-process directory listings for
 * filesystems using synthetic inode numbers, and is necessary
 * to keep getcwd() working.
 */
/**
 * find_inode_number - 检查是否有具有指定名称的目录项
 * @dir: 要检查的目录
 * @name: 要查找的名称
 *
 * 检查给定名称的目录项是否已存在，并在存在 inode 时返回 inode 编号。
 * 如果不存在，则返回 0。
 *
 * 这个函数用于为使用合成 inode 编号的文件系统后处理目录列表，
 * 并且是保持 getcwd() 正常工作所必需的。
 */
ino_t find_inode_number(struct dentry *dir, struct qstr *name)
{
	struct dentry *dentry;  // 用于存储查找到的目录项
	ino_t ino = 0;  // 初始化 inode 编号为 0，表示未找到

	// 使用 d_hash_and_lookup 函数查找具有特定名称的目录项
	dentry = d_hash_and_lookup(dir, name);
	if (dentry) {  // 如果找到了目录项
		if (dentry->d_inode)  // 并且目录项关联了一个 inode
			ino = dentry->d_inode->i_ino;  // 获取 inode 编号
		dput(dentry);  // 释放获取的目录项引用
	}
	return ino;  // 返回找到的 inode 编号，如果未找到或目录项没有关联 inode，则返回 0
}
EXPORT_SYMBOL(find_inode_number);

/*
 * 使用 __initdata 标记，表明 dhash_entries 变量在初始化后不再需要，可以释放其内存。
 */
static __initdata unsigned long dhash_entries;
/*
 * set_dhash_entries - 初始化目录项哈希表的条目数
 * @str: 从内核命令行传入的dhash_entries值的字符串表示
 *
 * 解析内核命令行参数 "dhash_entries=" 后的数字，并将其存储在全局变量 dhash_entries 中。
 * 如果没有提供参数，则不做任何操作。
 *
 * 返回 1 表示参数被成功处理，0 表示没有提供参数。
 */
static int __init set_dhash_entries(char *str)
{
	if (!str)
		return 0;  // 没有提供参数，直接返回 0
	dhash_entries = simple_strtoul(str, &str, 0);  // 将字符串转换为无符号长整数
	return 1;  // 参数成功处理，返回 1
}
/*
 * __setup 宏用于处理特定的内核命令行参数。
 * "dhash_entries=" 是当内核启动时通过命令行提供的参数，
 * set_dhash_entries 是处理该参数的函数。
 */
__setup("dhash_entries=", set_dhash_entries);

/*
 * dcache_init_early - 早期初始化目录项缓存
 *
 * 在系统启动过程中的早期阶段初始化目录项哈希表。
 * 此函数在内核内存管理完全启动之前调用，因此使用的是特定的早期内存分配策略。
 */
static void __init dcache_init_early(void)
{
	int loop;

	/* If hashes are distributed across NUMA nodes, defer
	 * hash allocation until vmalloc space is available.
	 */
	/*
	 * 如果哈希表分布于NUMA节点之间，则推迟哈希表分配
	 * 直到vmalloc空间可用。
	 */
	if (hashdist)  // 如果hashdist为真，表明哈希表应跨NUMA节点分布
		return;  // 在此阶段不进行哈希表分配，直接返回

	/*
	 * 分配目录项哈希表。
	 * alloc_large_system_hash 是一个内核函数，用于分配一个大型的系统哈希表。
	 * "Dentry cache" 是哈希表的名称。
	 * sizeof(struct hlist_head) 是哈希表每个元素的大小。
	 * dhash_entries 是从命令行解析得到的哈希表大小。
	 * 13 是默认的哈希表大小指数（如果没有提供 dhash_entries）。
	 * HASH_EARLY 表示使用早期内存分配策略。
	 * &d_hash_shift 是哈希表大小的位移量的存储位置。
	 * &d_hash_mask 是哈希掩码的存储位置。
	 * 0 是哈希表大小的最小值（如果为 0 则不设最小值）。
	 */
	// 哈希表维护在内存中的所有目录项，哈希表中每个元素都是一个双向循环链表，用于维护哈希值相等的目录项，
	// 这个双向循环链表是通过dentry中的d_hash成员来链接在一起的。
	dentry_hashtable =
		alloc_large_system_hash("Dentry cache",
					sizeof(struct hlist_head),
					dhash_entries,
					13,
					HASH_EARLY,
					&d_hash_shift,
					&d_hash_mask,
					0);

	/*
	 * 初始化哈希表中的所有链表头。
	 * 通过循环遍历哈希表中的每个桶（bucket），并对每个桶的链表头进行初始化。
	 */
	for (loop = 0; loop < (1 << d_hash_shift); loop++)
		INIT_HLIST_HEAD(&dentry_hashtable[loop]);
}

/*
 * dcache_init - 初始化目录项缓存
 *
 * 这个函数在系统启动时调用，完成目录项缓存的设置和初始化。
 */
static void __init dcache_init(void)
{
	int loop;

	/* 
	 * A constructor could be added for stable state like the lists,
	 * but it is probably not worth it because of the cache nature
	 * of the dcache. 
	 */
	/* 
	 * 可以为如列表这样的稳定状态添加一个构造函数，
	 * 但由于目录项缓存的缓存性质，可能不值得这样做。
	 */
	// 初始化目录项缓存池
	// SLAB_RECLAIM_ACCOUNT：记录对象的数量，以便回收内存时可以调整。
	// SLAB_PANIC：如果无法分配内存，则内核会出现panic。
	// SLAB_MEM_SPREAD：在NUMA系统中尝试在不同的节点间分散内存页。
	dentry_cache = KMEM_CACHE(dentry,
		SLAB_RECLAIM_ACCOUNT|SLAB_PANIC|SLAB_MEM_SPREAD);
	
	// 注册缩减器，以便在内存紧张时减少目录项缓存的使用
	register_shrinker(&dcache_shrinker);

	/* Hash may have been set up in dcache_init_early */
	/* 如果哈希表在dcache_init_early中已经设置，则无需再次设置 */
	if (!hashdist)
		return;

	/*
	 * 分配大型系统哈希表。
	 * "Dentry cache" 是哈希表的名称。
	 * sizeof(struct hlist_head) 是哈希表每个元素的大小。
	 * dhash_entries 是命令行或系统设置的预期哈希表大小。
	 * 13 是默认的哈希表大小指数。
	 * 0 是分配标志，表示常规内存分配。
	 */	
	dentry_hashtable =
		alloc_large_system_hash("Dentry cache",
					sizeof(struct hlist_head),
					dhash_entries,
					13,
					0,
					&d_hash_shift,
					&d_hash_mask,
					0);

	// 初始化哈希表的每个桶
	for (loop = 0; loop < (1 << d_hash_shift); loop++)
		INIT_HLIST_HEAD(&dentry_hashtable[loop]);
}

/* SLAB cache for __getname() consumers */
/* SLAB缓存用于__getname()函数的使用者 */
struct kmem_cache *names_cachep __read_mostly;
EXPORT_SYMBOL(names_cachep);

EXPORT_SYMBOL(d_genocide);

/* 提前初始化VFS相关的缓存 */
void __init vfs_caches_init_early(void)
{
	dcache_init_early();  // 提前初始化目录项缓存
	inode_init_early();   // 提前初始化inode缓存
}

// 内核初始化阶段设置和配置各种VFS（虚拟文件系统）相关的缓存

/* 初始化VFS缓存 */
void __init vfs_caches_init(unsigned long mempages)
{
	unsigned long reserve;

	/* Base hash sizes on available memory, with a reserve equal to
           150% of current kernel size */
	/* 根据可用内存确定基本的哈希大小，保留量等于当前内核大小的150% */
	// 计算保留内存，不让系统分配的内存超过当前空闲内存的150%
	reserve = min((mempages - nr_free_pages()) * 3/2, mempages - 1);
	mempages -= reserve;  // 从总内存中减去保留内存，得到可用于缓存的内存

	/* 创建用于文件名缓存的SLAB缓存 */
	names_cachep = kmem_cache_create("names_cache", PATH_MAX, 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL);

	dcache_init();  // 初始化目录项缓存
	inode_init();   // 初始化inode缓存
	files_init(mempages);  // 初始化文件描述符表
	mnt_init();  // 初始化挂载点缓存
	bdev_cache_init();  // 初始化块设备缓存
	chrdev_init();  // 初始化字符设备缓存
}
