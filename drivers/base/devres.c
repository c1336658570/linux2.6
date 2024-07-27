/*
 * drivers/base/devres.c - device resource management
 *
 * Copyright (c) 2006  SUSE Linux Products GmbH
 * Copyright (c) 2006  Tejun Heo <teheo@suse.de>
 *
 * This file is released under the GPLv2.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "base.h"

// 定义一个资源节点结构体，用于管理设备资源的释放。
struct devres_node {
	struct list_head		entry;		// 列表节点，用于将资源节点链接到设备的资源列表
	dr_release_t			release;	// 资源释放函数指针，当设备卸载或资源被释放时调用
#ifdef CONFIG_DEBUG_DEVRES
	const char			*name;		// 资源名称，用于调试目的
	size_t				size;		// 资源大小，用于调试目的
#endif
};

// 定义一个设备资源结构体，它包含了一个资源节点和数据数组
struct devres {
	struct devres_node		node;	// 包含基本的资源管理信息
	/* -- 3 pointers */
	// 数据数组，用于存储资源具体数据，保证了unsigned long long类型的对齐
	unsigned long long		data[];	/* guarantee ull alignment */
};

// 定义一个资源组结构体，用于管理一组资源
struct devres_group {
	struct devres_node		node[2];	// 资源节点数组，使用两个节点，可能用于标记组的开始和结束
	void				*id;		// 资源组标识符，用于识别或索引资源组
	int				color;		// 资源组的颜色或标记，用于区分不同的资源组
	/* -- 8 pointers */
};

// 如果定义了CONFIG_DEBUG_DEVRES，即启用了设备资源管理的调试功能
#ifdef CONFIG_DEBUG_DEVRES
// 定义一个静态整数变量log_devres，用于控制是否记录设备资源的日志，初始值为0（不记录）
static int log_devres = 0;
// 将log_devres变量暴露为模块参数“log”，允许在加载模块时设置它，并可通过/sys/module/.../parameters/读取和修改
// S_IRUGO | S_IWUSR：设置权限为可读和可写
module_param_named(log, log_devres, int, S_IRUGO | S_IWUSR);

static void set_node_dbginfo(struct devres_node *node, const char *name,
			     size_t size)
{
	node->name = name;	// 设置资源节点的名字
	node->size = size;	// 设置资源节点的大小
}

static void devres_log(struct device *dev, struct devres_node *node,
		       const char *op)
{
	if (unlikely(log_devres))	// 如果log_devres非零（不太可能，因为默认为0，只有显式设置后才会改变）
		// 使用dev_printk打印设备资源操作的日志，日志级别为KERN_ERR（错误信息）
		// 输出的信息包括操作类型（op），资源节点指针，资源名称，资源大小
		dev_printk(KERN_ERR, dev, "DEVRES %3s %p %s (%lu bytes)\n",
			   op, node, node->name, (unsigned long)node->size);
}
// 如果没有定义CONFIG_DEBUG_DEVRES，即没有启用设备资源管理的调试功能
#else /* CONFIG_DEBUG_DEVRES */
// 定义一个空操作宏，不记录任何调试信息
#define set_node_dbginfo(node, n, s)	do {} while (0)
// 定义一个空操作宏，不执行日志记录
#define devres_log(dev, node, op)	do {} while (0)
#endif /* CONFIG_DEBUG_DEVRES */

/*
 * Release functions for devres group.  These callbacks are used only
 * for identification.
 */
// 设备资源组的释放函数。这些回调函数仅用于识别。
static void group_open_release(struct device *dev, void *res)
{
	/* noop */
	// 这是一个空操作函数，用作资源组开始的标识。它不执行任何操作。
}

static void group_close_release(struct device *dev, void *res)
{
	/* noop */
	// 这是一个空操作函数，用作资源组结束的标识。它不执行任何操作。
}

// node_to_group - 将 devres_node 转换为包含它的 devres_group
// @node: 指向 devres_node 的指针
//
// 该函数检查提供的 devres_node 的释放回调，以确定它是作为开启节点还是关闭节点属于某个 devres_group。
// 它根据这种检查返回包含的 devres_group。
// 如果节点不对应任何一种情况，它返回 NULL。
static struct devres_group * node_to_group(struct devres_node *node)
{
	// 如果节点的释放函数是 group_open_release，表明这个节点是资源组的开启节点。
	// 使用 container_of 宏从节点获取到整个资源组的开始地址。
	if (node->release == &group_open_release)
		return container_of(node, struct devres_group, node[0]);
	// 如果节点的释放函数是 group_close_release，表明这个节点是资源组的关闭节点。
	// 使用 container_of 宏从节点获取到整个资源组的结束地址。
	if (node->release == &group_close_release)
		return container_of(node, struct devres_group, node[1]);
	// 如果节点的释放函数既不是 group_open_release 也不是 group_close_release，
	// 返回 NULL 表示这个节点不属于资源组的开启或关闭节点。
	return NULL;
}

// alloc_dr - 分配一个设备资源块
// @release: 此资源的释放函数
// @size: 由此资源管理的数据的大小
// @gfp: 内存分配的 GFP (获取空闲页) 标志
//
// 该函数分配一个内存块，包括 devres 结构和额外空间用于用户定义的数据。
// 它初始化列表并设置释放函数。
//
// 成功时返回指向分配的 devres 块的指针，失败时返回 NULL。
static __always_inline struct devres * alloc_dr(dr_release_t release,
						size_t size, gfp_t gfp)
{
	size_t tot_size = sizeof(struct devres) + size;	// 计算总大小，包括 devres 结构和用户数据
	struct devres *dr;

	dr = kmalloc_track_caller(tot_size, gfp);	// 使用 kmalloc_track_caller 分配内存，它跟踪调用者
	if (unlikely(!dr))
		return NULL;	// 如果内存分配失败，返回 NULL

	memset(dr, 0, tot_size);	// 分配的内存块清零
	INIT_LIST_HEAD(&dr->node.entry);	// 初始化 devres 结构中的 list_head
	dr->node.release = release;	// 设置释放函数
	return dr;	// 返回分配的 devres 结构的指针
}

// add_dr - 向设备的资源列表中添加一个设备资源节点
// @dev: 正在向其添加资源的设备
// @node: 要添加的设备资源节点
//
// 该函数记录资源的添加，检查编程错误，并将资源追加到设备的资源列表中。
static void add_dr(struct device *dev, struct devres_node *node)
{
	devres_log(dev, node, "ADD");	// 记录添加操作，如果启用了调试资源记录
	BUG_ON(!list_empty(&node->entry));	// 断言该节点的链表项已初始化为空，否则触发内核bug
	list_add_tail(&node->entry, &dev->devres_head);	// 将节点添加到设备的资源列表尾部
}

#ifdef CONFIG_DEBUG_DEVRES
// __devres_alloc - 分配并初始化带有调试信息的设备资源
// @release: 与此资源关联的释放函数
// @size: 要分配的内存大小
// @gfp: 指定内存分配类型的GFP标志
// @name: 与分配关联的调试名称
//
// 该函数为设备资源分配内存，初始化它，并记录关于分配的调试信息。
// 返回值：成功时指向分配的资源数据的指针，失败时为NULL。
void * __devres_alloc(dr_release_t release, size_t size, gfp_t gfp,
		      const char *name)
{
	struct devres *dr;

	dr = alloc_dr(release, size, gfp); // 调用 alloc_dr 分配并初始化设备资源
	if (unlikely(!dr))
		return NULL; // 如果分配失败，返回NULL
	set_node_dbginfo(&dr->node, name, size); // 设置节点的调试信息，便于跟踪
	return dr->data; // 返回指向分配资源数据的指针
}
EXPORT_SYMBOL_GPL(__devres_alloc);
// 如果未启用调试设备资源管理
#else
/**
 * devres_alloc - Allocate device resource data
 * @release: Release function devres will be associated with
 * @size: Allocation size
 * @gfp: Allocation flags
 *
 * Allocate devres of @size bytes.  The allocated area is zeroed, then
 * associated with @release.  The returned pointer can be passed to
 * other devres_*() functions.
 *
 * RETURNS:
 * Pointer to allocated devres on success, NULL on failure.
 */
// devres_alloc - 分配设备资源数据
// @release: devres将与之关联的释放函数
// @size: 分配大小
// @gfp: 分配标志
//
// 分配 @size 字节的 devres。分配的区域被清零，然后与 @release 关联。
// 返回的指针可以传递给其他 devres_*() 函数。
void * devres_alloc(dr_release_t release, size_t size, gfp_t gfp)
{
	struct devres *dr;
	
	dr = alloc_dr(release, size, gfp); // 调用 alloc_dr 分配并初始化设备资源
	if (unlikely(!dr))
		return NULL; // 如果分配失败，返回NULL
	return dr->data; // 返回指向分配资源数据的指针
}
EXPORT_SYMBOL_GPL(devres_alloc);
#endif

/**
 * devres_free - Free device resource data
 * @res: Pointer to devres data to free
 *
 * Free devres created with devres_alloc().
 */
/**
 * devres_free - 释放设备资源数据
 * @res: 指向要释放的 devres 数据的指针
 *
 * 释放通过 devres_alloc 创建的 devres。
 */
void devres_free(void *res)
{
	if (res) {	// 如果传入的资源指针不为空
		// 通过 res 指针获取 devres 结构体的地址
		struct devres *dr = container_of(res, struct devres, data);

		// 检查 devres 结构体的 entry 成员是否为空（应该为空，因为资源应已从所有列表中移除）
		BUG_ON(!list_empty(&dr->node.entry));	// 如果不为空，则触发内核 BUG，因为这表示资源管理出现问题
		kfree(dr);	// 使用 kfree 释放 devres 结构体占用的内存
	}
}
EXPORT_SYMBOL_GPL(devres_free);

/**
 * devres_add - Register device resource
 * @dev: Device to add resource to
 * @res: Resource to register
 *
 * Register devres @res to @dev.  @res should have been allocated
 * using devres_alloc().  On driver detach, the associated release
 * function will be invoked and devres will be freed automatically.
 */
/**
 * devres_add - 注册设备资源
 * @dev: 要添加资源的设备
 * @res: 要注册的资源
 *
 * 将 devres @res 注册到 @dev。@res 应该通过 devres_alloc() 分配。
 * 在驱动分离时，关联的释放函数将被调用，并且 devres 将自动被释放。
 */
void devres_add(struct device *dev, void *res)
{
	// 通过 res 指针获取 devres 结构体的地址
	struct devres *dr = container_of(res, struct devres, data);
	unsigned long flags; // 用于保存中断状态

	// 锁定设备资源锁，保存当前中断状态，并禁用中断
	spin_lock_irqsave(&dev->devres_lock, flags);

	// 将资源添加到设备的资源链表中
	add_dr(dev, &dr->node);

	// 解锁设备资源锁，恢复之前保存的中断状态
	spin_unlock_irqrestore(&dev->devres_lock, flags);
}

EXPORT_SYMBOL_GPL(devres_add);

/**
 * find_dr - 在设备的资源列表中查找资源
 * @dev: 要搜索的设备
 * @release: 资源释放函数，用于匹配资源
 * @match: 自定义匹配函数，可以为 NULL
 * @match_data: 传递给匹配函数的数据
 *
 * 遍历 @dev 的资源列表，查找与给定条件匹配的资源。这里的匹配条件包括
 * 释放函数和可选的自定义匹配函数。
 *
 * 返回找到的资源的指针，如果没有找到符合条件的资源则返回 NULL。
 */
static struct devres *find_dr(struct device *dev, dr_release_t release,
			      dr_match_t match, void *match_data)
{
	struct devres_node *node;  // 定义一个指向资源节点的指针

	// 从设备的资源列表的末尾开始向前遍历，因为最近添加的资源可能是最先需要访问的
	list_for_each_entry_reverse(node, &dev->devres_head, entry) {
		struct devres *dr = container_of(node, struct devres, node);  // 从节点获取资源的实际地址

		if (node->release != release)  // 如果节点的释放函数不匹配，则跳过该节点
			continue;
		if (match && !match(dev, dr->data, match_data))  // 如果提供了匹配函数且该函数返回假，也跳过
			continue;

		return dr;  // 找到匹配的资源，返回资源指针
	}

	return NULL;  // 没有找到匹配的资源，返回 NULL
}

/**
 * devres_find - Find device resource
 * @dev: Device to lookup resource from
 * @release: Look for resources associated with this release function
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev which is associated with @release
 * and for which @match returns 1.  If @match is NULL, it's considered
 * to match all.
 *
 * RETURNS:
 * Pointer to found devres, NULL if not found.
 */
/**
 * devres_find - 查找设备资源
 * @dev: 需要查找资源的设备
 * @release: 查找与此释放函数关联的资源
 * @match: 匹配函数（可选）
 * @match_data: 传递给匹配函数的数据
 *
 * 查找与 @dev 关联的、与 @release 释放函数关联且满足 @match 返回 1 的最新 devres。
 * 如果 @match 为 NULL，则认为所有资源都匹配。
 *
 * 返回值:
 * 找到的 devres 的指针，如果未找到则返回 NULL。
 */
void * devres_find(struct device *dev, dr_release_t release,
		   dr_match_t match, void *match_data)
{
	struct devres *dr;  // 定义一个指向 devres 的指针
	unsigned long flags;  // 定义一个用于保存中断状态的变量

	spin_lock_irqsave(&dev->devres_lock, flags);  // 获取自旋锁，保存中断状态
	dr = find_dr(dev, release, match, match_data);  // 调用 find_dr 函数查找匹配的资源
	spin_unlock_irqrestore(&dev->devres_lock, flags);  // 释放自旋锁，恢复中断状态

	if (dr)  // 如果找到了匹配的资源
		return dr->data;  // 返回资源的数据部分
	return NULL;  // 如果未找到资源，返回 NULL
}
EXPORT_SYMBOL_GPL(devres_find);

/**
 * devres_get - Find devres, if non-existent, add one atomically
 * @dev: Device to lookup or add devres for
 * @new_res: Pointer to new initialized devres to add if not found
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev which has the same release function
 * as @new_res and for which @match return 1.  If found, @new_res is
 * freed; otherwise, @new_res is added atomically.
 *
 * RETURNS:
 * Pointer to found or added devres.
 */
/**
 * devres_get - 查找设备资源，如果不存在则原子性地添加
 * @dev: 需要查找或添加资源的设备
 * @new_res: 如果未找到则需要添加的已初始化的新资源的指针
 * @match: 匹配函数（可选）
 * @match_data: 传递给匹配函数的数据
 *
 * 查找 @dev 的最新 devres，该资源具有与 @new_res 相同的释放函数，
 * 并且匹配函数 @match 返回 1。如果找到，则释放 @new_res；
 * 否则，将 @new_res 原子性地添加。
 *
 * 返回值:
 * 找到或添加的 devres 的指针。
 */
void * devres_get(struct device *dev, void *new_res,
		  dr_match_t match, void *match_data)
{
	struct devres *new_dr = container_of(new_res, struct devres, data);	// 从 new_res 数据指针获取 devres 结构体指针
	struct devres *dr;	// 用于存储找到的或将要添加的 devres 结构体
	unsigned long flags;	// 用于保存中断状态

	spin_lock_irqsave(&dev->devres_lock, flags);	// 获取自旋锁并保存中断状态
	dr = find_dr(dev, new_dr->node.release, match, match_data);	// 调用 find_dr 查找匹配的 devres
	if (!dr) {
		add_dr(dev, &new_dr->node);	// 添加新的 devres
		dr = new_dr;	// 更新 dr 为新添加的 devres
		new_dr = NULL;	// 清空 new_dr 指针，避免后面错误释放
	}
	spin_unlock_irqrestore(&dev->devres_lock, flags);	// 释放自旋锁并恢复中断状态
	devres_free(new_dr);	// 如果 new_dr 非空，则释放它

	return dr->data;	// 返回找到或添加的 devres 的数据部分
}
EXPORT_SYMBOL_GPL(devres_get);

/**
 * devres_remove - Find a device resource and remove it
 * @dev: Device to find resource from
 * @release: Look for resources associated with this release function
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev associated with @release and for
 * which @match returns 1.  If @match is NULL, it's considered to
 * match all.  If found, the resource is removed atomically and
 * returned.
 *
 * RETURNS:
 * Pointer to removed devres on success, NULL if not found.
 */
/**
 * devres_remove - 查找设备资源并移除
 * @dev: 要查找资源的设备
 * @release: 查找与此释放函数关联的资源
 * @match: 匹配函数（可选）
 * @match_data: 传递给匹配函数的数据
 *
 * 查找与 @release 关联且匹配函数 @match 返回 1 的 @dev 的最新 devres。
 * 如果 @match 为 NULL，则视为匹配所有。如果找到，则资源被原子性地移除并返回。
 *
 * 返回值:
 * 成功时返回移除的 devres 的指针，未找到时返回 NULL。
 */
void * devres_remove(struct device *dev, dr_release_t release,
		     dr_match_t match, void *match_data)
{
	struct devres *dr;  // 用于存储找到的 devres
	unsigned long flags;  // 用于保存中断状态

	spin_lock_irqsave(&dev->devres_lock, flags);  // 获取自旋锁并保存中断状态
	dr = find_dr(dev, release, match, match_data);  // 调用 find_dr 查找匹配的 devres
	if (dr) {  // 如果找到对应的 devres
		list_del_init(&dr->node.entry);  // 从设备的资源列表中移除该资源
		devres_log(dev, &dr->node, "REM");  // 记录资源移除的日志
	}
	spin_unlock_irqrestore(&dev->devres_lock, flags);  // 释放自旋锁并恢复中断状态

	if (dr)  // 如果找到资源
		return dr->data;  // 返回资源的数据部分
	return NULL;  // 如果未找到资源，返回 NULL
}
EXPORT_SYMBOL_GPL(devres_remove);

/**
 * devres_destroy - Find a device resource and destroy it
 * @dev: Device to find resource from
 * @release: Look for resources associated with this release function
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev associated with @release and for
 * which @match returns 1.  If @match is NULL, it's considered to
 * match all.  If found, the resource is removed atomically and freed.
 *
 * RETURNS:
 * 0 if devres is found and freed, -ENOENT if not found.
 */
/**
 * devres_destroy - 查找并销毁设备资源
 * @dev: 从中查找资源的设备
 * @release: 查找与此释放函数关联的资源
 * @match: 匹配函数（可选）
 * @match_data: 匹配函数的数据
 *
 * 查找与 @release 关联且匹配函数 @match 返回 1 的 @dev 的最新 devres。
 * 如果 @match 为 NULL，则视为匹配所有。如果找到，资源将被原子性地移除并释放。
 *
 * 返回值:
 * 如果找到并释放了 devres，则返回 0；如果未找到，则返回 -ENOENT。
 */
int devres_destroy(struct device *dev, dr_release_t release,
		   dr_match_t match, void *match_data)
{
	void *res;  // 用于存储找到的资源指针

	res = devres_remove(dev, release, match, match_data);  // 移除匹配的资源
	if (unlikely(!res))  // 如果未找到资源
		return -ENOENT;  // 返回 -ENOENT（没有对应条目）

	devres_free(res);  // 释放资源
	return 0;  // 返回 0 表示成功
}
EXPORT_SYMBOL_GPL(devres_destroy);


// color = 0：组尚未被处理。
// color = 1：表示组的某一部分（通常是开始部分）处在当前处理的范围内。
// color = 2：表示整个组完全位于当前处理的范围内（闭合组），或者是一个开放组的开始标记位于范围内。
/*
 * 从设备的资源管理列表中移除一系列节点的函数。具体来说，它处理两种类型的资源：
 * 普通的资源条目和资源组。资源组由一对节点表示：开组节点和闭组节点。
 * 函数分两次遍历，首先移动普通资源到另一个列表并初始化资源组的标记，然后处理资源组的颜色标记来决定是否移动整个资源组。
 */
static int remove_nodes(struct device *dev,
			struct list_head *first, struct list_head *end,
			struct list_head *todo)
{
	int cnt = 0, nr_groups = 0;	// cnt 记录移动的普通节点数，nr_groups 记录资源组的数量
	struct list_head *cur;

	/* First pass - move normal devres entries to @todo and clear
	 * devres_group colors.
	 */
	/* 第一次遍历 - 将普通的devres条目移动到@todo链表并清除devres_group的color字段 */
	cur = first;
	while (cur != end) {	// 遍历从first到end的列表
		struct devres_node *node;
		struct devres_group *grp;

		node = list_entry(cur, struct devres_node, entry);	// 获取当前节点
		cur = cur->next;	// 移动到下一个节点

		grp = node_to_group(node);	// 判断当前节点是否为资源组节点
		if (grp) {	// 如果是资源组
			/* clear color of group markers in the first pass */
			/* 在第一次遍历中清除组标记的color */
			grp->color = 0;	// 在第一次遍历中清除资源组标记的颜色
			nr_groups++;	// 资源组计数
		} else {	// 如果是单独的资源
			/* regular devres entry */
			/* 处理常规的devres条目 */
			/* 普通资源条目 */
			if (&node->entry == first)
				first = first->next;	// 更新first指针
			list_move_tail(&node->entry, todo);	// 将节点移动到 todo 列表
			cnt++;	// 计数普通资源节点
		}
	}

	if (!nr_groups)
		return cnt;	// 如果没有资源组，直接返回计数

	/* Second pass - Scan groups and color them.  A group gets
	 * color value of two iff the group is wholly contained in
	 * [cur, end).  That is, for a closed group, both opening and
	 * closing markers should be in the range, while just the
	 * opening marker is enough for an open group.
	 */
	/* 
	 * 第二次遍历 - 扫描资源组并设置它们的color。如果一个组完全包含在[cur, end)范围内，
	 * 则color值为2。对于一个闭合组，其开头和结尾标记都应该在范围内；对于一个开放组，
	 * 只需开头标记在范围内即可。
	 */
	// 第二遍扫描 - 扫描资源组并对它们进行处理
	cur = first;
	while (cur != end) {
		struct devres_node *node;
		struct devres_group *grp;

		node = list_entry(cur, struct devres_node, entry);	// 获取当前节点
		cur = cur->next;	// 移动到下一个节点

		grp = node_to_group(node);
		BUG_ON(!grp || list_empty(&grp->node[0].entry));	// 断言检查，确保资源组节点正确

		grp->color++;	// 为资源组上色
		// 如果grp->node[1].entry是空，也就意味着并没有调用devres_close_group来关闭资源组，
		// 在此处执行grp->color++让color变为2，在下面直接将整个资源组移动到待释放链表就好了
		// 如果grp->node[1].entry不是空，也就意味着调用了devres_close_group来关闭资源组，
		// 也就意味着grp->node[1]和grp->node[0]都在cur - end的范围内，也就是说同一个组会循环两次，
		// 在第二次的时候会把整个组加入到待释放链表中

		// 如果grp->node[1].entry为空，表明没有调用devres_close_group来关闭资源组，
		// 此时再次增加color，使其达到2，然后将整个资源组移动到待释放链表。
		// 如果grp->node[1].entry非空，表示已调用devres_close_group关闭资源组，
		// 此时grp->node[1]和grp->node[0]都将在cur到end的范围内，即同一个组会在此循环中被处理两次，
		// 在第二次遍历时会将整个组移入待释放链表。
		if (list_empty(&grp->node[1].entry))
			grp->color++;	// 如果资源组是开放的（开放组，没有结束标记）,额外上色

		BUG_ON(grp->color <= 0 || grp->color > 2);	// 颜色应该在1到2之间
		if (grp->color == 2) {	// 如果资源组完全在[first, end)区间内
			/* No need to update cur or end.  The removed
			 * nodes are always before both.
			 */
			/* 不需要更新cur或end，被移除的节点总是位于两者之前 */
			/* 将资源组从列表中移除，并移到 todo 列表 */
			list_move_tail(&grp->node[0].entry, todo);	// 将开组节点移动到todo列表
			list_del_init(&grp->node[1].entry);	// 从列表中移除闭组节点
		}
	}

	return cnt;	// 返回移动的普通资源节点数量
}

// 释放与设备关联的一系列资源节点。这个函数首先通过调用 remove_nodes 
// 函数将特定范围内的资源节点移动到一个临时列表中，然后释放这些资源。
static int release_nodes(struct device *dev, struct list_head *first,
			 struct list_head *end, unsigned long flags)
{
	LIST_HEAD(todo);	// 定义并初始化一个临时链表头用于存储待释放的资源
	int cnt;	// 用于存储移除的节点数量
	struct devres *dr, *tmp;	// 定义两个指针用于遍历待处理资源列表

	cnt = remove_nodes(dev, first, end, &todo);	// 移除first到end之间的所有节点，并将它们添加到todo列表

	spin_unlock_irqrestore(&dev->devres_lock, flags);	// 释放之前获取的锁，并恢复之前的中断状态

	/* Release.  Note that both devres and devres_group are
	 * handled as devres in the following loop.  This is safe.
	 */
	/* 
	 * 释放资源。注意，devres和devres_group都作为devres处理。
	 * 这样做是安全的。
	 */
	list_for_each_entry_safe_reverse(dr, tmp, &todo, node.entry) { // 安全反向遍历todo列表
		devres_log(dev, &dr->node, "REL"); // 记录资源释放的日志
		dr->node.release(dev, dr->data); // 调用资源的释放函数
		kfree(dr); // 释放资源结构体占用的内存
	}

	return cnt; // 返回处理的节点数量
}

/**
 * devres_release_all - Release all managed resources
 * @dev: Device to release resources for
 *
 * Release all resources associated with @dev.  This function is
 * called on driver detach.
 */
/**
 * devres_release_all - 释放所有管理的资源
 * @dev: 需要释放资源的设备
 *
 * 释放与 @dev 关联的所有资源。这个函数通常在驱动程序解绑时被调用。
 */
// 释放与给定设备关联的所有管理资源
int devres_release_all(struct device *dev)
{
	unsigned long flags;	// 用于保存中断状态

	/* Looks like an uninitialized device structure */
	// 检查设备的资源链表是否已初始化，未初始化则打印警告并返回错误
	if (WARN_ON(dev->devres_head.next == NULL))
		return -ENODEV;
	// 加锁保护设备资源链表，同时保存中断状态
	spin_lock_irqsave(&dev->devres_lock, flags);
	// 调用 release_nodes 函数释放所有资源，传入资源链表的开始和结束位置
	return release_nodes(dev, dev->devres_head.next, &dev->devres_head,
			     flags);
}

/**
 * devres_open_group - Open a new devres group
 * @dev: Device to open devres group for
 * @id: Separator ID
 * @gfp: Allocation flags
 *
 * Open a new devres group for @dev with @id.  For @id, using a
 * pointer to an object which won't be used for another group is
 * recommended.  If @id is NULL, address-wise unique ID is created.
 *
 * RETURNS:
 * ID of the new group, NULL on failure.
 */
/**
 * devres_open_group - 开启一个新的设备资源组
 * @dev: 需要开启设备资源组的设备
 * @id: 分隔符 ID
 * @gfp: 分配标志
 *
 * 为 @dev 设备开启一个新的资源组，并使用 @id 作为标识。对于 @id，建议使用一个不会被其他组使用的对象指针。
 * 如果 @id 为空，则创建一个地址唯一的 ID。
 *
 * 返回值:
 * 新组的 ID，如果失败则返回 NULL。
 */
// 指定设备开启一个新的设备资源（devres）组。每个资源组由一个起始和一个结束标记节点组成，用于管理和释放设备资源的逻辑分组。
void * devres_open_group(struct device *dev, void *id, gfp_t gfp)
{
	struct devres_group *grp;	// 定义一个指向设备资源组的指针
	unsigned long flags;	// 用于保存中断状态

	// 为资源组分配内存，使用传入的gfp参数指定的内存分配策略
	grp = kmalloc(sizeof(*grp), gfp);
	if (unlikely(!grp))	// 如果内存分配失败
		return NULL;	// 返回 NULL

	// 初始化资源组的节点，这两个节点分别代表资源组的开启和关闭标记
	grp->node[0].release = &group_open_release; // 设置开启标记的释放函数
	grp->node[1].release = &group_close_release; // 设置关闭标记的释放函数
	INIT_LIST_HEAD(&grp->node[0].entry); // 初始化开启标记的列表头
	INIT_LIST_HEAD(&grp->node[1].entry); // 初始化关闭标记的列表头
	set_node_dbginfo(&grp->node[0], "grp<", 0); // 设置开启标记的调试信息
	set_node_dbginfo(&grp->node[1], "grp>", 0); // 设置关闭标记的调试信息
	grp->id = grp; // 默认使用资源组的地址作为ID
	if (id) // 如果调用者提供了ID
		grp->id = id; // 使用提供的ID

	spin_lock_irqsave(&dev->devres_lock, flags);	// 锁定设备的devres锁并保存当前的中断状态
	add_dr(dev, &grp->node[0]);	// 将开启标记节点添加到设备的devres列表中
	spin_unlock_irqrestore(&dev->devres_lock, flags);	// 解锁并恢复之前的中断状态
	return grp->id;	// 返回资源组的ID
}
EXPORT_SYMBOL_GPL(devres_open_group);

/* Find devres group with ID @id.  If @id is NULL, look for the latest. */
/**
 * find_group - 查找具有给定 ID 的资源组。如果 ID 为 NULL，则查找最新的资源组。
 * @dev: 要查找资源组的设备
 * @id: 资源组的标识符。如果为 NULL，查找最后一个打开的资源组。
 *
 * 这个函数遍历设备的 devres 链表，从最后一个元素向前查找，尝试找到与提供的 ID 匹配的资源组。
 * 如果 @id 为 NULL，则返回最后一个打开的资源组。
 *
 * 返回值:
 * 找到的 devres_group 结构体指针，如果没有找到合适的资源组则返回 NULL。
 */
static struct devres_group * find_group(struct device *dev, void *id)
{
	struct devres_node *node;

	// 从设备的 devres 链表的末尾开始向前查找
	list_for_each_entry_reverse(node, &dev->devres_head, entry) {
		struct devres_group *grp;

		// 如果当前节点的释放函数不是 group_open_release，则跳过，继续查找
		if (node->release != &group_open_release)
			continue;

		// 获取包含当前节点的 devres_group 结构
		grp = container_of(node, struct devres_group, node[0]);

		// 如果提供了 id，检查当前组的 id 是否与之匹配
		if (id) {
			if (grp->id == id)
				return grp;
		} else if (list_empty(&grp->node[1].entry))
			// 如果没有提供 id，则查找最后一个开放的资源组（没有关闭标记的组）
			return grp;
	}

	return NULL;	// 如果没有找到匹配的资源组，返回 NULL
}

/**
 * devres_close_group - Close a devres group
 * @dev: Device to close devres group for
 * @id: ID of target group, can be NULL
 *
 * Close the group identified by @id.  If @id is NULL, the latest open
 * group is selected.
 */
/**
 * devres_close_group - 关闭一个设备资源组
 * @dev: 需要关闭资源组的设备
 * @id: 目标组的标识符，可以为 NULL
 *
 * 关闭由 @id 标识的资源组。如果 @id 为 NULL，则选择最近打开的资源组。
 */
void devres_close_group(struct device *dev, void *id)
{
	struct devres_group *grp;  // 用于存储找到的资源组
	unsigned long flags;       // 用于保存中断状态

	spin_lock_irqsave(&dev->devres_lock, flags);  // 加锁以保护资源链表的操作

	grp = find_group(dev, id);  // 查找指定 ID 的资源组
	if (grp)
		add_dr(dev, &grp->node[1]);  // 如果找到了资源组，则添加一个关闭节点
	else
		WARN_ON(1);  // 如果没有找到资源组，则发出警告

	spin_unlock_irqrestore(&dev->devres_lock, flags);  // 解锁
}
EXPORT_SYMBOL_GPL(devres_close_group);

/**
 * devres_remove_group - Remove a devres group
 * @dev: Device to remove group for
 * @id: ID of target group, can be NULL
 *
 * Remove the group identified by @id.  If @id is NULL, the latest
 * open group is selected.  Note that removing a group doesn't affect
 * any other resources.
 */
void devres_remove_group(struct device *dev, void *id)
{
	struct devres_group *grp;
	unsigned long flags;

	spin_lock_irqsave(&dev->devres_lock, flags);

	grp = find_group(dev, id);
	if (grp) {
		list_del_init(&grp->node[0].entry);
		list_del_init(&grp->node[1].entry);
		devres_log(dev, &grp->node[0], "REM");
	} else
		WARN_ON(1);

	spin_unlock_irqrestore(&dev->devres_lock, flags);

	kfree(grp);
}
EXPORT_SYMBOL_GPL(devres_remove_group);

/**
 * devres_release_group - Release resources in a devres group
 * @dev: Device to release group for
 * @id: ID of target group, can be NULL
 *
 * Release all resources in the group identified by @id.  If @id is
 * NULL, the latest open group is selected.  The selected group and
 * groups properly nested inside the selected group are removed.
 *
 * RETURNS:
 * The number of released non-group resources.
 */
/**
 * devres_release_group - 释放设备资源组中的资源
 * @dev: 需要释放资源的设备
 * @id: 目标组的标识符，可以为 NULL
 *
 * 释放由 @id 标识的资源组中的所有资源。如果 @id 为 NULL，则选择最近打开的资源组。
 * 被选中的组以及适当嵌套在所选组内的组将被移除。
 *
 * 返回值:
 * 释放的非组资源的数量。
 */
int devres_release_group(struct device *dev, void *id)
{
	struct devres_group *grp;  // 用于存储找到的资源组
	unsigned long flags;       // 用于保存中断状态
	int cnt = 0;               // 记录释放的资源数量

	spin_lock_irqsave(&dev->devres_lock, flags);  // 加锁以保护资源链表的操作

	grp = find_group(dev, id);  // 查找指定 ID 的资源组
	if (grp) {
		struct list_head *first = &grp->node[0].entry;  // 组的开始节点
		struct list_head *end = &dev->devres_head;     // 默认设为设备资源列表的末尾

		if (!list_empty(&grp->node[1].entry))
			end = grp->node[1].entry.next;  // 如果组已关闭，则设为组的结束节点的下一个节点

		cnt = release_nodes(dev, first, end, flags);  // 释放这个范围内的资源节点
	} else {
		WARN_ON(1);  // 如果没有找到资源组，则发出警告
		spin_unlock_irqrestore(&dev->devres_lock, flags);
	}

	return cnt;  // 返回释放的非组资源的数量
}
EXPORT_SYMBOL_GPL(devres_release_group);

/*
 * Managed kzalloc/kfree
 */
/*
 * 管理的 kzalloc/kfree
 */
static void devm_kzalloc_release(struct device *dev, void *res)
{
	/* noop */
	// 这个释放函数实际上不执行任何操作（No operation），因为释放动作被封装在其他函数中处理
}

static int devm_kzalloc_match(struct device *dev, void *res, void *data)
{
	// 该函数检查传入的资源指针 res 是否与用户提供的比较数据 data 相等
	// 如果相等，返回 1，表示匹配成功；否则返回 0
	return res == data;
}

/**
 * devm_kzalloc - Resource-managed kzalloc
 * @dev: Device to allocate memory for
 * @size: Allocation size
 * @gfp: Allocation gfp flags
 *
 * Managed kzalloc.  Memory allocated with this function is
 * automatically freed on driver detach.  Like all other devres
 * resources, guaranteed alignment is unsigned long long.
 *
 * RETURNS:
 * Pointer to allocated memory on success, NULL on failure.
 */
/**
 * devm_kzalloc - 资源管理的 kzalloc
 * @dev: 需要为其分配内存的设备
 * @size: 分配的大小
 * @gfp: 分配的 gfp 标志
 *
 * 资源管理的 kzalloc。使用此函数分配的内存将在驱动程序从设备分离时自动释放。
 * 像所有其他的devres资源一样，保证了unsigned long long的对齐。
 *
 * 返回：
 * 成功时返回指向分配的内存的指针，失败时返回NULL。
 */
void * devm_kzalloc(struct device *dev, size_t size, gfp_t gfp)
{
	struct devres *dr;

	/* use raw alloc_dr for kmalloc caller tracing */
	// 使用原始 alloc_dr 函数进行 kmalloc 调用跟踪
	dr = alloc_dr(devm_kzalloc_release, size, gfp);
	if (unlikely(!dr))
		return NULL;	// 如果内存分配失败，返回NULL

	// 设置节点调试信息，用于追踪资源释放
	set_node_dbginfo(&dr->node, "devm_kzalloc_release", size);
	devres_add(dev, dr->data);	// 将新分配的内存添加到设备的资源列表中
	return dr->data;	// 返回指向分配内存的指针
}
EXPORT_SYMBOL_GPL(devm_kzalloc);

/**
 * devm_kfree - Resource-managed kfree
 * @dev: Device this memory belongs to
 * @p: Memory to free
 *
 * Free memory allocated with dev_kzalloc().
 */
/**
 * devm_kfree - 资源管理的 kfree
 * @dev: 这块内存所属的设备
 * @p: 需要释放的内存
 *
 * 释放通过 devm_kzalloc() 分配的内存。
 */
void devm_kfree(struct device *dev, void *p)
{
	int rc;	// 用于存储 devres_destroy 函数的返回值

	// 尝试销毁和释放指定的资源，确保这块内存之前是通过 devm_kzalloc 分配的
	rc = devres_destroy(dev, devm_kzalloc_release, devm_kzalloc_match, p);
	WARN_ON(rc);	// 如果 devres_destroy 返回错误（非0值），打印警告信息
}
EXPORT_SYMBOL_GPL(devm_kfree);
