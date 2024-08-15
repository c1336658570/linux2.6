/*
 * Copyright (C) 2001 Momchil Velikov
 * Portions Copyright (C) 2001 Christoph Hellwig
 * Copyright (C) 2005 SGI, Christoph Lameter
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

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/radix-tree.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/string.h>
#include <linux/bitops.h>
#include <linux/rcupdate.h>


// radix树通过long型的位操作来查询各个节点， 存储效率高，并且可以快速查询。

// 根据编译环境设置RADIX_TREE_MAP_SHIFT值。在内核编译环境中，根据CONFIG_BASE_SMALL配置，该值可以是4或6。
// 在非内核环境下，用于更加严格的测试，该值被设置为3。
#ifdef __KERNEL__
#define RADIX_TREE_MAP_SHIFT	(CONFIG_BASE_SMALL ? 4 : 6)
#else
#define RADIX_TREE_MAP_SHIFT	3	/* For more stressful testing */
#endif

// 定义基数树节点中槽位的数量，通过将1左移RADIX_TREE_MAP_SHIFT位来计算。
#define RADIX_TREE_MAP_SIZE	(1UL << RADIX_TREE_MAP_SHIFT)
// 定义一个掩码用于索引节点中的槽位，其值为槽位数量减一。
#define RADIX_TREE_MAP_MASK	(RADIX_TREE_MAP_SIZE-1)

// 计算存储标签所需的unsigned long数组的长度。这里确保有足够的空间存放所有位。
#define RADIX_TREE_TAG_LONGS	\
	((RADIX_TREE_MAP_SIZE + BITS_PER_LONG - 1) / BITS_PER_LONG)

struct radix_tree_node {
	// height 表示的整个 radix树的高度（即叶子节点到树根的高度），
	// 不是当前节点到树根的高度
	// 节点的高度，从底部计算
	/* radix树的高度 */
	unsigned int	height;		/* Height from the bottom */
	// 表示当前节点的子节点个数，叶子节点的 count=0
	// 节点中存储的元素数量
	/* 当前节点的子节点数目 */
	unsigned int	count;
	// RCU发生时触发的回调函数链表
	/* RCU 回调函数链表 */
	struct rcu_head	rcu_head;
	// 每个slot对应一个子节点（叶子节点）
	// 指针数组，每个元素指向子树节点或数据项
	/* 节点中的slot数组 */
	void		*slots[RADIX_TREE_MAP_SIZE];
	// 标记子节点是否 dirty 或者 wirteback
	// 标签数组，用于标记特定状态或属性，例如脏数据或锁定状态
	/* slot标签 */
	unsigned long	tags[RADIX_TREE_MAX_TAGS][RADIX_TREE_TAG_LONGS];
};

struct radix_tree_path {
	struct radix_tree_node *node;	// 指向当前节点的指针
	int offset;	// 当前节点在其父节点slots数组中的偏移
};

// 定义每个unsigned long能表示的位数，通常用于计算索引的位数
#define RADIX_TREE_INDEX_BITS  (8 /* CHAR_BIT */ * sizeof(unsigned long))
// 计算基数树的最大可能高度，使用索引位数除以每个节点的分支因子（通过MAP_SHIFT确定）
#define RADIX_TREE_MAX_PATH (DIV_ROUND_UP(RADIX_TREE_INDEX_BITS, \
					  RADIX_TREE_MAP_SHIFT))

/*
 * The height_to_maxindex array needs to be one deeper than the maximum
 * path as height 0 holds only 1 entry.
 */
/*
 * height_to_maxindex 数组的深度需要比最大路径深一层，因为高度为0的时候只持有1个条目。
 */
static unsigned long height_to_maxindex[RADIX_TREE_MAX_PATH + 1] __read_mostly;

/*
 * Radix tree node cache.
 */
/*
 * 基数树节点缓存。
 */
// 定义一个内核内存缓存指针，用于基数树节点的内存管理
static struct kmem_cache *radix_tree_node_cachep;

/*
 * Per-cpu pool of preloaded nodes
 */
/*
 * 每个CPU的预加载节点池
 */
struct radix_tree_preload {
	int nr;	// 预加载节点的数量
	struct radix_tree_node *nodes[RADIX_TREE_MAX_PATH];	// 预加载节点数组
};
// 使用DEFINE_PER_CPU宏定义每个CPU的预加载节点结构，初始值为0
static DEFINE_PER_CPU(struct radix_tree_preload, radix_tree_preloads) = { 0, };

static inline gfp_t root_gfp_mask(struct radix_tree_root *root)
{
	return root->gfp_mask & __GFP_BITS_MASK;	// 返回基数树根节点的GFP掩码，只保留有效的GFP位
}

static inline void tag_set(struct radix_tree_node *node, unsigned int tag,
		int offset)
{
	__set_bit(offset, node->tags[tag]);	// 设置节点的标签数组中的特定标签位，标志节点中的某个位置
}

static inline void tag_clear(struct radix_tree_node *node, unsigned int tag,
		int offset)
{
	__clear_bit(offset, node->tags[tag]);	// 清除节点的标签数组中的特定标签位，解除对某个位置的标志
}

static inline int tag_get(struct radix_tree_node *node, unsigned int tag,
		int offset)
{
	return test_bit(offset, node->tags[tag]);	// 检测节点的标签数组中的特定标签位是否已设置，并返回其状态
}

static inline void root_tag_set(struct radix_tree_root *root, unsigned int tag)
{
	// 在根节点的GFP掩码中设置特定的标签位，使用强制类型转换以确保操作的正确性
	root->gfp_mask |= (__force gfp_t)(1 << (tag + __GFP_BITS_SHIFT));
}

static inline void root_tag_clear(struct radix_tree_root *root, unsigned int tag)
{
	// 清除根节点的gfp_mask中指定的标签位。通过左移tag值加上偏移量__GFP_BITS_SHIFT，
	// 然后对结果取反以清除特定位，最后使用按位与更新root->gfp_mask。
	root->gfp_mask &= (__force gfp_t)~(1 << (tag + __GFP_BITS_SHIFT));
}

static inline void root_tag_clear_all(struct radix_tree_root *root)
{
	// 清除根节点的所有标签位。通过与__GFP_BITS_MASK进行按位与操作，
	// 只保留有效的GFP位，从而清除所有自定义或非标准标签位。
	root->gfp_mask &= __GFP_BITS_MASK;
}

static inline int root_tag_get(struct radix_tree_root *root, unsigned int tag)
{
	// 获取根节点的gfp_mask中指定的标签位状态。通过左移tag值加上偏移量__GFP_BITS_SHIFT，
	// 然后与root->gfp_mask进行按位与操作，如果指定位为1则返回非零值（通常为真），
	// 否则返回0（假）。这里使用__force转换确保类型正确。
	return (__force unsigned)root->gfp_mask & (1 << (tag + __GFP_BITS_SHIFT));
}

/*
 * Returns 1 if any slot in the node has this tag set.
 * Otherwise returns 0.
 */
/*
 * 如果节点中的任何槽位设置了此标签，则返回1。
 * 否则返回0。
 */
static inline int any_tag_set(struct radix_tree_node *node, unsigned int tag)
{
	int idx;	// 用于循环遍历标签数组的索引变量
	for (idx = 0; idx < RADIX_TREE_TAG_LONGS; idx++) {	// 遍历标签数组中的所有元素
		if (node->tags[tag][idx])	// 检查当前标签位组中的标签位是否被设置
			return 1;	// 如果任何标签位被设置，立即返回1
	}
	return 0;	// 如果遍历完成后未找到任何设置的标签位，返回0
}
/*
 * This assumes that the caller has performed appropriate preallocation, and
 * that the caller has pinned this thread of control to the current CPU.
 */
/*
 * 这假设调用者已经执行了适当的预分配，
 * 并且调用者已经将此控制线程固定到当前CPU。
 */
static struct radix_tree_node *
radix_tree_node_alloc(struct radix_tree_root *root)
{
	struct radix_tree_node *ret = NULL;	// 初始化返回的节点指针为NULL
	gfp_t gfp_mask = root_gfp_mask(root);	// 获取根节点的GFP掩码

	if (!(gfp_mask & __GFP_WAIT)) {	// 如果GFP掩码中不包含等待标志
		struct radix_tree_preload *rtp;

		/*
		 * Provided the caller has preloaded here, we will always
		 * succeed in getting a node here (and never reach
		 * kmem_cache_alloc)
		 */
		/*
		 * 如果调用者已经进行了预加载，我们将始终在这里成功获取节点
		 * （并且永远不会达到kmem_cache_alloc）
		 */
		rtp = &__get_cpu_var(radix_tree_preloads);	// 获取当前CPU的预加载结构
		if (rtp->nr) {	// 如果预加载结构中有节点可用
			ret = rtp->nodes[rtp->nr - 1];	// 从预加载数组中获取一个节点
			rtp->nodes[rtp->nr - 1] = NULL;	// 将该位置置为空
			rtp->nr--;	// 预加载节点计数减一
		}
	}
	if (ret == NULL)	// 如果没有从预加载中获取到节点
		ret = kmem_cache_alloc(radix_tree_node_cachep, gfp_mask);	// 从内核内存缓存分配一个节点

	BUG_ON(radix_tree_is_indirect_ptr(ret));	// 确保返回的节点不是一个间接指针
	return ret;	// 返回节点
}

static void radix_tree_node_rcu_free(struct rcu_head *head)
{
	// 通过rcu_head指针获取到实际的radix_tree_node结构指针
	struct radix_tree_node *node =
			container_of(head, struct radix_tree_node, rcu_head);

	/*
	 * must only free zeroed nodes into the slab. radix_tree_shrink
	 * can leave us with a non-NULL entry in the first slot, so clear
	 * that here to make sure.
	 */
	/*
	 * 必须只将清零的节点释放回slab（内存缓存）。radix_tree_shrink
	 * 可能会留下第一个槽中的非NULL条目，因此在这里清除以确保安全。
	 */
	// 清除节点的标签位，确保在释放前节点状态是干净的
	tag_clear(node, 0, 0);  // 清除第一个标签组的第一个标签位
	tag_clear(node, 1, 0);  // 清除第二个标签组的第一个标签位
	node->slots[0] = NULL;  // 清空节点的第一个槽位
	node->count = 0;  // 将节点的计数器设置为0

	// 释放节点回内存缓存
	kmem_cache_free(radix_tree_node_cachep, node);
}

static inline void
radix_tree_node_free(struct radix_tree_node *node)
{
	// 使用RCU（Read-Copy-Update）机制安全释放基数树节点
	call_rcu(&node->rcu_head, radix_tree_node_rcu_free);	// 调度RCU回调函数radix_tree_node_rcu_free以延迟释放节点
}

/*
 * Load up this CPU's radix_tree_node buffer with sufficient objects to
 * ensure that the addition of a single element in the tree cannot fail.  On
 * success, return zero, with preemption disabled.  On error, return -ENOMEM
 * with preemption not disabled.
 *
 * To make use of this facility, the radix tree must be initialised without
 * __GFP_WAIT being passed to INIT_RADIX_TREE().
 */
/*
 * 为当前CPU的基数树节点缓冲区加载足够的对象，以确保树中添加单个元素不会失败。
 * 成功时返回零，并禁用抢占。出错时返回-ENOMEM，并不禁用抢占。
 *
 * 要使用此功能，必须在初始化基数树时不传递__GFP_WAIT给INIT_RADIX_TREE()。
 */
int radix_tree_preload(gfp_t gfp_mask)
{
	struct radix_tree_preload *rtp;  // 预加载结构指针
	struct radix_tree_node *node;  // 节点指针
	int ret = -ENOMEM;  // 默认返回值设为内存不足错误

	preempt_disable();  // 禁止抢占，确保当前CPU不会被中断
	rtp = &__get_cpu_var(radix_tree_preloads);  // 获取当前CPU的预加载数据
	while (rtp->nr < ARRAY_SIZE(rtp->nodes)) {  // 当预加载的节点数未达到数组容量时
		preempt_enable();  // 允许抢占，因为接下来可能会进行阻塞的内存分配
		node = kmem_cache_alloc(radix_tree_node_cachep, gfp_mask);  // 从内存缓存分配一个新节点
		if (node == NULL)  // 如果分配失败
			goto out;  // 跳到函数末尾处理返回
		preempt_disable();  // 再次禁止抢占，因为接下来要修改预加载数据
		rtp = &__get_cpu_var(radix_tree_preloads);  // 重新获取当前CPU的预加载数据
		if (rtp->nr < ARRAY_SIZE(rtp->nodes))  // 如果仍有空间
			rtp->nodes[rtp->nr++] = node;  // 将新节点添加到数组
		else  // 如果在分配后数组已满
			kmem_cache_free(radix_tree_node_cachep, node);  // 释放多余的节点
	}
	ret = 0;  // 设置返回值为成功
out:
	return ret;  // 返回结果
}
EXPORT_SYMBOL(radix_tree_preload);

/*
 *	Return the maximum key which can be store into a
 *	radix tree with height HEIGHT.
 */
/*
 * 返回可以存储在具有给定高度HEIGHT的基数树中的最大键值。
 */
static inline unsigned long radix_tree_maxindex(unsigned int height)
{
	// 从预先计算的数组中直接获取给定高度的基数树的最大索引值
	return height_to_maxindex[height];
}

/*
 *	Extend a radix tree so it can store key @index.
 */
/*
 * 扩展一个基数树，使其能够存储键@index。
 */
static int radix_tree_extend(struct radix_tree_root *root, unsigned long index)
{
	struct radix_tree_node *node;  // 节点指针
	unsigned int height;  // 树的高度
	int tag;  // 标签索引

	/* Figure out what the height should be.  */
	// 计算应该的高度
	height = root->height + 1;
	while (index > radix_tree_maxindex(height))
		height++;	// 如果索引超过当前高度所能表示的最大索引，则增加高度

	if (root->rnode == NULL) {
		root->height = height;	// 如果根节点为空，直接设置树的高度
		goto out;
	}

	do {
		unsigned int newheight;
		if (!(node = radix_tree_node_alloc(root)))
			return -ENOMEM;	// 如果节点分配失败，返回内存不足错误

		/* Increase the height.  */
		// 增加高度
		node->slots[0] = radix_tree_indirect_to_ptr(root->rnode);	// 将原根节点设为新节点的第一个子节点

		/* Propagate the aggregated tag info into the new root */
		// 将聚合的标签信息传播到新的根节点
		for (tag = 0; tag < RADIX_TREE_MAX_TAGS; tag++) {
			if (root_tag_get(root, tag))
				tag_set(node, tag, 0);	// 如果原根节点有标签，则设置到新节点的相应标签
		}

		newheight = root->height+1;
		node->height = newheight;  // 设置新节点的高度
		node->count = 1;  // 设置新节点的子节点计数为1
		node = radix_tree_ptr_to_indirect(node);  // 将新节点转换为间接节点
		rcu_assign_pointer(root->rnode, node);  // 将新节点设置为树的根节点
		root->height = newheight;  // 更新树的高度
	} while (height > root->height);  // 如果计算的高度大于当前树的高度，继续循环
out:
	return 0;  // 返回成功
}

/**
 *	radix_tree_insert    -    insert into a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *	@item:		item to insert
 *
 *	Insert an item into the radix tree at position @index.
 */
/**
 * radix_tree_insert - 向基数树中插入元素
 * @root: 基数树根
 * @index: 索引键
 * @item: 要插入的项
 *
 * 在位置@index处向基数树插入一个项。
 */
int radix_tree_insert(struct radix_tree_root *root,
			unsigned long index, void *item)
{
	struct radix_tree_node *node = NULL, *slot;  // 定义用于插入操作的节点和槽位变量
	unsigned int height, shift;  // 定义树的高度和用于计算索引位置的位移量
	int offset;  // 定义数组中的位置偏移量
	int error;  // 用于存储错误代码

	BUG_ON(radix_tree_is_indirect_ptr(item));	// 确保插入的项不是间接指针

	/* Make sure the tree is high enough.  */
	// 确保树的高度足够以容纳此索引
	if (index > radix_tree_maxindex(root->height)) {	// 如果索引大于当前树高度所能容纳的最大索引
		error = radix_tree_extend(root, index);	// 尝试扩展树以容纳此索引
		if (error)	// 如果扩展过程中出现错误
			return error;	// 返回错误码
	}

	slot = radix_tree_indirect_to_ptr(root->rnode);	// 将根节点从可能的间接指针转换为直接指针

	height = root->height;	// 获取当前树的高度
	shift = (height-1) * RADIX_TREE_MAP_SHIFT;	// 计算位移量，用于确定索引在节点中的位置

	// 初始化偏移量，避免未初始化变量警告
	offset = 0;			/* uninitialised var warning */
	while (height > 0) {
		if (slot == NULL) {
			/* Have to add a child node.  */
			// 需要添加一个子节点
			if (!(slot = radix_tree_node_alloc(root)))
				return -ENOMEM;	// 分配节点失败，返回内存不足错误
			slot->height = height;	// 设置新节点的高度
			if (node) {
				rcu_assign_pointer(node->slots[offset], slot);	// 将新节点连接到当前节点
				node->count++;	// 增加当前节点的子节点计数
			} else
				rcu_assign_pointer(root->rnode,
					radix_tree_ptr_to_indirect(slot));	// 如果当前节点为空，设置根节点指向新节点
		}

		/* Go a level down */
		// 下降一层
		offset = (index >> shift) & RADIX_TREE_MAP_MASK;	// 计算当前索引在节点中的位置
		node = slot;	// 更新当前节点指针
		slot = node->slots[offset];	// 移动到对应的子节点槽位
		shift -= RADIX_TREE_MAP_SHIFT;	// 减小位移量，用于下一层的索引计算
		height--;	// 减少高度计数，向树的更深层移动
	}

	if (slot != NULL)
		return -EEXIST;	// 如果最终位置非空，返回已存在错误

	if (node) {
		node->count++;	// 增加节点的子节点计数
		rcu_assign_pointer(node->slots[offset], item);	// 在正确的位置插入新项
		BUG_ON(tag_get(node, 0, offset));	// 确保在该位置上标签0未被错误设置
		BUG_ON(tag_get(node, 1, offset));	// 确保在该位置上标签1未被错误设置
	} else {
		rcu_assign_pointer(root->rnode, item);	// 如果是空树，直接将根节点指向新项
		BUG_ON(root_tag_get(root, 0));		// 确保根节点标签0未设置
		BUG_ON(root_tag_get(root, 1));		// 确保根节点标签1未设置
	}

	return 0;	// 返回成功
}
EXPORT_SYMBOL(radix_tree_insert);

/*
 * is_slot == 1 : search for the slot.
 * is_slot == 0 : search for the node.
 */
/*
 * is_slot == 1：搜索槽位。
 * is_slot == 0：搜索节点。
 */
static void *radix_tree_lookup_element(struct radix_tree_root *root,
				unsigned long index, int is_slot)
{
	unsigned int height, shift;  // 定义树的高度和位移量
	struct radix_tree_node *node, **slot;  // 定义节点和槽位指针

	node = rcu_dereference_raw(root->rnode);  // 通过RCU安全获取根节点
	if (node == NULL)
		return NULL;  // 如果根节点为空，则返回NULL

	if (!radix_tree_is_indirect_ptr(node)) {  // 如果根节点不是间接指针
		if (index > 0)
			return NULL;  // 如果索引大于0，则返回NULL（根节点只能包含索引0的元素）
		return is_slot ? (void *)&root->rnode : node;  // 根据is_slot返回根节点的槽位地址或节点本身
	}
	node = radix_tree_indirect_to_ptr(node);  // 将间接指针转换为直接节点指针

	height = node->height;  // 获取节点高度
	if (index > radix_tree_maxindex(height))
		return NULL;  // 如果索引超出最大索引范围，则返回NULL

	shift = (height-1) * RADIX_TREE_MAP_SHIFT;  // 计算初始位移

	do {
		slot = (struct radix_tree_node **)
			(node->slots + ((index>>shift) & RADIX_TREE_MAP_MASK));  // 计算索引对应的槽位地址
		node = rcu_dereference_raw(*slot);  // 通过RCU安全获取槽位指向的节点
		if (node == NULL)
			return NULL;  // 如果节点为空，则返回NULL

		shift -= RADIX_TREE_MAP_SHIFT;  // 更新位移量，向下一层移动
		height--;  // 减少高度计数
	} while (height > 0);  // 持续直到达到叶子节点

	return is_slot ? (void *)slot : node;  // 根据is_slot返回找到的槽位地址或节点
}

/**
 *	radix_tree_lookup_slot    -    lookup a slot in a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *
 *	Returns:  the slot corresponding to the position @index in the
 *	radix tree @root. This is useful for update-if-exists operations.
 *
 *	This function can be called under rcu_read_lock iff the slot is not
 *	modified by radix_tree_replace_slot, otherwise it must be called
 *	exclusive from other writers. Any dereference of the slot must be done
 *	using radix_tree_deref_slot.
 */
/**
 * radix_tree_lookup_slot - 在基数树中查找一个槽
 * @root: 基数树根
 * @index: 索引键
 *
 * 返回：对应于基数树 @root 中位置 @index 的槽。这对于“如果存在则更新”操作很有用。
 *
 * 此函数可以在 rcu_read_lock 下调用，如果槽没有被 radix_tree_replace_slot 修改的话；
 * 否则，必须在其他写入者独占时调用。任何对槽的引用必须使用 radix_tree_deref_slot 完成。
 */
void **radix_tree_lookup_slot(struct radix_tree_root *root, unsigned long index)
{
	return (void **)radix_tree_lookup_element(root, index, 1);
}
EXPORT_SYMBOL(radix_tree_lookup_slot);

/**
 *	radix_tree_lookup    -    perform lookup operation on a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *
 *	Lookup the item at the position @index in the radix tree @root.
 *
 *	This function can be called under rcu_read_lock, however the caller
 *	must manage lifetimes of leaf nodes (eg. RCU may also be used to free
 *	them safely). No RCU barriers are required to access or modify the
 *	returned item, however.
 */
/**
 * radix_tree_lookup - 在基数树上执行查找操作
 * @root: 基数树根
 * @index: 索引键
 *
 * 在基数树 @root 的位置 @index 查找项。
 *
 * 此函数可以在 rcu_read_lock 下调用，但调用者必须管理叶节点的生命周期（例如，也可以使用 RCU 安全地释放它们）。
 * 然而，访问或修改返回的项不需要 RCU 屏障。
 */
void *radix_tree_lookup(struct radix_tree_root *root, unsigned long index)
{
	return radix_tree_lookup_element(root, index, 0);
}
EXPORT_SYMBOL(radix_tree_lookup);

/**
 *	radix_tree_tag_set - set a tag on a radix tree node
 *	@root:		radix tree root
 *	@index:		index key
 *	@tag: 		tag index
 *
 *	Set the search tag (which must be < RADIX_TREE_MAX_TAGS)
 *	corresponding to @index in the radix tree.  From
 *	the root all the way down to the leaf node.
 *
 *	Returns the address of the tagged item.   Setting a tag on a not-present
 *	item is a bug.
 */
/**
 * radix_tree_tag_set - 在基数树节点上设置标签
 * @root: 基数树根
 * @index: 索引键
 * @tag: 标签索引
 *
 * 在基数树中对应于@index的搜索标签设置（标签必须小于RADIX_TREE_MAX_TAGS）。
 * 从根节点一直到叶节点。
 *
 * 返回被标记项的地址。在不存在的项上设置标签是一个错误。
 */
void *radix_tree_tag_set(struct radix_tree_root *root,
			unsigned long index, unsigned int tag)
{
	unsigned int height, shift;  // 树的高度和位移量
	struct radix_tree_node *slot;  // 当前操作的节点

	height = root->height;  // 获取树的高度
	BUG_ON(index > radix_tree_maxindex(height));  // 确保索引在允许的范围内

	slot = radix_tree_indirect_to_ptr(root->rnode);  // 获取根节点的直接指针
	shift = (height - 1) * RADIX_TREE_MAP_SHIFT;  // 初始化位移量

	while (height > 0) {  // 遍历所有层级
		int offset;

		offset = (index >> shift) & RADIX_TREE_MAP_MASK;  // 计算当前层的槽位
		if (!tag_get(slot, tag, offset))
			tag_set(slot, tag, offset);  // 如果当前槽位未被标记，则设置标签
		slot = slot->slots[offset];  // 移动到下一层的对应槽位
		BUG_ON(slot == NULL);  // 确保不会遍历到空槽位
		shift -= RADIX_TREE_MAP_SHIFT;  // 更新位移量
		height--;  // 减少高度计数
	}

	/* set the root's tag bit */
	// 设置根节点的标签位
	if (slot && !root_tag_get(root, tag))
		root_tag_set(root, tag);  // 如果项存在且根标签未设置，则设置之

	return slot;  // 返回标记的项
}

EXPORT_SYMBOL(radix_tree_tag_set);

/**
 *	radix_tree_tag_clear - clear a tag on a radix tree node
 *	@root:		radix tree root
 *	@index:		index key
 *	@tag: 		tag index
 *
 *	Clear the search tag (which must be < RADIX_TREE_MAX_TAGS)
 *	corresponding to @index in the radix tree.  If
 *	this causes the leaf node to have no tags set then clear the tag in the
 *	next-to-leaf node, etc.
 *
 *	Returns the address of the tagged item on success, else NULL.  ie:
 *	has the same return value and semantics as radix_tree_lookup().
 */
/**
 * radix_tree_tag_clear - 在基数树节点上清除标签
 * @root: 基数树根
 * @index: 索引键
 * @tag: 标签索引
 *
 * 清除对应于基数树中@index的搜索标签（该标签必须小于RADIX_TREE_MAX_TAGS）。
 * 如果这导致叶节点没有设置任何标签，那么清除倒数第二个叶节点中的标签，依此类推。
 *
 * 成功时返回被标记项的地址，否则返回NULL。即：
 * 有与radix_tree_lookup()相同的返回值和语义。
 */
void *radix_tree_tag_clear(struct radix_tree_root *root,
			unsigned long index, unsigned int tag)
{
	/*
	 * The radix tree path needs to be one longer than the maximum path
	 * since the "list" is null terminated.
	 */
	/*
	 * 基数树路径需要比最大路径长度多一个，因为“列表”是以null终止的。
	 */
	struct radix_tree_path path[RADIX_TREE_MAX_PATH + 1], *pathp = path;  // 创建一个基数树路径数组，用于存储遍历路径
	struct radix_tree_node *slot = NULL;  // 指向当前操作的节点
	unsigned int height, shift;  // 树的高度和位移量

	height = root->height;  // 获取树的当前高度
	if (index > radix_tree_maxindex(height))
		goto out;	// 如果索引超出了当前高度所能覆盖的最大索引，则直接跳到函数结束

	shift = (height - 1) * RADIX_TREE_MAP_SHIFT;	// 计算位移量，用于后续的索引计算
	pathp->node = NULL;	// 初始化路径的当前节点为NULL
	slot = radix_tree_indirect_to_ptr(root->rnode);	// 将根节点的间接指针转换为直接节点指针

	while (height > 0) {
		int offset;	// 定义索引在当前节点槽位中的偏移

		if (slot == NULL)
			goto out;	// 如果节点为空，则跳转到函数出口

		offset = (index >> shift) & RADIX_TREE_MAP_MASK;  // 计算索引在当前节点槽位中的位置
		pathp[1].offset = offset;  // 存储当前槽位的偏移量
		pathp[1].node = slot;  // 存储当前节点
		slot = slot->slots[offset];  // 移动到对应的子节点
		pathp++;  // 移动路径指针到下一层
		shift -= RADIX_TREE_MAP_SHIFT;  // 更新位移量，准备下一层的索引计算
		height--;  // 递减树的高度计数
	}

	if (slot == NULL)
		goto out;	// 如果最终的槽位为空，表示没有找到元素，跳转到函数出口

	while (pathp->node) {	// 逆向遍历路径，从叶节点向上清除标签
		if (!tag_get(pathp->node, tag, pathp->offset))
			goto out;	// 如果当前节点在此偏移位置的标签未设置，跳转到函数出口
		tag_clear(pathp->node, tag, pathp->offset);	// 清除标签
		if (any_tag_set(pathp->node, tag))
			goto out;	// 如果在当前节点清除标签后还有标签设置，停止进一步清除
		pathp--;	// 向上移动到父节点
	}

	/* clear the root's tag bit */
	// 清除根节点的标签位
	if (root_tag_get(root, tag))
		root_tag_clear(root, tag);	// 如果根节点的此标签设置，清除它

out:
	return slot;	// 返回找到的槽位（叶节点），或NULL如果未找到
}
EXPORT_SYMBOL(radix_tree_tag_clear);

/**
 * radix_tree_tag_get - get a tag on a radix tree node
 * @root:		radix tree root
 * @index:		index key
 * @tag: 		tag index (< RADIX_TREE_MAX_TAGS)
 *
 * Return values:
 *
 *  0: tag not present or not set
 *  1: tag set
 *
 * Note that the return value of this function may not be relied on, even if
 * the RCU lock is held, unless tag modification and node deletion are excluded
 * from concurrency.
 */
/**
 * radix_tree_tag_get - 获取基数树节点上的标签
 * @root:		基数树根节点
 * @index:		索引键
 * @tag:		标签索引（必须小于 RADIX_TREE_MAX_TAGS）
 *
 * 返回值：
 *
 *  0: 标签不存在或未设置
 *  1: 标签设置
 *
 * 注意，即使持有RCU锁，此函数的返回值也不能完全依赖，除非从并发中排除了标签修改和节点删除。
 */
int radix_tree_tag_get(struct radix_tree_root *root,
			unsigned long index, unsigned int tag)
{
	unsigned int height, shift;
	struct radix_tree_node *node;
	int saw_unset_tag = 0;	// 标记是否遇到未设置的标签

	/* check the root's tag bit */
	// 检查根节点的标签位
	if (!root_tag_get(root, tag))
		return 0;	// 如果根节点的标签位未设置，直接返回0

	node = rcu_dereference_raw(root->rnode);
	if (node == NULL)
		return 0;	// 如果根节点为空，返回0

	if (!radix_tree_is_indirect_ptr(node))
		return (index == 0);	// 如果根节点不是间接指针，且索引为0，则返回1（表示标签设置）
	node = radix_tree_indirect_to_ptr(node);	// 将间接指针转换为直接节点指针

	height = node->height;
	if (index > radix_tree_maxindex(height))
		return 0;	// 如果索引超出最大索引范围，返回0

	shift = (height - 1) * RADIX_TREE_MAP_SHIFT;

	for ( ; ; ) {
		int offset;

		if (node == NULL)
			return 0;	// 如果节点为空，返回0

		offset = (index >> shift) & RADIX_TREE_MAP_MASK;	// 计算索引对应的槽位

		/*
		 * This is just a debug check.  Later, we can bale as soon as
		 * we see an unset tag.
		 */
		// 这只是一个调试检查。稍后，我们可以在看到未设置的标签后立即退出。
		if (!tag_get(node, tag, offset))
			saw_unset_tag = 1;	// 如果标签未设置，标记 saw_unset_tag
		if (height == 1)
			return !!tag_get(node, tag, offset);	// 如果是叶节点，返回标签是否设置的结果
		node = rcu_dereference_raw(node->slots[offset]);	// 移动到对应的子节点
		shift -= RADIX_TREE_MAP_SHIFT;	// 更新位移量
		height--;	// 降低高度
	}
}
EXPORT_SYMBOL(radix_tree_tag_get);

/**
 *	radix_tree_next_hole    -    find the next hole (not-present entry)
 *	@root:		tree root
 *	@index:		index key
 *	@max_scan:	maximum range to search
 *
 *	Search the set [index, min(index+max_scan-1, MAX_INDEX)] for the lowest
 *	indexed hole.
 *
 *	Returns: the index of the hole if found, otherwise returns an index
 *	outside of the set specified (in which case 'return - index >= max_scan'
 *	will be true). In rare cases of index wrap-around, 0 will be returned.
 *
 *	radix_tree_next_hole may be called under rcu_read_lock. However, like
 *	radix_tree_gang_lookup, this will not atomically search a snapshot of
 *	the tree at a single point in time. For example, if a hole is created
 *	at index 5, then subsequently a hole is created at index 10,
 *	radix_tree_next_hole covering both indexes may return 10 if called
 *	under rcu_read_lock.
 */
/**
 * radix_tree_next_hole - 查找下一个空洞（不存在的条目）
 * @root: 树的根节点
 * @index: 索引键
 * @max_scan: 搜索的最大范围
 *
 * 在集合[index, min(index+max_scan-1, MAX_INDEX)]中搜索最低索引的空洞。
 *
 * 返回：如果找到空洞，则返回空洞的索引；否则返回指定集合之外的索引
 *（此时 'return - index >= max_scan' 将为真）。在极少数索引回绕的情况下，将返回0。
 *
 * radix_tree_next_hole可以在rcu_read_lock下调用。然而，像radix_tree_gang_lookup一样，
 * 这不会在单个时间点原子地搜索树的快照。例如，如果在索引5创建了一个空洞，
 * 随后在索引10创建了一个空洞，如果在rcu_read_lock下调用，
 * radix_tree_next_hole覆盖这两个索引可能返回10。
 */
unsigned long radix_tree_next_hole(struct radix_tree_root *root,
				unsigned long index, unsigned long max_scan)
{
	unsigned long i;

	for (i = 0; i < max_scan; i++) {
		if (!radix_tree_lookup(root, index))
			break;  // 如果在当前索引未找到项，即找到一个空洞，退出循环
		index++;  // 否则增加索引继续搜索
		if (index == 0)
			break;  // 如果索引回绕到0，退出循环
	}

	return index;  // 返回找到的空洞的索引，或超出搜索范围的下一个索引
}
EXPORT_SYMBOL(radix_tree_next_hole);

/**
 *	radix_tree_prev_hole    -    find the prev hole (not-present entry)
 *	@root:		tree root
 *	@index:		index key
 *	@max_scan:	maximum range to search
 *
 *	Search backwards in the range [max(index-max_scan+1, 0), index]
 *	for the first hole.
 *
 *	Returns: the index of the hole if found, otherwise returns an index
 *	outside of the set specified (in which case 'index - return >= max_scan'
 *	will be true). In rare cases of wrap-around, LONG_MAX will be returned.
 *
 *	radix_tree_next_hole may be called under rcu_read_lock. However, like
 *	radix_tree_gang_lookup, this will not atomically search a snapshot of
 *	the tree at a single point in time. For example, if a hole is created
 *	at index 10, then subsequently a hole is created at index 5,
 *	radix_tree_prev_hole covering both indexes may return 5 if called under
 *	rcu_read_lock.
 */
/**
 * radix_tree_prev_hole - 查找前一个空洞（不存在的条目）
 * @root: 树的根
 * @index: 索引键
 * @max_scan: 最大搜索范围
 *
 * 在范围 [max(index-max_scan+1, 0), index] 向后搜索第一个空洞。
 *
 * 返回：如果找到空洞，则返回空洞的索引；否则返回指定集合之外的索引
 *（这种情况下 'index - return >= max_scan' 将为真）。在罕见的索引回绕情况下，将返回 LONG_MAX。
 *
 * radix_tree_next_hole 可以在 rcu_read_lock 下调用。然而，像 radix_tree_gang_lookup 一样，
 * 这不会在单一时间点原子性地搜索树的快照。例如，如果在索引10处创建了一个空洞，
 * 随后在索引5处创建了一个空洞，如果在 rcu_read_lock 下调用，
 * radix_tree_prev_hole 覆盖这两个索引可能返回5。
 */
unsigned long radix_tree_prev_hole(struct radix_tree_root *root,
				   unsigned long index, unsigned long max_scan)
{
	unsigned long i;

	for (i = 0; i < max_scan; i++) {
		if (!radix_tree_lookup(root, index))
			break;  // 如果在当前索引处未找到项（即发现空洞），则中断循环
		index--;  // 向后递减索引以继续搜索
		if (index == LONG_MAX)
			break;  // 检查索引是否回绕，防止无限循环
	}

	return index;  // 返回找到的空洞的索引，或超出搜索范围的下一个索引
}
EXPORT_SYMBOL(radix_tree_prev_hole);

static unsigned int
__lookup(struct radix_tree_node *slot, void ***results, unsigned long index,
	unsigned int max_items, unsigned long *next_index)
{
	unsigned int nr_found = 0;	// 已找到的元素数量
	unsigned int shift, height;	// 位移量和高度
	unsigned long i;

	height = slot->height;  // 获取当前节点的高度
	if (height == 0)  // 如果是叶节点
		goto out;
	shift = (height-1) * RADIX_TREE_MAP_SHIFT;  // 计算位移量，用于定位索引位置

	for ( ; height > 1; height--) {  // 从当前高度遍历到根
		i = (index >> shift) & RADIX_TREE_MAP_MASK;  // 计算当前索引的槽位位置
		for (;;) {  // 无限循环，直到找到非空槽或遍历完所有槽
			if (slot->slots[i] != NULL)
				break;  // 如果当前槽不为空，跳出循环
			index &= ~((1UL << shift) - 1);  // 清除低位，准备检查下一个槽位
			index += 1UL << shift;  // 更新索引到下一个位置
			if (index == 0)
				/* 32-bit wraparound */
				goto out;	// 如果索引回绕到0，则退出
			i++;
			if (i == RADIX_TREE_MAP_SIZE)
				goto out;  // 如果超出槽位大小，退出
		}

		shift -= RADIX_TREE_MAP_SHIFT;  // 更新位移量，准备下一层的索引计算
		slot = rcu_dereference_raw(slot->slots[i]);  // 获取下一层的节点
		if (slot == NULL)  // 如果节点为空，退出
			goto out;
	}

	/* Bottom level: grab some items */
	// 在最底层，获取一些元素
	for (i = index & RADIX_TREE_MAP_MASK; i < RADIX_TREE_MAP_SIZE; i++) {
		index++;  // 递增索引
		if (slot->slots[i]) {  // 如果槽位非空
			results[nr_found++] = &(slot->slots[i]);  // 存储结果
			if (nr_found == max_items)  // 如果达到最大元素数，退出
				goto out;
		}
	}
out:
	*next_index = index;  // 设置下一个索引
	return nr_found;  // 返回找到的元素数量
}

/**
 *	radix_tree_gang_lookup - perform multiple lookup on a radix tree
 *	@root:		radix tree root
 *	@results:	where the results of the lookup are placed
 *	@first_index:	start the lookup from this key
 *	@max_items:	place up to this many items at *results
 *
 *	Performs an index-ascending scan of the tree for present items.  Places
 *	them at *@results and returns the number of items which were placed at
 *	*@results.
 *
 *	The implementation is naive.
 *
 *	Like radix_tree_lookup, radix_tree_gang_lookup may be called under
 *	rcu_read_lock. In this case, rather than the returned results being
 *	an atomic snapshot of the tree at a single point in time, the semantics
 *	of an RCU protected gang lookup are as though multiple radix_tree_lookups
 *	have been issued in individual locks, and results stored in 'results'.
 */
/**
 * radix_tree_gang_lookup - 在基数树上执行多重查找
 * @root: 基数树根节点
 * @results: 查找结果存放位置
 * @first_index: 从此键开始查找
 * @max_items: 在 *results 中存放的最大项数
 *
 * 对树进行索引升序扫描以查找存在的项。将它们放在 *@results 中，并返回放置在 *@results 中的项数。
 *
 * 这个实现是初级的。
 *
 * 像 radix_tree_lookup 一样，radix_tree_gang_lookup 可以在 rcu_read_lock 下调用。在这种情况下，
 * 返回的结果不是树在单一时间点的原子快照，而是像在单独的锁中发出多个 radix_tree_lookups 一样，
 * 并将结果存储在 'results' 中。
 */
unsigned int
radix_tree_gang_lookup(struct radix_tree_root *root, void **results,
			unsigned long first_index, unsigned int max_items)
{
	unsigned long max_index;  // 树的最大索引
	struct radix_tree_node *node;  // 当前操作的节点
	unsigned long cur_index = first_index;  // 当前索引，从first_index开始
	unsigned int ret;  // 返回的查找结果数量

	node = rcu_dereference_raw(root->rnode);  // 获取根节点
	if (!node)
		return 0;  // 如果根节点为空，则返回0

	if (!radix_tree_is_indirect_ptr(node)) {
		if (first_index > 0)
			return 0;  // 如果根节点不是间接指针且first_index大于0，则返回0
		results[0] = node;  // 将根节点放入结果数组
		return 1;  // 返回找到的1个元素
	}
	node = radix_tree_indirect_to_ptr(node);  // 将间接指针转换为直接节点指针

	max_index = radix_tree_maxindex(node->height);  // 获取最大索引

	ret = 0;  // 初始化找到的结果数量为0
	while (ret < max_items) {  // 当找到的项数小于最大项数时，继续查找
		unsigned int nr_found, slots_found, i;
		/* Index of next search */
		unsigned long next_index;  // 下一次搜索的索引

		if (cur_index > max_index)
			break;  // 如果当前索引超过最大索引，则停止查找
		slots_found = __lookup(node, (void ***)results + ret, cur_index,
					max_items - ret, &next_index);  // 执行查找操作
		nr_found = 0;
		for (i = 0; i < slots_found; i++) {
			struct radix_tree_node *slot;
			slot = *(((void ***)results)[ret + i]);
			if (!slot)
				continue;  // 如果槽位为空，继续下一个槽位
			results[ret + nr_found] = rcu_dereference_raw(slot);  // 将槽位中的元素放入结果数组
			nr_found++;  // 增加找到的结果数量
		}
		ret += nr_found;  // 更新总的结果数量
		if (next_index == 0)
			break;  // 如果下一索引为0，表示索引回绕，停止查找
		cur_index = next_index;  // 更新当前索引
	}

	return ret;  // 返回总的找到的结果数量
}
EXPORT_SYMBOL(radix_tree_gang_lookup);

/**
 *	radix_tree_gang_lookup_slot - perform multiple slot lookup on radix tree
 *	@root:		radix tree root
 *	@results:	where the results of the lookup are placed
 *	@first_index:	start the lookup from this key
 *	@max_items:	place up to this many items at *results
 *
 *	Performs an index-ascending scan of the tree for present items.  Places
 *	their slots at *@results and returns the number of items which were
 *	placed at *@results.
 *
 *	The implementation is naive.
 *
 *	Like radix_tree_gang_lookup as far as RCU and locking goes. Slots must
 *	be dereferenced with radix_tree_deref_slot, and if using only RCU
 *	protection, radix_tree_deref_slot may fail requiring a retry.
 */
/**
 * radix_tree_gang_lookup_slot - 在基数树上执行多个槽位查找
 * @root: 基数树根
 * @results: 存放查找结果的位置
 * @first_index: 从此键开始查找
 * @max_items: 在 *results 中放置的最大项数
 *
 * 对树进行索引升序扫描以查找存在的项。将它们的槽位放在 *@results 中，并返回放置在 *@results 中的项数。
 *
 * 这个实现是初级的。
 *
 * 就 RCU 和锁定而言，与 radix_tree_gang_lookup 相同。槽位必须通过 radix_tree_deref_slot 解引用，
 * 如果仅使用 RCU 保护，radix_tree_deref_slot 可能失败，需要重试。
 */
unsigned int
radix_tree_gang_lookup_slot(struct radix_tree_root *root, void ***results,
			unsigned long first_index, unsigned int max_items)
{
	unsigned long max_index;  // 树的最大索引
	struct radix_tree_node *node;  // 当前操作的节点
	unsigned long cur_index = first_index;  // 当前索引，从first_index开始
	unsigned int ret;  // 返回的查找结果数量

	node = rcu_dereference_raw(root->rnode);  // 获取根节点
	if (!node)
		return 0;  // 如果根节点为空，则返回0

	if (!radix_tree_is_indirect_ptr(node)) {
		if (first_index > 0)
			return 0;  // 如果根节点不是间接指针且first_index大于0，则返回0
		results[0] = (void **)&root->rnode;  // 将根节点的槽位地址放入结果数组
		return 1;  // 返回找到的1个元素
	}
	node = radix_tree_indirect_to_ptr(node);  // 将间接指针转换为直接节点指针

	max_index = radix_tree_maxindex(node->height);  // 获取最大索引

	ret = 0;  // 初始化找到的结果数量为0
	while (ret < max_items) {  // 当找到的项数小于最大项数时，继续查找
		unsigned int slots_found;
		/* Index of next search */
		unsigned long next_index;  // 下一次搜索的索引

		if (cur_index > max_index)
			break;  // 如果当前索引超过最大索引，则停止查找
		slots_found = __lookup(node, results + ret, cur_index,
					max_items - ret, &next_index);  // 执行查找操作
		ret += slots_found;  // 更新总的结果数量
		if (next_index == 0)
			break;  // 如果下一索引为0，表示索引回绕，停止查找
		cur_index = next_index;  // 更新当前索引
	}

	return ret;  // 返回总的找到的结果数量
}
EXPORT_SYMBOL(radix_tree_gang_lookup_slot);

/*
 * FIXME: the two tag_get()s here should use find_next_bit() instead of
 * open-coding the search.
 */
/*
 * FIXME：此处的两个 tag_get() 应使用 find_next_bit() 替代现有的明码搜索方式。
 */
static unsigned int
__lookup_tag(struct radix_tree_node *slot, void ***results, unsigned long index,
	unsigned int max_items, unsigned long *next_index, unsigned int tag)
{
	unsigned int nr_found = 0;  // 找到的条目数量
	unsigned int shift, height;  // 位移和高度

	height = slot->height;  // 节点的高度
	if (height == 0)
		goto out;  // 如果高度为0，跳到结束
	shift = (height-1) * RADIX_TREE_MAP_SHIFT;  // 计算位移量

	while (height > 0) {
		unsigned long i = (index >> shift) & RADIX_TREE_MAP_MASK ;	// 计算索引位置

		for (;;) {	// 循环直到找到设置了标签的槽或完成一层的搜索
			if (tag_get(slot, tag, i))
				break;	// 如果当前位置的标签被设置，跳出循环
			index &= ~((1UL << shift) - 1);	// 清除低位
			index += 1UL << shift;	// 调整索引到下一个槽
			if (index == 0)
				// 如果发生32位环绕，跳到结束
				goto out;	/* 32-bit wraparound */
			i++;
			if (i == RADIX_TREE_MAP_SIZE)
				goto out;	// 如果超出槽位范围，跳到结束
		}
		height--;	// 减小高度，准备进入下一层
		// 如果到达底层，开始收集条目
		if (height == 0) {	/* Bottom level: grab some items */
			unsigned long j = index & RADIX_TREE_MAP_MASK;	// 计算底层索引

			for ( ; j < RADIX_TREE_MAP_SIZE; j++) {	// 遍历槽位
				index++;
				if (!tag_get(slot, tag, j))
					continue;	// 如果标签未设置，继续下一个
				/*
				 * Even though the tag was found set, we need to
				 * recheck that we have a non-NULL node, because
				 * if this lookup is lockless, it may have been
				 * subsequently deleted.
				 *
				 * Similar care must be taken in any place that
				 * lookup ->slots[x] without a lock (ie. can't
				 * rely on its value remaining the same).
				 */
				/*
				 * 即使找到了设置的标签，我们仍需要重新检查我们拥有一个非空节点，因为
				 * 如果这次查找是无锁的，它可能已经被后续删除了。
				 *
				 * 在任何无锁查找 ->slots[x] 的地方都必须采取类似的谨慎措施（即不能
				 * 依赖其值保持不变）。
				 */
				if (slot->slots[j]) {	// 检查槽位是否非空
					results[nr_found++] = &(slot->slots[j]);	// 收集结果
					if (nr_found == max_items)
						goto out;	// 如果收集足够的条目，结束
				}
			}
		}
		shift -= RADIX_TREE_MAP_SHIFT;	// 更新位移量
		slot = rcu_dereference_raw(slot->slots[i]);	// 移动到下一节点
		if (slot == NULL)
			break;	// 如果节点为空，结束循环
	}
out:
	*next_index = index;	// 设置下一个索引
	return nr_found;	// 返回找到的条目数量
}

/**
 *	radix_tree_gang_lookup_tag - perform multiple lookup on a radix tree
 *	                             based on a tag
 *	@root:		radix tree root
 *	@results:	where the results of the lookup are placed
 *	@first_index:	start the lookup from this key
 *	@max_items:	place up to this many items at *results
 *	@tag:		the tag index (< RADIX_TREE_MAX_TAGS)
 *
 *	Performs an index-ascending scan of the tree for present items which
 *	have the tag indexed by @tag set.  Places the items at *@results and
 *	returns the number of items which were placed at *@results.
 */
/**
 *	radix_tree_gang_lookup_tag - 根据标签在基数树上执行多重查找
 *	@root:		基数树根节点
 *	@results:	存放查找结果的位置
 *	@first_index:	从此键开始查找
 *	@max_items:	最多在 *results 中存放这么多项
 *	@tag:		标签索引 (< RADIX_TREE_MAX_TAGS)
 *
 *	执行基数树的索引升序扫描，查找设置了由 @tag 索引的标签的存在项。
 *	将找到的项放置在 *@results 中，并返回放置在 *@results 中的项数。
 */
unsigned int
radix_tree_gang_lookup_tag(struct radix_tree_root *root, void **results,
		unsigned long first_index, unsigned int max_items,
		unsigned int tag)
{
	struct radix_tree_node *node;  // 当前操作的节点
	unsigned long max_index;  // 能查找到的最大索引
	unsigned long cur_index = first_index;  // 当前查找的起始索引
	unsigned int ret;  // 函数返回的找到的项数

	/* check the root's tag bit */
	/* 检查根节点的标签位 */
	if (!root_tag_get(root, tag))
		return 0;  // 如果根节点未设置指定标签，直接返回0

	node = rcu_dereference_raw(root->rnode);  // 获取根节点
	if (!node)
		return 0;  // 如果根节点为空，返回0

	if (!radix_tree_is_indirect_ptr(node)) {
		if (first_index > 0)
			return 0;  // 如果根节点不是间接指针且first_index大于0，则返回0
		results[0] = node;  // 直接将根节点放入结果
		return 1;  // 返回找到一个结果
	}
	node = radix_tree_indirect_to_ptr(node);  // 将间接指针转换为直接节点指针

	max_index = radix_tree_maxindex(node->height);  // 计算最大索引

	ret = 0;	// 初始化结果计数
	while (ret < max_items) {	// 当找到的结果数未达到上限时继续查找
		unsigned int nr_found, slots_found, i;
		// 下一个查找的起始索引
		unsigned long next_index;	/* Index of next search */

		if (cur_index > max_index)
			break;	// 如果当前索引超过最大索引，则终止循环
		slots_found = __lookup_tag(node, (void ***)results + ret,
				cur_index, max_items - ret, &next_index, tag);	// 查找设置了标签的节点
		nr_found = 0;
		for (i = 0; i < slots_found; i++) {
			struct radix_tree_node *slot;
			slot = *(((void ***)results)[ret + i]);
			if (!slot)
				continue;	// 如果槽位为空，继续下一个槽位
			results[ret + nr_found] = rcu_dereference_raw(slot);	// 存储非空槽位
			nr_found++;	// 增加找到的结果数
		}
		ret += nr_found;	// 更新已找到的总结果数
		if (next_index == 0)
			break;	// 如果下一个索引为0，表示已完成所有查找
		cur_index = next_index;	// 更新当前查找索引
	}

	return ret;	// 返回找到的总结果数
}
EXPORT_SYMBOL(radix_tree_gang_lookup_tag);

/**
 *	radix_tree_gang_lookup_tag_slot - perform multiple slot lookup on a
 *					  radix tree based on a tag
 *	@root:		radix tree root
 *	@results:	where the results of the lookup are placed
 *	@first_index:	start the lookup from this key
 *	@max_items:	place up to this many items at *results
 *	@tag:		the tag index (< RADIX_TREE_MAX_TAGS)
 *
 *	Performs an index-ascending scan of the tree for present items which
 *	have the tag indexed by @tag set.  Places the slots at *@results and
 *	returns the number of slots which were placed at *@results.
 */
/**
 * radix_tree_gang_lookup_tag_slot - 基于标签在基数树上执行多个槽位查找
 * @root:       基数树根节点
 * @results:    存放查找结果的位置
 * @first_index: 从这个键开始查找
 * @max_items:  在 *results 中最多放置这么多项
 * @tag:        标签索引 (< RADIX_TREE_MAX_TAGS)
 *
 * 对树执行索引升序扫描，查找设置了由 @tag 索引的标签的存在项。
 * 将槽位放在 *@results 中，并返回在 *@results 中放置的槽位数。
 */
unsigned int
radix_tree_gang_lookup_tag_slot(struct radix_tree_root *root, void ***results,
		unsigned long first_index, unsigned int max_items,
		unsigned int tag)
{
	struct radix_tree_node *node;  // 当前节点
	unsigned long max_index;  // 最大索引
	unsigned long cur_index = first_index;  // 当前索引
	unsigned int ret;  // 返回值，记录找到的槽数

	/* check the root's tag bit */
	/* 检查根节点的标签位 */
	if (!root_tag_get(root, tag))
		return 0;  // 如果根节点没有设置此标签，返回0

	node = rcu_dereference_raw(root->rnode);  // 获取根节点
	if (!node)
		return 0;  // 如果节点为空，返回0

	if (!radix_tree_is_indirect_ptr(node)) {
		if (first_index > 0)
			return 0;  // 如果是直接指针并且first_index大于0，返回0
		results[0] = (void **)&root->rnode;  // 直接返回根节点的地址
		return 1;
	}
	node = radix_tree_indirect_to_ptr(node);  // 获取节点的实际地址

	max_index = radix_tree_maxindex(node->height);  // 获取可以查找的最大索引

	ret = 0;  // 初始化结果数
	while (ret < max_items) {
		unsigned int slots_found;	// 在这一轮找到的槽位数
		// 下一次查找的索引
		unsigned long next_index;	/* Index of next search */

		if (cur_index > max_index)
			break;	// 如果当前索引大于最大索引，结束循环
		slots_found = __lookup_tag(node, results + ret,
				cur_index, max_items - ret, &next_index, tag);	// 查找设置了指定标签的槽位
		ret += slots_found;	// 更新找到的总槽数
		if (next_index == 0)
			break;	// 如果下一次的索引为0，结束循环
		cur_index = next_index;	// 更新当前索引
	}

	return ret;	// 返回找到的槽数
}
EXPORT_SYMBOL(radix_tree_gang_lookup_tag_slot);


/**
 *	radix_tree_shrink    -    shrink height of a radix tree to minimal
 *	@root		radix tree root
 */
/**
 * radix_tree_shrink - 缩减基数树的高度至最小
 * @root       基数树的根节点
 */
static inline void radix_tree_shrink(struct radix_tree_root *root)
{
	/* try to shrink tree height */
	// 尝试缩减树的高度
	while (root->height > 0) {
		struct radix_tree_node *to_free = root->rnode;
		void *newptr;

		BUG_ON(!radix_tree_is_indirect_ptr(to_free));	// 确保rnode是间接指针
		to_free = radix_tree_indirect_to_ptr(to_free);	// 将间接指针转换为直接节点指针

		/*
		 * The candidate node has more than one child, or its child
		 * is not at the leftmost slot, we cannot shrink.
		 */
		/**
		 * 如果候选节点有多于一个子节点，或其子节点不在最左边的槽位，
		 * 我们无法缩减。
		 */
		if (to_free->count != 1)
			break;
		if (!to_free->slots[0])
			break;

		/*
		 * We don't need rcu_assign_pointer(), since we are simply
		 * moving the node from one part of the tree to another. If
		 * it was safe to dereference the old pointer to it
		 * (to_free->slots[0]), it will be safe to dereference the new
		 * one (root->rnode).
		 */
		/**
		 * 我们不需要 rcu_assign_pointer()，因为我们只是简单地将节点
		 * 从树的一个部分移动到另一个部分。如果旧指针（to_free->slots[0]）
		 * 的解引用是安全的，那么新的指针（root->rnode）的解引用也是安全的。
		 */
		newptr = to_free->slots[0];
		if (root->height > 1)
			newptr = radix_tree_ptr_to_indirect(newptr);  // 如果树的高度大于1，将节点转换为间接指针
		root->rnode = newptr;  // 更新根节点的指针
		root->height--;  // 减少树的高度
		radix_tree_node_free(to_free);  // 释放被替换的节点
	}
}

/**
 *	radix_tree_delete    -    delete an item from a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *
 *	Remove the item at @index from the radix tree rooted at @root.
 *
 *	Returns the address of the deleted item, or NULL if it was not present.
 */
/**
 * radix_tree_delete - 从基数树中删除一个项
 * @root: 基数树根节点
 * @index: 索引键
 *
 * 从位于 @root 的基数树中删除 @index 处的项。
 *
 * 返回被删除项的地址，如果该项不存在则返回 NULL。
 */
void *radix_tree_delete(struct radix_tree_root *root, unsigned long index)
{
	/*
	 * The radix tree path needs to be one longer than the maximum path
	 * since the "list" is null terminated.
	 */
	/*
	 * 基数树路径需要比最大路径长度长一，因为“列表”以 null 结尾。
	 */
	struct radix_tree_path path[RADIX_TREE_MAX_PATH + 1], *pathp = path;  // 路径数组和路径指针初始化
	struct radix_tree_node *slot = NULL;  // 当前操作的节点初始化为NULL
	struct radix_tree_node *to_free;  // 将要释放的节点
	unsigned int height, shift;  // 树的高度和位移变量
	int tag;  // 用于迭代标签的变量
	int offset;  // 节点内部偏移量

	height = root->height;  // 获取树的当前高度
	if (index > radix_tree_maxindex(height))
		goto out;  // 如果给定索引超出当前树高度所能表示的最大索引，则跳转到函数结束

	slot = root->rnode;  // 获取根节点指针
	if (height == 0) {
		root_tag_clear_all(root);  // 如果树高度为0，清除所有根标签
		root->rnode = NULL;  // 并将根节点指针设置为NULL
		goto out;  // 跳转到函数结束
	}
	slot = radix_tree_indirect_to_ptr(slot);  // 将根节点指针转换为直接指针

	shift = (height - 1) * RADIX_TREE_MAP_SHIFT;  // 计算初始位移量
	pathp->node = NULL;  // 初始化路径数组的第一个节点指针为NULL

	do {
		if (slot == NULL)
			goto out;  // 如果当前节点为NULL，则跳出，表示没有找到对应项

		pathp++;  // 移动到路径数组的下一个元素
		offset = (index >> shift) & RADIX_TREE_MAP_MASK;  // 计算当前层的索引偏移
		pathp->offset = offset;  // 存储当前节点在其父节点中的偏移量
		pathp->node = slot;  // 存储当前访问的节点
		slot = slot->slots[offset];  // 移动到下一层的对应节点
		shift -= RADIX_TREE_MAP_SHIFT;  // 更新位移量，为下一层的索引计算做准备
		height--;  // 减小树的剩余高度
	} while (height > 0);  // 如果还没有到达叶子节点，继续循环

	if (slot == NULL)
		goto out;  // 如果找到的叶子节点为NULL，表示没有找到对应项，跳出

	/*
	 * Clear all tags associated with the just-deleted item
	 */
	/*
	 * 清除与刚刚删除的项关联的所有标签
	 */
	for (tag = 0; tag < RADIX_TREE_MAX_TAGS; tag++) {  // 遍历所有可能的标签
		if (tag_get(pathp->node, tag, pathp->offset))  // 如果在当前节点的当前位置设置了标签
			radix_tree_tag_clear(root, index, tag);  // 清除该标签
	}

	to_free = NULL;	// 初始化待释放节点指针
	/* Now free the nodes we do not need anymore */
	/* 现在释放我们不再需要的节点 */
	while (pathp->node) {	// 遍历从叶子到根的路径
		pathp->node->slots[pathp->offset] = NULL;	// 清除指向被删除节点的指针
		pathp->node->count--;	// 减少父节点的子节点计数
		/*
		 * Queue the node for deferred freeing after the
		 * last reference to it disappears (set NULL, above).
		 */
		/*
		 * 在最后一个引用消失后（上面设置为 NULL），将节点排队等待延迟释放。
		 */
		if (to_free)
			radix_tree_node_free(to_free);	// 释放前一个标记为释放的节点

		if (pathp->node->count) {  // 如果当前节点仍有子节点
			if (pathp->node == radix_tree_indirect_to_ptr(root->rnode))
				radix_tree_shrink(root);  // 如果是根节点，尝试缩减树高度
			goto out;  // 跳出，因为不需要进一步释放
		}

		/* Node with zero slots in use so free it */
		/* 使用槽位为零的节点因此释放它 */
		to_free = pathp->node;	// 标记当前节点待释放
		pathp--;	// 移动到下一个路径点，即向根节点方向移动

	}
	root_tag_clear_all(root);	// 清除根节点的所有标签
	root->height = 0;		// 将树高设置为0
	root->rnode = NULL;		// 将根节点指针置为空
	if (to_free)
		radix_tree_node_free(to_free);	// 释放最后一个标记为释放的节点

out:
	return slot;	// 返回被删除的节点的地址，如果未找到则为 NULL
}
EXPORT_SYMBOL(radix_tree_delete);

/**
 *	radix_tree_tagged - test whether any items in the tree are tagged
 *	@root:		radix tree root
 *	@tag:		tag to test
 */
/**
 * radix_tree_tagged - 测试树中是否有任何项被标记
 * @root: 基数树根节点
 * @tag: 要测试的标签
 */
int radix_tree_tagged(struct radix_tree_root *root, unsigned int tag)
{
	return root_tag_get(root, tag);  // 返回是否有项与给定标签相关联
}
EXPORT_SYMBOL(radix_tree_tagged);

static void
radix_tree_node_ctor(void *node)
{
	memset(node, 0, sizeof(struct radix_tree_node));	// 初始化节点，将其内存区域设置为0
}

static __init unsigned long __maxindex(unsigned int height)
{
	unsigned int width = height * RADIX_TREE_MAP_SHIFT;  // 计算树的宽度
	int shift = RADIX_TREE_INDEX_BITS - width;  // 计算位移量

	if (shift < 0)
		return ~0UL;  // 如果位移量小于0，返回最大无符号长整型
	if (shift >= BITS_PER_LONG)
		return 0UL;  // 如果位移量大于或等于一个长整型的位数，返回0
	return ~0UL >> shift;  // 返回位移后的值
}

// 用于存储不同高度的基数树可以有的最大索引值。它遍历数组，并为每个可能的树高调用 __maxindex 函数来计算并设置对应的最大索引值。
static __init void radix_tree_init_maxindex(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(height_to_maxindex); i++)
		height_to_maxindex[i] = __maxindex(i);	// 初始化高度对应的最大索引数组
}

static int radix_tree_callback(struct notifier_block *nfb,
                            unsigned long action,
                            void *hcpu)
{
       int cpu = (long)hcpu;	// 获取发生事件的 CPU 编号
       struct radix_tree_preload *rtp;

       /* Free per-cpu pool of perloaded nodes */
       /* 释放每个 CPU 上预加载的节点池 */
;
       if (action == CPU_DEAD || action == CPU_DEAD_FROZEN) {  // 如果 CPU 死亡或冻结
		rtp = &per_cpu(radix_tree_preloads, cpu);  // 获取该 CPU 对应的预加载节点结构
		while (rtp->nr) {  // 遍历并释放所有预加载的节点
			kmem_cache_free(radix_tree_node_cachep,
			                rtp->nodes[rtp->nr-1]);  // 释放节点内存
			rtp->nodes[rtp->nr-1] = NULL;  // 清空指针
			rtp->nr--;  // 节点计数减少
		}
	}
	return NOTIFY_OK;  // 返回通知处理结果
}

void __init radix_tree_init(void)
{
	// 在系统初始化时，创建用于基数树节点的内存缓存池
	radix_tree_node_cachep = kmem_cache_create("radix_tree_node",
			sizeof(struct radix_tree_node), 0,
			SLAB_PANIC | SLAB_RECLAIM_ACCOUNT,
			radix_tree_node_ctor);
	radix_tree_init_maxindex();	// 初始化基数树能够索引的最大值数组
	// 注册处理器热插拔的回调函数
	hotcpu_notifier(radix_tree_callback, 0);
}
