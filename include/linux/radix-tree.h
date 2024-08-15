/*
 * Copyright (C) 2001 Momchil Velikov
 * Portions Copyright (C) 2001 Christoph Hellwig
 * Copyright (C) 2006 Nick Piggin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2, or (at
 * your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#ifndef _LINUX_RADIX_TREE_H
#define _LINUX_RADIX_TREE_H

#include <linux/preempt.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/rcupdate.h>

/*
 * An indirect pointer (root->rnode pointing to a radix_tree_node, rather
 * than a data item) is signalled by the low bit set in the root->rnode
 * pointer.
 *
 * In this case root->height is > 0, but the indirect pointer tests are
 * needed for RCU lookups (because root->height is unreliable). The only
 * time callers need worry about this is when doing a lookup_slot under
 * RCU.
 */
/*
 * 一个间接指针（root->rnode 指向一个 radix_tree_node 而非数据项）
 * 通过 root->rnode 指针的最低位被设置来表示。
 *
 * 在这种情况下 root->height 大于 0，但是对间接指针的测试
 * 对于 RCU 查找是必要的（因为 root->height 是不可靠的）。唯一需要
 * 关心这个问题的时候是在 RCU 下进行 lookup_slot 操作时。
 */
#define RADIX_TREE_INDIRECT_PTR	1	// 定义标志位，表示间接指针
#define RADIX_TREE_RETRY ((void *)-1UL)	// 定义重试的特殊指针，通常用于操作失败或特殊情况

// 将指针转换为表示间接指针的形式，通过设置其最低位
static inline void *radix_tree_ptr_to_indirect(void *ptr)
{
	return (void *)((unsigned long)ptr | RADIX_TREE_INDIRECT_PTR);	// 返回设置了最低位的指针地址
}

// 从表示间接指针的形式恢复原始指针，通过清除最低位
static inline void *radix_tree_indirect_to_ptr(void *ptr)
{
	return (void *)((unsigned long)ptr & ~RADIX_TREE_INDIRECT_PTR);	// 返回清除了最低位的指针地址
}

// 检查指针是否为间接指针，即检查其最低位是否被设置
static inline int radix_tree_is_indirect_ptr(void *ptr)
{
	return (int)((unsigned long)ptr & RADIX_TREE_INDIRECT_PTR);	// 检查并返回是否设置了最低位
}

/*** radix-tree API starts here ***/
// 基数树API从这里开始

#define RADIX_TREE_MAX_TAGS 2	// 定义基数树最多可以有两个标签

/* root tags are stored in gfp_mask, shifted by __GFP_BITS_SHIFT */
// 根节点的标签存储在gfp_mask中，通过__GFP_BITS_SHIFT进行位移
struct radix_tree_root {
	unsigned int		height;  // 树的高度
	gfp_t			gfp_mask;  // GFP掩码，用于内存分配控制
	struct radix_tree_node	*rnode;  // 指向树根节点的指针
};

// 宏定义初始化一个基数树的根，设置高度为0，gfp_mask为指定值，根节点指针为NULL
#define RADIX_TREE_INIT(mask)	{					\
	.height = 0,							\
	.gfp_mask = (mask),						\
	.rnode = NULL,							\
}

// 宏定义创建并初始化一个名为name的基数树变量，使用指定的内存分配掩码
#define RADIX_TREE(name, mask) \
	struct radix_tree_root name = RADIX_TREE_INIT(mask)

// 宏定义在函数内初始化一个基数树root，将高度设置为0，gfp_mask设置为mask，根节点设置为NULL
#define INIT_RADIX_TREE(root, mask)					\
do {									\
	(root)->height = 0;						\
	(root)->gfp_mask = (mask);					\
	(root)->rnode = NULL;						\
} while (0)

/**
 * Radix-tree synchronization
 *
 * The radix-tree API requires that users provide all synchronisation (with
 * specific exceptions, noted below).
 *
 * Synchronization of access to the data items being stored in the tree, and
 * management of their lifetimes must be completely managed by API users.
 *
 * For API usage, in general,
 * - any function _modifying_ the tree or tags (inserting or deleting
 *   items, setting or clearing tags) must exclude other modifications, and
 *   exclude any functions reading the tree.
 * - any function _reading_ the tree or tags (looking up items or tags,
 *   gang lookups) must exclude modifications to the tree, but may occur
 *   concurrently with other readers.
 *
 * The notable exceptions to this rule are the following functions:
 * radix_tree_lookup
 * radix_tree_lookup_slot
 * radix_tree_tag_get
 * radix_tree_gang_lookup
 * radix_tree_gang_lookup_slot
 * radix_tree_gang_lookup_tag
 * radix_tree_gang_lookup_tag_slot
 * radix_tree_tagged
 *
 * The first 7 functions are able to be called locklessly, using RCU. The
 * caller must ensure calls to these functions are made within rcu_read_lock()
 * regions. Other readers (lock-free or otherwise) and modifications may be
 * running concurrently.
 *
 * It is still required that the caller manage the synchronization and lifetimes
 * of the items. So if RCU lock-free lookups are used, typically this would mean
 * that the items have their own locks, or are amenable to lock-free access; and
 * that the items are freed by RCU (or only freed after having been deleted from
 * the radix tree *and* a synchronize_rcu() grace period).
 *
 * (Note, rcu_assign_pointer and rcu_dereference are not needed to control
 * access to data items when inserting into or looking up from the radix tree)
 *
 * Note that the value returned by radix_tree_tag_get() may not be relied upon
 * if only the RCU read lock is held.  Functions to set/clear tags and to
 * delete nodes running concurrently with it may affect its result such that
 * two consecutive reads in the same locked section may return different
 * values.  If reliability is required, modification functions must also be
 * excluded from concurrency.
 *
 * radix_tree_tagged is able to be called without locking or RCU.
 */
/**
 * 基数树同步
 *
 * 基数树 API 要求用户提供所有的同步（具体例外见下文）。
 *
 * 访问树中存储的数据项及其生命周期的同步必须完全由 API 用户管理。
 *
 * 通常情况下，API 使用规则如下：
 * - 任何_修改_树或标签的函数（插入或删除项目，设置或清除标签）必须排除其他修改，并排除任何读取树的函数。
 * - 任何_读取_树或标签的函数（查找项目或标签，批量查找）必须排除对树的修改，但可以与其他读取者并发执行。
 *
 * 此规则的显著例外是以下函数：
 * radix_tree_lookup
 * radix_tree_lookup_slot
 * radix_tree_tag_get
 * radix_tree_gang_lookup
 * radix_tree_gang_lookup_slot
 * radix_tree_gang_lookup_tag
 * radix_tree_gang_lookup_tag_slot
 * radix_tree_tagged
 *
 * 前7个函数可以在没有锁的情况下使用 RCU 调用。调用者必须确保在 rcu_read_lock() 区域内调用这些函数。其他读取者（无锁或其他方式）和修改可能会同时进行。
 *
 * 调用者仍然需要管理项的同步和生命周期。因此，如果使用 RCU 无锁查找，通常意味着项目具有自己的锁，或适合无锁访问；并且项目通过 RCU 释放（或只在从基数树删除 *并* 经过 synchronize_rcu() 宽限期后释放）。
 *
 * （注意，插入或查找基数树时，不需要使用 rcu_assign_pointer 和 rcu_dereference 来控制对数据项的访问）
 *
 * 注意，如果只持有 RCU 读锁，不能依赖 radix_tree_tag_get() 返回的值。同时运行的设置/清除标签和删除节点的函数可能会影响其结果，以致于在同一锁定区段内的两次连续读取可能返回不同的值。如果需要可靠性，也必须排除修改函数的并发。
 *
 * radix_tree_tagged 可以在没有锁定或 RCU 的情况下调用。
 */

/**
 * radix_tree_deref_slot	- dereference a slot
 * @pslot:	pointer to slot, returned by radix_tree_lookup_slot
 * Returns:	item that was stored in that slot with any direct pointer flag
 *		removed.
 *
 * For use with radix_tree_lookup_slot().  Caller must hold tree at least read
 * locked across slot lookup and dereference.  More likely, will be used with
 * radix_tree_replace_slot(), as well, so caller will hold tree write locked.
 */
/**
 * radix_tree_deref_slot - 取消引用槽
 * @pslot: 指向槽的指针，由 radix_tree_lookup_slot 返回
 * 返回：存储在该槽中的项，已移除任何直接指针标志。
 *
 * 用于 radix_tree_lookup_slot()。调用者必须至少在槽查找和取消引用过程中对树进行读锁定。更可能的是，将与 radix_tree_replace_slot() 一起使用，因此调用者将对树进行写锁定。
 */
static inline void *radix_tree_deref_slot(void **pslot)
{
	void *ret = rcu_dereference(*pslot);  // 使用 RCU 解引用槽中的指针
	if (unlikely(radix_tree_is_indirect_ptr(ret)))
		ret = RADIX_TREE_RETRY;  // 如果是间接指针，则返回重试特殊值
	return ret;  // 返回解引用的结果
}
/**
 * radix_tree_replace_slot	- replace item in a slot
 * @pslot:	pointer to slot, returned by radix_tree_lookup_slot
 * @item:	new item to store in the slot.
 *
 * For use with radix_tree_lookup_slot().  Caller must hold tree write locked
 * across slot lookup and replacement.
 */
/**
 * radix_tree_replace_slot - 在槽中替换项
 * @pslot: 指向槽的指针，由 radix_tree_lookup_slot 返回
 * @item: 要存储在槽中的新项。
 *
 * 用于 radix_tree_lookup_slot()。调用者必须在槽查找和替换过程中持有树的写锁。
 */
static inline void radix_tree_replace_slot(void **pslot, void *item)
{
	BUG_ON(radix_tree_is_indirect_ptr(item));	// 检查item是否为间接指针，如果是则触发BUG_ON，即内核崩溃
	rcu_assign_pointer(*pslot, item);		// 安全地将新项分配给槽，使用RCU机制
}

// 基数树操作函数的原型和实现定义，包括插入、查找、删除和特定的批量查找等功能。

// 插入一个项到基数树中
int radix_tree_insert(struct radix_tree_root *, unsigned long, void *);
// 查找基数树中的一个项
void *radix_tree_lookup(struct radix_tree_root *, unsigned long);
// 查找基数树中的一个槽，返回指向槽的指针
void **radix_tree_lookup_slot(struct radix_tree_root *, unsigned long);
// 从基数树中删除一个项
void *radix_tree_delete(struct radix_tree_root *, unsigned long);
// 从基数树中批量查找项
unsigned int
radix_tree_gang_lookup(struct radix_tree_root *root, void **results,
			unsigned long first_index, unsigned int max_items);
// 从基数树中批量查找槽位
unsigned int
radix_tree_gang_lookup_slot(struct radix_tree_root *root, void ***results,
			unsigned long first_index, unsigned int max_items);
// 查找基数树中的下一个空槽
unsigned long radix_tree_next_hole(struct radix_tree_root *root,
				unsigned long index, unsigned long max_scan);
// 查找基数树中的上一个空槽
unsigned long radix_tree_prev_hole(struct radix_tree_root *root,
				unsigned long index, unsigned long max_scan);
// 预加载基数树，用于性能优化
int radix_tree_preload(gfp_t gfp_mask);
// 初始化基数树模块
void radix_tree_init(void);
// 设置基数树中某项的标签
void *radix_tree_tag_set(struct radix_tree_root *root,
			unsigned long index, unsigned int tag);
// 清除基数树中某项的标签
void *radix_tree_tag_clear(struct radix_tree_root *root,
			unsigned long index, unsigned int tag);
// 获取基数树中某项的标签状态
int radix_tree_tag_get(struct radix_tree_root *root,
			unsigned long index, unsigned int tag);
// 基于标签的批量查找
unsigned int
radix_tree_gang_lookup_tag(struct radix_tree_root *root, void **results,
		unsigned long first_index, unsigned int max_items,
		unsigned int tag);
// 基于标签的批量查找槽位
unsigned int
radix_tree_gang_lookup_tag_slot(struct radix_tree_root *root, void ***results,
		unsigned long first_index, unsigned int max_items,
		unsigned int tag);
// 检查基数树中是否有设置了特定标签的项
int radix_tree_tagged(struct radix_tree_root *root, unsigned int tag);

// 结束基数树的预加载状态
static inline void radix_tree_preload_end(void)
{
	preempt_enable();	// 重新启用抢占
}

#endif /* _LINUX_RADIX_TREE_H */
