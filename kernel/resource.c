/*
 *	linux/kernel/resource.c
 *
 * Copyright (C) 1999	Linus Torvalds
 * Copyright (C) 1999	Martin Mares <mj@ucw.cz>
 *
 * Arbitrary resource management.
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/device.h>
#include <linux/pfn.h>
#include <asm/io.h>

/*
 * I/O Ports (ioport)
 * 定义：I/O端口是一种传统的资源，特别是在x86架构中，用于通过端口映射的方式控制硬件设备。
 * 用途：主要用于较老的或简单的设备（如早期的声卡、并口设备等），这些设备不需要大量的数据传输，而是依靠简单的读写操作来进行控制。
 * 管理：通过ioport_resource结构体来表示系统中所有的I/O端口资源，以确保资源的合理分配和使用。
 */
struct resource ioport_resource = {
	.name	= "PCI IO",        // 资源名称为 "PCI IO"
	.start	= 0,              // 资源起始地址为 0
	.end	= IO_SPACE_LIMIT, // 资源结束地址为 IO_SPACE_LIMIT，表示I/O空间的上限
	.flags	= IORESOURCE_IO,  // 资源类型标志为 I/O 资源
};
EXPORT_SYMBOL(ioport_resource);

/*
 * I/O Memory (iomem)
 * 定义：I/O内存（iomem）是一种内存映射的I/O资源，用于映射到物理硬件设备的内存。这允许CPU直接通过内存访问的方式与硬件设备交互，可以通过读写这些特定的内存地址来控制硬件。
 * 用途：广泛用于现代计算机系统中，尤其是在与复杂硬件如网络卡、显卡等进行高速数据交换时。
 * 管理：通过iomem_resource结构体来表示系统中所有的I/O内存资源，确保对这些资源的管理是集中和系统化的。
 */
struct resource iomem_resource = {
	.name	= "PCI mem",      // 资源名称为 "PCI mem"
	.start	= 0,              // 资源起始地址为 0
	.end	= -1,             // 资源结束地址为 -1，表示内存空间的上限
	.flags	= IORESOURCE_MEM, // 资源类型标志为内存资源
};
EXPORT_SYMBOL(iomem_resource);

static DEFINE_RWLOCK(resource_lock);	// 定义并初始化读写锁 resource_lock

static void *r_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct resource *p = v; // 将当前资源节点 v 转换为资源指针 p
	(*pos)++; // 位置计数器 pos 增加 1
	if (p->child) // 如果当前资源节点有子节点
		return p->child; // 返回子节点
	while (!p->sibling && p->parent) // 如果没有兄弟节点且有父节点
		p = p->parent; // 向上移动到父节点
	return p->sibling; // 返回兄弟节点
}

#ifdef CONFIG_PROC_FS

enum { MAX_IORES_LEVEL = 5 };	// 定义最大资源层级深度为5

/*
 * r_start - 为序列文件操作提供起始资源指针
 * @m: 序列文件的结构体，存储了输出的上下文
 * @pos: 位置指针，表示需要获取的资源的位置
 *
 * 该函数加锁读取资源树，并返回与给定位置对应的资源。
 * 如果该位置之前没有资源，则返回NULL。
 */
static void *r_start(struct seq_file *m, loff_t *pos)
	__acquires(resource_lock)	// 通过这个属性告诉分析器此函数将获取一个锁
{
	struct resource *p = m->private;  // 从序列文件私有数据获取资源的起始点
	loff_t l = 0;  // 初始化位置计数器
	read_lock(&resource_lock);  // 获取读锁以安全地访问全局资源树
	for (p = p->child; p && l < *pos; p = r_next(m, p, &l))  // 遍历子资源，直到找到对应的位置
		;  // 循环体为空，实际工作在循环条件中完成
	return p;  // 返回找到的资源，或者在到达列表末尾时返回NULL
}

static void r_stop(struct seq_file *m, void *v)
	__releases(resource_lock)
{
	read_unlock(&resource_lock);	// 释放资源锁
}

static int r_show(struct seq_file *m, void *v)
{
	struct resource *root = m->private;	// 获取根资源节点
	struct resource *r = v, *p;	// 当前资源节点 r 和遍历用的指针 p
	int width = root->end < 0x10000 ? 4 : 8;	// 根据根资源节点的结束地址确定地址宽度
	int depth;	// 节点深度

	// 计算当前资源节点的深度
	for (depth = 0, p = r; depth < MAX_IORES_LEVEL; depth++, p = p->parent)
		if (p->parent == root)
			break;
	// 打印资源信息
	seq_printf(m, "%*s%0*llx-%0*llx : %s\n",
			depth * 2, "",	// 缩进空格
			width, (unsigned long long) r->start,	// 起始地址
			width, (unsigned long long) r->end,		// 结束地址
			r->name ? r->name : "<BAD>");					// 资源名称
	return 0;
}

static const struct seq_operations resource_op = {
	.start	= r_start,	// 迭代开始函数
	.next	= r_next,			// 迭代下一个函数
	.stop	= r_stop,			// 迭代停止函数
	.show	= r_show,			// 显示函数
};

static int ioports_open(struct inode *inode, struct file *file)
{
	// 使用资源操作结构体打开序列文件
	int res = seq_open(file, &resource_op);
	if (!res) { // 如果打开成功
		struct seq_file *m = file->private_data; // 获取序列文件的私有数据
		m->private = &ioport_resource; // 将 ioport_resource 设置为序列文件的私有数据
	}
	return res; // 返回操作结果
}

static int iomem_open(struct inode *inode, struct file *file)
{
	// 使用资源操作结构体打开序列文件
	int res = seq_open(file, &resource_op);
	if (!res) { // 如果打开成功
		struct seq_file *m = file->private_data; // 获取序列文件的私有数据
		m->private = &iomem_resource; // 将 iomem_resource 设置为序列文件的私有数据
	}
	return res; // 返回操作结果
}

// 定义针对 /proc/ioports 文件的操作
static const struct file_operations proc_ioports_operations = {
	.open		= ioports_open,  // 打开文件时调用 ioports_open 函数
	.read		= seq_read,      // 读取文件时调用 seq_read 函数
	.llseek		= seq_lseek,     // 文件定位时调用 seq_lseek 函数
	.release	= seq_release,   // 关闭文件时调用 seq_release 函数
};

// 定义针对 /proc/iomem 文件的操作
static const struct file_operations proc_iomem_operations = {
	.open		= iomem_open,    // 打开文件时调用 iomem_open 函数
	.read		= seq_read,      // 读取文件时调用 seq_read 函数
	.llseek		= seq_lseek,     // 文件定位时调用 seq_lseek 函数
	.release	= seq_release,   // 关闭文件时调用 seq_release 函数
};

// 初始化 I/O 资源，并创建相关的 proc 文件
static int __init ioresources_init(void)
{
	// 在 /proc 目录下创建 "ioports" 文件，不设定任何默认权限，使用 proc_ioports_operations 中定义的操作
	proc_create("ioports", 0, NULL, &proc_ioports_operations);
	// 在 /proc 目录下创建 "iomem" 文件，同样不设定任何默认权限，使用 proc_iomem_operations 中定义的操作
	proc_create("iomem", 0, NULL, &proc_iomem_operations);
	return 0;	// 初始化成功返回 0
}
__initcall(ioresources_init);	// 告诉内核启动时调用 ioresources_init 函数

#endif /* CONFIG_PROC_FS */

/* Return the conflict entry if you can't request it */
/* 如果无法请求资源，则返回冲突条目 */
// 如果找到冲突资源，则返回冲突的资源指针。如果没有冲突，则返回 NULL，表示请求成功。
static struct resource * __request_resource(struct resource *root, struct resource *new)
{
	 // 定义新资源的开始和结束位置
	resource_size_t start = new->start;
	resource_size_t end = new->end;
	struct resource *tmp, **p;

	// 检查结束位置是否小于开始位置，如果是则返回根资源
	if (end < start)
		return root;
	// 检查新资源的开始位置是否小于根资源的开始位置，如果是则返回根资源
	if (start < root->start)
		return root;
	// 检查新资源的结束位置是否大于根资源的结束位置，如果是则返回根资源
	if (end > root->end)
		return root;
	// 将指针 p 指向根资源的子资源链表
	p = &root->child;
	for (;;) {
		// 获取当前指针 p 指向的资源
		tmp = *p;
		// 如果当前资源为空或当前资源的开始位置大于新资源的结束位置
		if (!tmp || tmp->start > end) {
			// 将新资源的兄弟指针指向当前资源
			new->sibling = tmp;
			*p = new;	// 将当前指针 p 指向新资源
			new->parent = root;	// 将新资源的父资源指向根资源
			return NULL;	// 返回空，表示请求成功
		}
		p = &tmp->sibling;	// 将指针 p 指向当前资源的兄弟资源
		if (tmp->end < start)	// 如果当前资源的结束位置小于新资源的开始位置，则继续循环
			continue;
		// 返回当前资源，表示存在冲突
		return tmp;
	}
}

/*
 * 这个函数用于释放资源。
 * 
 * 1. 将指针 p 指向旧资源的父资源的子资源链表。
 * 2. 遍历子资源链表，寻找与旧资源匹配的资源。
 * 3. 如果找到旧资源，则将旧资源从链表中移除，并将其父资源指针置为空，返回 0 表示释放成功。
 * 4. 如果未找到旧资源，则返回 -EINVAL 表示无效参数。
 */
static int __release_resource(struct resource *old)
{
	struct resource *tmp, **p;

	// 将指针 p 指向旧资源的父资源的子资源链表
	p = &old->parent->child;
	for (;;) {
		// 获取当前指针 p 指向的资源
		tmp = *p;
		// 如果当前资源为空，则退出循环
		if (!tmp)
			break;
		// 如果当前资源是旧资源
		if (tmp == old) {
			*p = tmp->sibling;	// 将当前指针 p 指向旧资源的兄弟资源
			old->parent = NULL;	// 将旧资源的父资源指针置为空
			return 0;	 // 返回 0，表示释放成功
		}
		p = &tmp->sibling;	// 将指针 p 指向当前资源的兄弟资源
	}
	return -EINVAL;	// 如果未找到旧资源，则返回 -EINVAL，表示无效参数
}

/*
 * 这个函数用于递归释放一个资源的所有子资源。
 *
 * 1. 将子资源指针 p 指向当前资源 r 的第一个子资源。
 * 2. 将当前资源 r 的子资源指针置为空。
 * 3. 遍历所有子资源：
 *    - 临时存储当前子资源。
 *    - 将子资源指针 p 移动到下一个兄弟资源。
 *    - 将当前子资源的父资源指针和兄弟资源指针置为空。
 *    - 递归释放当前子资源的子资源。
 *    - 打印调试信息，释放子资源。
 *    - 恢复子资源的大小，并保留标志，将子资源的起始地址置为 0，结束地址置为 size - 1。
 */
static void __release_child_resources(struct resource *r)
{
	struct resource *tmp, *p;
	resource_size_t size;

	// 将子资源指针 p 指向当前资源 r 的第一个子资源
	p = r->child;
	// 将当前资源 r 的子资源指针置为空
	r->child = NULL;
	// 遍历所有子资源
	while (p) {
		tmp = p;	// 临时存储当前子资源
		p = p->sibling;	 // 将子资源指针 p 移动到下一个兄弟资源

		// 将当前子资源的父资源指针和兄弟资源指针置为空
		tmp->parent = NULL;
		tmp->sibling = NULL;
		// 递归释放当前子资源的子资源
		__release_child_resources(tmp);

		// 打印调试信息，释放子资源
		printk(KERN_DEBUG "release child resource %pR\n", tmp);
		/* need to restore size, and keep flags */
		// 需要恢复大小，并保留标志
		size = resource_size(tmp);
		tmp->start = 0;
		tmp->end = size - 1;
	}
}

/*
 * release_child_resources - 释放资源的所有子资源
 * @r: 要释放子资源的父资源
 *
 * 这个函数首先获取写锁来保护资源树结构，然后调用内部函数
 * __release_child_resources 来递归释放资源 r 的所有子资源。
 * 最后释放写锁。
 */
void release_child_resources(struct resource *r)
{
	write_lock(&resource_lock);		// 获取写锁，以保护资源树结构
	__release_child_resources(r);	// 调用内部函数递归释放子资源
	write_unlock(&resource_lock);	// 释放写锁
}

/**
 * request_resource_conflict - request and reserve an I/O or memory resource
 * @root: root resource descriptor
 * @new: resource descriptor desired by caller
 *
 * Returns 0 for success, conflict resource on error.
 */
/**
 * request_resource_conflict - 请求并保留一个I/O或内存资源
 * @root: 根资源描述符
 * @new: 调用者所需的资源描述符
 *
 * 返回0表示成功，发生冲突时返回冲突的资源。
 */
struct resource *request_resource_conflict(struct resource *root, struct resource *new)
{
	struct resource *conflict;

	write_lock(&resource_lock);	// 获取写锁，以保护资源树结构
	conflict = __request_resource(root, new);	// 尝试请求资源，并返回冲突的资源（如果有）
	write_unlock(&resource_lock);	// 释放写锁
	return conflict;	// 返回冲突的资源（如果请求成功则返回NULL）
}

/**
 * request_resource - request and reserve an I/O or memory resource
 * @root: root resource descriptor
 * @new: resource descriptor desired by caller
 *
 * Returns 0 for success, negative error code on error.
 */
/**
 * request_resource - 请求并保留一个I/O或内存资源
 * @root: 根资源描述符
 * @new: 调用者所需的资源描述符
 *
 * 返回0表示成功，错误时返回负的错误代码。
 */
int request_resource(struct resource *root, struct resource *new)
{
	struct resource *conflict;

	// 调用request_resource_conflict来尝试请求资源，该函数返回冲突的资源
	conflict = request_resource_conflict(root, new);
	// 如果存在冲突资源，则返回-EBUSY，表示资源忙碌或已被占用
	return conflict ? -EBUSY : 0;
}

EXPORT_SYMBOL(request_resource);

/**
 * release_resource - release a previously reserved resource
 * @old: resource pointer
 */
/**
 * release_resource - 释放之前保留的资源
 * @old: 资源指针
 *
 * 返回0表示成功，返回-EINVAL表示失败。
 */
int release_resource(struct resource *old)
{
	int retval;

	write_lock(&resource_lock);	// 获取资源锁进行写操作
	retval = __release_resource(old);	// 调用__release_resource尝试释放资源
	write_unlock(&resource_lock);	// 释放资源锁
	return retval;	// 返回操作结果
}

EXPORT_SYMBOL(release_resource);

#if !defined(CONFIG_ARCH_HAS_WALK_MEMORY)
/*
 * Finds the lowest memory reosurce exists within [res->start.res->end)
 * the caller must specify res->start, res->end, res->flags and "name".
 * If found, returns 0, res is overwritten, if not found, returns -1.
 */
/*
 * 寻找给定范围[res->start, res->end)内最低的内存资源。
 * 调用者必须指定 res->start, res->end, res->flags 和 "name"。
 * 如果找到，返回0，并覆盖res；如果未找到，返回-1。
 */
static int find_next_system_ram(struct resource *res, char *name)
{
	resource_size_t start, end;
	struct resource *p;

	BUG_ON(!res);	// 确保res不为空，否则触发内核bug

	start = res->start;	// 开始地址
	end = res->end;			// 结束地址
	// 确保起始地址小于结束地址，否则触发内核bug
	BUG_ON(start >= end);

	read_lock(&resource_lock);	// 获取读锁，以保护资源结构
	// 遍历内存资源
	for (p = iomem_resource.child; p ; p = p->sibling) {
		/* system ram is just marked as IORESOURCE_MEM */
		if (p->flags != res->flags)	// 过滤非指定类型的资源
			continue;
		if (name && strcmp(p->name, name))	// 名称匹配检查
			continue;
		if (p->start > end) {	// 当前资源起始地址超出搜索范围
			p = NULL;
			break;
		}
		if ((p->end >= start) && (p->start < end))	// 找到交叉部分
			break;
	}
	read_unlock(&resource_lock);	// 释放读锁
	if (!p)	// 如果没找到匹配资源
		return -1;
	/* copy data */
	if (res->start < p->start)	// 更新返回资源的起始地址为实际可用的最低地址
		res->start = p->start;
	if (res->end > p->end)			// 更新返回资源的结束地址为实际可用的最高地址
		res->end = p->end;
	return 0;	// 成功找到资源
}

/*
 * This function calls callback against all memory range of "System RAM"
 * which are marked as IORESOURCE_MEM and IORESOUCE_BUSY.
 * Now, this function is only for "System RAM".
 */
/*
 * 此函数对标记为 IORESOURCE_MEM 和 IORESOURCE_BUSY 的所有“系统 RAM”内存范围
 * 调用回调函数。目前，此函数仅用于“系统 RAM”。
 */
int walk_system_ram_range(unsigned long start_pfn, unsigned long nr_pages,
		void *arg, int (*func)(unsigned long, unsigned long, void *))
{
	struct resource res;
	unsigned long pfn, end_pfn;
	u64 orig_end;
	int ret = -1;

	// 将页框号转换为物理地址，设置资源的起始和结束地址
	res.start = (u64) start_pfn << PAGE_SHIFT;
	res.end = ((u64)(start_pfn + nr_pages) << PAGE_SHIFT) - 1;
	// 设置资源标志为内存和正在使用
	res.flags = IORESOURCE_MEM | IORESOURCE_BUSY;
	// 保存原始的结束地址
	orig_end = res.end;
	// 遍历系统 RAM，直到结束地址小于开始地址或找不到下一个系统 RAM
	while ((res.start < res.end) &&
		(find_next_system_ram(&res, "System RAM") >= 0)) {
		// 计算当前内存范围的起始和结束页框号
		pfn = (res.start + PAGE_SIZE - 1) >> PAGE_SHIFT;
		end_pfn = (res.end + 1) >> PAGE_SHIFT;
		// 如果有有效的页框号范围，调用回调函数
		if (end_pfn > pfn)
			ret = (*func)(pfn, end_pfn - pfn, arg);
		// 如果回调函数返回非0值，终止遍历
		if (ret)
			break;
		// 更新资源的开始地址为当前结束地址的下一个地址，重置结束地址为原始结束地址
		res.start = res.end + 1;
		res.end = orig_end;
	}
	return ret;
}

#endif

static int __is_ram(unsigned long pfn, unsigned long nr_pages, void *arg)
{
	return 1;	// 总是返回1，表示给定的页面帧号(PFN)是RAM
}
/*
 * This generic page_is_ram() returns true if specified address is
 * registered as "System RAM" in iomem_resource list.
 */
/*
 * 这个通用的 page_is_ram() 函数返回 true，如果指定的地址被登记为
 * iomem_resource 列表中的“System RAM”。
 */
int __weak page_is_ram(unsigned long pfn)
{
	// 调用 walk_system_ram_range 检查从 pfn 开始的单个页面是否属于System RAM
	return walk_system_ram_range(pfn, 1, NULL, __is_ram) == 1;
}

/*
 * Find empty slot in the resource tree given range and alignment.
 */
/*
 * 在给定范围和对齐要求的资源树中寻找空闲槽。
 */
/*
 * 负责在资源树中找到一个满足特定大小、对齐要求和范围限制的空闲区域。
 * 该函数广泛用于系统资源的管理，如内存和I/O空间的分配。通过遍历资源树，
 * 尝试找到一个合适的位置放置新的资源描述符。如果成功，函数初始化新资源
 * 的起始和结束地址并返回0。如果没有找到合适的空间，则返回错误代码。
 */
static int find_resource(struct resource *root, struct resource *new,
			 resource_size_t size, resource_size_t min,
			 resource_size_t max, resource_size_t align,
			 resource_size_t (*alignf)(void *,
						   const struct resource *,
						   resource_size_t,
						   resource_size_t),
			 void *alignf_data)
{
	struct resource *this = root->child;	// 开始从根资源的子节点遍历
	struct resource tmp = *new;	// 创建一个临时资源用于调整和检测

	tmp.start = root->start;	// 初始化临时资源的起始地址为根资源的起始地址
	/*
	 * Skip past an allocated resource that starts at 0, since the assignment
	 * of this->start - 1 to tmp->end below would cause an underflow.
	 */
	/*
	 * 如果存在一个起始地址为0的已分配资源，则跳过，因为下面将 this->start - 1
	 * 赋值给 tmp.end 会导致下溢。
	 */
	if (this && this->start == 0) {
		tmp.start = this->end + 1;	// 调整临时资源的起始地址为该资源的结束地址加1
		this = this->sibling;				// 移动到下一个兄弟资源
	}
	for(;;) {
		if (this)
			tmp.end = this->start - 1;	// 设定临时资源的结束地址为当前资源的起始地址减1
		else
			tmp.end = root->end;	// 如果没有更多的子资源，将临时资源的结束地址设为根资源的结束地址
		if (tmp.start < min)
			tmp.start = min;	// 调整临时资源的起始地址不小于最小允许地址
		if (tmp.end > max)
			tmp.end = max;		// 调整临时资源的结束地址不大于最大允许地址
		tmp.start = ALIGN(tmp.start, align);	// 根据对齐参数调整起始地址
		if (alignf)
			tmp.start = alignf(alignf_data, &tmp, size, align);	// 使用自定义对齐函数进一步调整起始地址
		if (tmp.start < tmp.end && tmp.end - tmp.start >= size - 1) {
			new->start = tmp.start;	// 设置新资源的起始地址
			new->end = tmp.start + size - 1;	// 设置新资源的结束地址
			return 0;	// 成功找到符合条件的空间，返回0
		}
		if (!this)
			break;	// 如果没有更多的子资源，退出循环
		tmp.start = this->end + 1;	// 将临时资源的起始地址设置为当前资源的结束地址加1
		this = this->sibling;				// 移动到下一个兄弟资源
	}
	return -EBUSY;	// 找不到符合条件的资源，返回忙状态错误
}

/**
 * allocate_resource - allocate empty slot in the resource tree given range & alignment
 * @root: root resource descriptor
 * @new: resource descriptor desired by caller
 * @size: requested resource region size
 * @min: minimum size to allocate
 * @max: maximum size to allocate
 * @align: alignment requested, in bytes
 * @alignf: alignment function, optional, called if not NULL
 * @alignf_data: arbitrary data to pass to the @alignf function
 */
/**
 * allocate_resource - 在给定范围和对齐条件下，在资源树中分配一个空闲槽
 * @root: 根资源描述符
 * @new: 调用者需要的资源描述符
 * @size: 请求的资源区域大小
 * @min: 可分配的最小尺寸
 * @max: 可分配的最大尺寸
 * @align: 请求的对齐，以字节为单位
 * @alignf: 对齐函数，可选，如果非NULL则调用
 * @alignf_data: 传递给@alignf函数的任意数据
 */
int allocate_resource(struct resource *root, struct resource *new,
		      resource_size_t size, resource_size_t min,
		      resource_size_t max, resource_size_t align,
		      resource_size_t (*alignf)(void *,
						const struct resource *,
						resource_size_t,
						resource_size_t),
		      void *alignf_data)
{
	int err;	// 用于存储错误状态或结果的变量

	write_lock(&resource_lock);	// 获取写锁，保护资源树的操作是原子的
	// 尝试在资源树中找到一个合适的空闲槽
	err = find_resource(root, new, size, min, max, align, alignf, alignf_data);
	if (err >= 0 && __request_resource(root, new))	// 如果找到了空间并尝试请求这块资源
		err = -EBUSY;	// 如果资源请求失败，设置错误为忙碌
	write_unlock(&resource_lock);	// 释放写锁
	return err;	// 返回操作结果，成功为0或者错误代码
}

EXPORT_SYMBOL(allocate_resource);

/*
 * Insert a resource into the resource tree. If successful, return NULL,
 * otherwise return the conflicting resource (compare to __request_resource())
 */
/*
 * 将一个资源插入到资源树中。如果成功，返回NULL，
 * 否则返回冲突的资源（与__request_resource()函数进行对比）
 */
/*
 * 在资源管理系统中插入新资源的过程。它首先尝试将新资源插入指定的父资源下，如果有冲突，会尝试处理冲突或找
 * 到合适的位置进行插入。如果插入成功，函数返回NULL。如果存在无法解决的冲突，例如部分重叠，函数返回冲突的资源。
 */
static struct resource * __insert_resource(struct resource *parent, struct resource *new)
{
	struct resource *first, *next;

	for (;; parent = first) {
		first = __request_resource(parent, new);	// 请求将新资源加入到父资源下
		// 到此已经满足first->start <= new->end;tmp->end >= new->start;

		if (!first)	// 如果没有冲突，返回NULL
			return first;

		if (first == parent)	// 如果冲突资源是父资源本身，返回父资源
			return first;

		// 检查是否冲突资源不完全包含新资源，即存在任何端点不覆盖的情况
		if ((first->start > new->start) || (first->end < new->end))
			// first 在至少一端没有覆盖 new，需要进一步处理
			break;
		// 如果冲突资源与新资源完全相同，处理结束
		if ((first->start == new->start) && (first->end == new->end))
			break;
	}

	for (next = first; ; next = next->sibling) {
		/* Partial overlap? Bad, and unfixable */
		// 检查部分重叠，这是不好的，也无法修复
		if (next->start < new->start || next->end > new->end)	// 存在部分重叠，返回冲突资源
			return next;	// 返回冲突资源
		if (!next->sibling)	// 如果没有兄弟节点了，结束循环
			break;
		// 如果下一个兄弟的起始位置在新资源结束之后，结束循环
		if (next->sibling->start > new->end)
			break;
	}

	// 设置新资源的关系链
	new->parent = parent;
	new->sibling = next->sibling;
	new->child = first;

	// 更新兄弟节点的链接
	next->sibling = NULL;
	for (next = first; next; next = next->sibling)
		next->parent = new;	// 将冲突资源的子资源的父节点更新为新资源

	// 如果父资源的子资源是第一个冲突的资源，直接将新资源设置为父资源的子资源
	if (parent->child == first) {
		parent->child = new;
	} else {
		next = parent->child;
		while (next->sibling != first)	// 查找第一个冲突资源的前一个兄弟资源
			next = next->sibling;
		next->sibling = new;	// 将新资源插入到这个位置
	}
	return NULL;	// 成功插入，返回NULL
}

/**
 * insert_resource_conflict - Inserts resource in the resource tree
 * @parent: parent of the new resource
 * @new: new resource to insert
 *
 * Returns 0 on success, conflict resource if the resource can't be inserted.
 *
 * This function is equivalent to request_resource_conflict when no conflict
 * happens. If a conflict happens, and the conflicting resources
 * entirely fit within the range of the new resource, then the new
 * resource is inserted and the conflicting resources become children of
 * the new resource.
 */
/**
 * insert_resource_conflict - 将资源插入资源树
 * @parent: 新资源的父资源
 * @new: 要插入的新资源
 *
 * 返回值：成功返回0，如果资源无法插入则返回冲突资源。
 *
 * 当没有冲突发生时，此函数等同于 request_resource_conflict。
 * 如果发生冲突，并且冲突资源完全位于新资源的范围内，则新资源被插入，
 * 冲突资源成为新资源的子资源。
 */
struct resource *insert_resource_conflict(struct resource *parent, struct resource *new)
{
	struct resource *conflict;

	write_lock(&resource_lock);	// 获取写锁，保护资源树的修改过程
	conflict = __insert_resource(parent, new);	// 尝试插入资源，并处理可能的冲突
	write_unlock(&resource_lock);	// 释放写锁
	return conflict;	// 返回冲突资源，如果成功插入则为NULL
}

/**
 * insert_resource - Inserts a resource in the resource tree
 * @parent: parent of the new resource
 * @new: new resource to insert
 *
 * Returns 0 on success, -EBUSY if the resource can't be inserted.
 */
/**
 * insert_resource - 在资源树中插入一个资源
 * @parent: 新资源的父资源
 * @new: 要插入的新资源
 *
 * 返回值：成功时返回0，如果资源无法插入返回-EBUSY。
 */
int insert_resource(struct resource *parent, struct resource *new)
{
	struct resource *conflict;

	// 调用 insert_resource_conflict 尝试将新资源插入到资源树中
	conflict = insert_resource_conflict(parent, new);	// 如果存在冲突资源，则返回-EBUSY，表示资源插入失败
	return conflict ? -EBUSY : 0;
}

/**
 * insert_resource_expand_to_fit - Insert a resource into the resource tree
 * @root: root resource descriptor
 * @new: new resource to insert
 *
 * Insert a resource into the resource tree, possibly expanding it in order
 * to make it encompass any conflicting resources.
 */
/**
 * insert_resource_expand_to_fit - 将资源插入到资源树中，并在必要时扩展资源
 * @root: 根资源描述符
 * @new: 要插入的新资源
 *
 * 将一个资源插入到资源树中，如果有必要，扩展这个资源以包含任何冲突的资源。
 */
void insert_resource_expand_to_fit(struct resource *root, struct resource *new)
{
	if (new->parent)	// 如果新资源已有父资源，直接返回，不进行操作
		return;

	// 锁定资源树进行修改
	write_lock(&resource_lock);
	for (;;) {
		struct resource *conflict;

		// 尝试将新资源插入到资源树中
		conflict = __insert_resource(root, new);
		// 如果没有冲突或冲突资源是根资源，则退出循环
		if (!conflict)
			break;
		if (conflict == root)
			break;

		/* Ok, expand resource to cover the conflict, then try again .. */
		// 如果存在冲突资源，调整新资源的开始和结束位置以包括冲突资源
		if (conflict->start < new->start)
			new->start = conflict->start;
		if (conflict->end > new->end)
			new->end = conflict->end;

		// 打印扩展资源的信息，显示由于哪个资源的冲突而进行了扩展
		printk("Expanded resource %s due to conflict with %s\n", new->name, conflict->name);
	}
	write_unlock(&resource_lock);	// 解锁资源树
}

/**
 * adjust_resource - modify a resource's start and size
 * @res: resource to modify
 * @start: new start value
 * @size: new size
 *
 * Given an existing resource, change its start and size to match the
 * arguments.  Returns 0 on success, -EBUSY if it can't fit.
 * Existing children of the resource are assumed to be immutable.
 */
/**
 * adjust_resource - 修改资源的起始地址和大小
 * @res: 要修改的资源
 * @start: 新的起始值
 * @size: 新的大小
 *
 * 给定一个已存在的资源，修改其起始地址和大小以匹配传入的参数。
 * 如果成功返回0，如果空间不足则返回-EBUSY。
 * 假设该资源的子资源是不可变的。
 */
int adjust_resource(struct resource *res, resource_size_t start, resource_size_t size)
{
	struct resource *tmp, *parent = res->parent;  // 定义指向资源的父资源和临时资源的指针
	resource_size_t end = start + size - 1;  // 计算新资源的结束地址
	int result = -EBUSY;  // 默认返回值为-EBUSY

	write_lock(&resource_lock);  // 加写锁保护资源树的修改

	// 检查新的起始和结束地址是否超出父节点的范围
	if ((start < parent->start) || (end > parent->end))	// 如果新的范围超出了父资源的范围
		goto out;	// 跳到出口代码

	// 遍历子资源，检查是否有子资源超出新范围
	for (tmp = res->child; tmp; tmp = tmp->sibling) {	// 遍历该资源的所有子资源
		if ((tmp->start < start) || (tmp->end > end))	// 如果子资源不在新范围内
			goto out;	// 跳到出口代码
	}

	// 检查兄弟节点是否与新的结束地址冲突
	if (res->sibling && (res->sibling->start <= end))	// 如果该资源的兄弟节点在新的结束地址之内
		goto out;	// 冲突，跳转到out标签进行解锁并返回

	// 从父节点的第一个子节点开始，检查是否有节点与新范围冲突
	tmp = parent->child;	// 从父资源的第一个子资源开始检查
	if (tmp != res) {
		while (tmp->sibling != res)
			tmp = tmp->sibling;
		if (start <= tmp->end)
			goto out;	// 冲突，跳转到out标签进行解锁并返回
	}

	// 没有冲突，更新资源的起始和结束地址
	res->start = start;
	res->end = end;
	result = 0;	// 操作成功，设置返回值为0

 out:
	write_unlock(&resource_lock);	// 解锁
	return result;	// 返回结果
}

/**
 * __reserve_region_with_split - 预留指定区域，必要时进行拆分
 * @root: 根资源描述符
 * @start: 要预留区域的起始地址
 * @end: 要预留区域的结束地址
 * @name: 资源名称
 *
 * 该函数尝试在资源树中预留一个指定的区域，如果该区域与现有的资源冲突，
 * 则将其拆分并分别预留不冲突的子区域。
 */
static void __init __reserve_region_with_split(struct resource *root,
		resource_size_t start, resource_size_t end,
		const char *name)
{
	struct resource *parent = root; // 根资源描述符
	struct resource *conflict; // 用于存储冲突资源的指针
	struct resource *res = kzalloc(sizeof(*res), GFP_ATOMIC); // 为新资源分配内存

	if (!res)
		return; // 如果内存分配失败，直接返回

	res->name = name; // 设置资源名称
	res->start = start; // 设置资源起始地址
	res->end = end; // 设置资源结束地址
	res->flags = IORESOURCE_BUSY; // 设置资源标志为忙

	conflict = __request_resource(parent, res); // 尝试请求资源
	if (!conflict)
		return; // 如果请求成功，直接返回

	/* failed, split and try again */
	/* 请求失败，进行拆分并再次尝试 */
	kfree(res);	// 释放分配的内存

	/* conflict covered whole area */
	/* 如果冲突资源覆盖了整个区域 */
	if (conflict->start <= start && conflict->end >= end)
		return;	// 无法拆分，直接返回

	// 如果冲突资源的起始地址在要预留区域的起始地址之后
	if (conflict->start > start)
		// 递归调用预留前半部分区域
		__reserve_region_with_split(root, start, conflict->start-1, name);
	// 如果冲突资源的结束地址在要预留区域的结束地址之前
	if (conflict->end < end)
		// 递归调用预留后半部分区域
		__reserve_region_with_split(root, conflict->end+1, end, name);
}

/**
 * reserve_region_with_split - 在资源树中预留指定区域，必要时进行拆分
 * @root: 根资源描述符
 * @start: 要预留区域的起始地址
 * @end: 要预留区域的结束地址
 * @name: 资源的名称
 *
 * 该函数在资源树中预留指定的区域。如果此区域与现有资源存在冲突，
 * 则自动进行拆分，以确保所有非冲突区域都被预留。
 * 调用该函数前会获取写锁定，确保操作的原子性和线程安全。
 */
void __init reserve_region_with_split(struct resource *root,
		resource_size_t start, resource_size_t end,
		const char *name)
{
	write_lock(&resource_lock); // 获取资源的写锁定，确保资源分配过程的线程安全
	__reserve_region_with_split(root, start, end, name); // 调用内部函数执行实际的资源预留和拆分操作
	write_unlock(&resource_lock); // 释放写锁定
}

EXPORT_SYMBOL(adjust_resource);

/**
 * resource_alignment - calculate resource's alignment
 * @res: resource pointer
 *
 * Returns alignment on success, 0 (invalid alignment) on failure.
 */
/**
 * resource_alignment - 计算资源的对齐大小
 * @res: 资源指针
 *
 * 根据资源的flags中对齐相关的位来计算其对齐大小。
 * 返回对齐大小（成功时），或者返回0表示无效对齐（失败时）。
 */
resource_size_t resource_alignment(struct resource *res)
{
	// 根据资源的flags字段中的对齐位进行选择
	switch (res->flags & (IORESOURCE_SIZEALIGN | IORESOURCE_STARTALIGN)) {
	case IORESOURCE_SIZEALIGN:	// 如果设置了按大小对齐
		return resource_size(res);	// 返回资源的大小作为对齐单位
	case IORESOURCE_STARTALIGN:	// 如果设置了按起始地址对齐
		return res->start;	// 返回资源的起始地址作为对齐单位
	default:	// 如果没有设置特定的对齐方式
		return 0;	// 返回0，表示对齐方式无效或未设置
	}
}

/*
 * This is compatibility stuff for IO resources.
 *
 * Note how this, unlike the above, knows about
 * the IO flag meanings (busy etc).
 *
 * request_region creates a new busy region.
 *
 * check_region returns non-zero if the area is already busy.
 *
 * release_region releases a matching busy region.
 */
/*
 * 这是兼容 IO 资源的代码。
 *
 * 注意，这与上面的代码不同，它知道 IO 标志的含义（忙碌等）。
 *
 * request_region 创建一个新的忙碌区域。
 *
 * check_region 如果区域已经被占用则返回非零值。
 *
 * release_region 释放一个匹配的忙碌区域。
 */

/**
 * __request_region - create a new busy resource region
 * @parent: parent resource descriptor
 * @start: resource start address
 * @n: resource region size
 * @name: reserving caller's ID string
 * @flags: IO resource flags
 */
/**
 * __request_region - 创建一个新的忙碌资源区域
 * @parent: 父资源描述符
 * @start: 资源起始地址
 * @n: 资源区域大小
 * @name: 请求者的标识字符串
 * @flags: IO资源标志
 *
 * 创建并返回一个新的资源对象，它表示一个被标记为忙碌的资源区域。
 * 如果无法分配资源，返回NULL。
 */
struct resource * __request_region(struct resource *parent,
				   resource_size_t start, resource_size_t n,
				   const char *name, int flags)
{
	struct resource *res = kzalloc(sizeof(*res), GFP_KERNEL);  // 为资源对象分配内存

	if (!res)  // 如果内存分配失败
		return NULL;  // 返回NULL

	res->name = name;  // 设置资源的名称
	res->start = start;  // 设置资源的起始地址
	res->end = start + n - 1;  // 计算并设置资源的结束地址
	res->flags = IORESOURCE_BUSY;  // 设置资源状态为忙碌
	res->flags |= flags;  // 添加额外的标志

	write_lock(&resource_lock);  // 获取写锁

	for (;;) {
		struct resource *conflict;	// 用于检测冲突的资源

		conflict = __request_resource(parent, res);	// 尝试请求资源
		if (!conflict)	// 如果没有冲突
			break;	// 跳出循环，资源请求成功
		if (conflict != parent) {	// 如果发生冲突且冲突资源不是父资源
			parent = conflict;	// 更新父资源为冲突资源
			if (!(conflict->flags & IORESOURCE_BUSY))	// 如果冲突资源不是忙碌状态
				continue;	 // 继续循环，尝试在更新的父资源下请求资源
		}

		/* Uhhuh, that didn't work out.. */
		/* 如果无法解决冲突 */
		kfree(res);	// 释放已分配的资源对象
		res = NULL;	// 将资源指针设为NULL
		break;		// 跳出循环
	}
	write_unlock(&resource_lock);	// 释放写锁
	return res;	// 返回资源对象（成功）或NULL（失败）
}
EXPORT_SYMBOL(__request_region);

/**
 * __check_region - check if a resource region is busy or free
 * @parent: parent resource descriptor
 * @start: resource start address
 * @n: resource region size
 *
 * Returns 0 if the region is free at the moment it is checked,
 * returns %-EBUSY if the region is busy.
 *
 * NOTE:
 * This function is deprecated because its use is racy.
 * Even if it returns 0, a subsequent call to request_region()
 * may fail because another driver etc. just allocated the region.
 * Do NOT use it.  It will be removed from the kernel.
 */
/**
 * __check_region - 检查资源区域是否忙碌或空闲
 * @parent: 父资源描述符
 * @start: 资源起始地址
 * @n: 资源区域大小
 *
 * 如果在检查时区域空闲，则返回0；
 * 如果区域忙碌，则返回 -EBUSY。
 *
 * 注意：
 * 此函数已被弃用，因为其使用存在竞态问题。
 * 即使返回0，后续调用request_region()可能会失败，
 * 因为可能有其他驱动等刚刚分配了该区域。
 * 请勿使用此函数。它将从内核中移除。
 */
int __check_region(struct resource *parent, resource_size_t start,
			resource_size_t n)
{
	struct resource * res;

	res = __request_region(parent, start, n, "check-region", 0);	// 尝试请求区域，不设置忙碌标志
	if (!res)	// 如果请求失败，说明区域已被占用
		return -EBUSY;	// 返回忙碌

	release_resource(res);	// 释放刚才请求的资源
	kfree(res);	// 释放资源所占用的内存
	return 0;		// 返回空闲
}
EXPORT_SYMBOL(__check_region);

/**
 * __release_region - release a previously reserved resource region
 * @parent: parent resource descriptor
 * @start: resource start address
 * @n: resource region size
 *
 * The described resource region must match a currently busy region.
 */
/**
 * __release_region - 释放之前预留的资源区域
 * @parent: 父资源描述符
 * @start: 资源起始地址
 * @n: 资源区域大小
 *
 * 描述的资源区域必须与当前忙碌的区域匹配。
 */
void __release_region(struct resource *parent, resource_size_t start,
			resource_size_t n)
{
	struct resource **p;  // 指向资源的指针
	resource_size_t end;  // 资源结束地址

	p = &parent->child;  // 从父资源的子资源开始
	end = start + n - 1;  // 计算资源的结束位置

	write_lock(&resource_lock);  // 获取写锁，保护资源树的修改操作

	for (;;) {	// 无限循环，直到找到对应资源或遍历完所有资源
		struct resource *res = *p;	// 获取当前资源

		if (!res)	// 如果资源不存在，跳出循环
			break;
		if (res->start <= start && res->end >= end) {	// 如果找到匹配的资源区域
			if (!(res->flags & IORESOURCE_BUSY)) {			// 如果资源不忙碌，继续检查子资源
				p = &res->child;
				continue;
			}
			if (res->start != start || res->end != end)	// 如果不完全匹配，跳出循环
				break;
			*p = res->sibling;	// 从父资源的子链表中删除该资源
			write_unlock(&resource_lock);	// 释放写锁
			kfree(res);	// 释放资源结构体占用的内存
			return;	// 返回，释放成功
		}
		p = &res->sibling;	// 指向下一个兄弟资源
	}

	write_unlock(&resource_lock);	// 释放写锁

	printk(KERN_WARNING "Trying to free nonexistent resource "
		"<%016llx-%016llx>\n", (unsigned long long)start,
		(unsigned long long)end);	// 打印警告信息
}
EXPORT_SYMBOL(__release_region);

/*
 * Managed region resource
 */
/*
 * 管理区域资源的数据结构
 */
struct region_devres {
	struct resource *parent;  // 父资源
	resource_size_t start;    // 资源起始地址
	resource_size_t n;        // 资源大小
};

/*
 * 设备管理的资源释放函数
 * @dev: 设备指针
 * @res: 要释放的资源
 *
 * 当设备被卸载或资源不再需要时调用，用于释放设备占用的资源。
 */
static void devm_region_release(struct device *dev, void *res)
{
	struct region_devres *this = res;	// 将void指针转换为region_devres指针

	__release_region(this->parent, this->start, this->n);	// 调用内核函数释放资源区域
}

/*
 * 设备资源匹配函数
 * @dev: 设备指针
 * @res: 资源指针
 * @match_data: 匹配数据
 *
 * 用于检查资源是否匹配给定的匹配数据，主要在释放资源时使用。
 * 返回值为1表示匹配成功，0表示不匹配。
 */
static int devm_region_match(struct device *dev, void *res, void *match_data)
{
	struct region_devres *this = res, *match = match_data;	// 转换指针类型

	// 比较父资源、起始地址和资源大小，全部匹配返回1，否则返回0
	return this->parent == match->parent &&
		this->start == match->start && this->n == match->n;
}

/*
 * __devm_request_region - 请求并注册一个设备管理的资源区域
 * @dev: 设备实例
 * @parent: 父资源
 * @start: 资源起始地址
 * @n: 资源区域的大小
 * @name: 请求者的名称，用于标识资源所有者
 *
 * 此函数用于为设备请求并注册一段资源区域。如果请求成功，资源会被标记为忙碌，并且
 * 这段资源将会被添加到设备的资源管理列表中，确保在设备释放时资源也被自动释放。
 * 返回资源指针，如果无法分配资源或资源已被占用，则返回 NULL。
 */
struct resource * __devm_request_region(struct device *dev,
				struct resource *parent, resource_size_t start,
				resource_size_t n, const char *name)
{
	struct region_devres *dr = NULL;	// 设备资源管理结构体指针
	struct resource *res;	// 资源指针

	// 为设备资源管理结构体分配内存
	dr = devres_alloc(devm_region_release, sizeof(struct region_devres),
			  GFP_KERNEL);
	if (!dr)
		return NULL;	// 如果内存分配失败，返回 NULL

	// 初始化设备资源管理结构体
	dr->parent = parent;
	dr->start = start;
	dr->n = n;

	// 请求资源区域
	res = __request_region(parent, start, n, name, 0);
	if (res)
		devres_add(dev, dr);	// 如果资源请求成功，将资源管理结构体添加到设备的资源列表中
	else
		devres_free(dr);	// 如果请求失败，释放之前分配的设备资源管理结构体

	return res;	// 返回资源指针，成功或失败
}
EXPORT_SYMBOL(__devm_request_region);

/*
 * __devm_release_region - 释放由设备管理的资源区域
 * @dev: 设备实例
 * @parent: 父资源
 * @start: 资源起始地址
 * @n: 资源区域的大小
 *
 * 此函数释放之前由 __devm_request_region 函数请求的资源区域，并确保该资源区域从设备的资源列表中移除。
 */
void __devm_release_region(struct device *dev, struct resource *parent,
			   resource_size_t start, resource_size_t n)
{
	struct region_devres match_data = { parent, start, n };	// 创建一个局部变量用于匹配资源

	__release_region(parent, start, n);	// 调用 __release_region 函数释放资源区域
	// 使用 devres_destroy 来释放与设备相关联的资源，并验证资源是否正确释放
	// WARN_ON 宏在其条件为真时打印警告信息
	WARN_ON(devres_destroy(dev, devm_region_release, devm_region_match,
			       &match_data));
}
EXPORT_SYMBOL(__devm_release_region);

/*
 * Called from init/main.c to reserve IO ports.
 */
#define MAXRESERVE 4  // 定义最多可以保留的资源数量
/*
 * reserve_setup - 从 init/main.c 调用以保留 IO 端口。
 * @str: 传入的参数字符串，包含要保留的资源的起始地址和数量。
 *
 * 用于系统启动时通过内核命令行参数保留一组特定的 I/O 资源。
 * 此函数解析由 'reserve=' 传递的字符串，按需保留资源。
 */
static int __init reserve_setup(char *str)
{
	static int reserved;	// 已保留资源的数量
	static struct resource reserve[MAXRESERVE];	// 存储保留资源的静态数组

	for (;;) {	// 无限循环，直到解析完所有输入或达到保留限制
		unsigned int io_start, io_num;	// 存储从字符串中解析的起始地址和数量
		int x = reserved;	// 当前已保留资源的数量

		if (get_option (&str, &io_start) != 2)	// 解析起始地址
			break;	// 如果解析失败或不正确，退出循环
		if (get_option (&str, &io_num)   == 0)	// 解析数量
			break;	// 如果解析失败，退出循环
		if (x < MAXRESERVE) {	// 检查是否还有保留空间
			struct resource *res = reserve + x;	// 指向当前要操作的资源对象
			res->name = "reserved";	// 设置资源名称
			res->start = io_start;	// 设置资源的起始地址
			res->end = io_start + io_num - 1;	// 设置资源的结束地址
			res->flags = IORESOURCE_BUSY;	// 标记为已占用
			res->child = NULL;	// 无子资源
			if (request_resource(res->start >= 0x10000 ? &iomem_resource : &ioport_resource, res) == 0)
				reserved = x+1;	// 如果资源请求成功，增加保留计数
		}
	}
	return 1;	// 返回1表示处理成功
}

__setup("reserve=", reserve_setup);

/*
 * Check if the requested addr and size spans more than any slot in the
 * iomem resource tree.
 */
/*
 * iomem_map_sanity_check - 检查请求的地址和大小是否跨越 iomem 资源树中的任何槽位
 * @addr: 请求的起始地址
 * @size: 请求的大小
 *
 * 返回0表示没有冲突，返回-1表示发现冲突。
 *
 * 此函数用于确保给定的地址和大小没有跨越系统中任何已经存在的资源。主要用于资源映射的前期检查。
 */
int iomem_map_sanity_check(resource_size_t addr, unsigned long size)
{
	struct resource *p = &iomem_resource;  // iomem_resource 是系统内存资源的根
	int err = 0;  // 错误标志，默认为0，表示无错误
	loff_t l;  // 用于存储位置偏移

	read_lock(&resource_lock);  // 获取读锁

	for (p = p->child; p ; p = r_next(NULL, p, &l)) {	// 遍历资源树的子资源
		/*
		 * We can probably skip the resources without
		 * IORESOURCE_IO attribute?
		 */
		/*
		 * 我们可能可以跳过没有 IORESOURCE_IO 属性的资源？
		 */
		if (p->start >= addr + size)	// 如果资源开始地址在请求区域之外，跳过
			continue;
		if (p->end < addr)	// 如果资源结束地址在请求区域之前，跳过
			continue;
		if (PFN_DOWN(p->start) <= PFN_DOWN(addr) &&	// PFN_DOWN 宏将地址转换为页帧号
		    PFN_DOWN(p->end) >= PFN_DOWN(addr + size - 1))	// 检查页帧号是否完全覆盖请求区域
			continue;
		/*
		 * if a resource is "BUSY", it's not a hardware resource
		 * but a driver mapping of such a resource; we don't want
		 * to warn for those; some drivers legitimately map only
		 * partial hardware resources. (example: vesafb)
		 */
		/*
		 * 如果一个资源标记为 "BUSY"，它不是硬件资源
		 * 而是该资源的驱动程序映射；我们不想对这些发出警告；
		 * 一些驱动程序合法地只映射部分硬件资源。(例如: vesafb)
		 */
		if (p->flags & IORESOURCE_BUSY)	// 检查资源是否被占用
			continue;

		// 打印警告信息，表明发现冲突
		printk(KERN_WARNING "resource map sanity check conflict: "
		       "0x%llx 0x%llx 0x%llx 0x%llx %s\n",
		       (unsigned long long)addr,
		       (unsigned long long)(addr + size - 1),
		       (unsigned long long)p->start,
		       (unsigned long long)p->end,
		       p->name);
		err = -1;	// 设置错误标志
		break;
	}
	read_unlock(&resource_lock);	// 释放读锁

	return err;	// 返回错误状态
}

#ifdef CONFIG_STRICT_DEVMEM
static int strict_iomem_checks = 1;	// 如果定义了 CONFIG_STRICT_DEVMEM，则启用严格的内存检查
#else
static int strict_iomem_checks;			// 否则，默认不启用严格的内存检查
#endif

/*
 * check if an address is reserved in the iomem resource tree
 * returns 1 if reserved, 0 if not reserved.
 */
/*
 * iomem_is_exclusive - 检查一个地址是否在iomem资源树中被保留
 * @addr: 要检查的地址
 *
 * 返回 1 如果地址被保留，0 如果没有被保留。
 *
 * 此函数用于检查一个给定的地址是否在iomem资源树中被标记为保留并且是独占的。
 */
int iomem_is_exclusive(u64 addr)
{
	struct resource *p = &iomem_resource;  // 指向系统的iomem资源树的根资源
	int err = 0;  // 错误标志，初始设置为0，表示没有错误（即地址不是独占的）
	loff_t l;  // 用于存储位置偏移
	int size = PAGE_SIZE;  // 设置检查大小为一页内存

	if (!strict_iomem_checks)  // 如果没有启用严格的内存检查
		return 0;  // 直接返回0，表示地址不是独占的

	addr = addr & PAGE_MASK;  // 将地址对齐到页面边界

	read_lock(&resource_lock);  // 获取读锁
	for (p = p->child; p ; p = r_next(NULL, p, &l)) {	// 遍历iomem资源的子资源
		/*
		 * We can probably skip the resources without
		 * IORESOURCE_IO attribute?
		 */
		/*
		 * 我们可能可以跳过没有 IORESOURCE_IO 属性的资源？
		 */
		if (p->start >= addr + size)	// 如果资源的起始地址在检查的地址之后，停止循环
			break;
		if (p->end < addr)	// 如果资源的结束地址在检查的地址之前，跳过当前资源
			continue;
		if (p->flags & IORESOURCE_BUSY &&	// 如果资源被标记为忙碌
		     p->flags & IORESOURCE_EXCLUSIVE) {	// 并且资源是独占的
			err = 1;	// 设置错误标志为1，表示地址是独占的
			break;		// 停止循环
		}
	}
	read_unlock(&resource_lock);	// 释放读锁

	return err;	// 返回错误标志
}

/*
 * __init - 指定这是一个初始化函数，仅在系统启动时调用一次
 * strict_iomem - 用于处理启动参数来设置内存访问的严格检查级别
 * @str: 传递给函数的字符串参数，包含 "iomem=" 后的值
 *
 * 此函数根据内核启动参数调整严格的内存访问检查设置。
 */
static int __init strict_iomem(char *str)
{
	if (strstr(str, "relaxed"))	// 如果字符串中包含 "relaxed"
		strict_iomem_checks = 0;	// 设置严格检查为禁用（放宽内存访问检查）
	if (strstr(str, "strict"))	// 如果字符串中包含 "strict"
		strict_iomem_checks = 1;	// 启用严格的内存访问检查
	return 1;	// 总是返回1，表示处理成功
}

/*
 * 这段代码允许内核启动时通过 "iomem=" 参数来设置严格内存访问的检查级别。
 * 例如，通过添加 "iomem=strict" 或 "iomem=relaxed" 到内核的启动参数中，
 * 可以控制系统在处理内存I/O时的安全检查行为。
 */
__setup("iomem=", strict_iomem);	// 注册 "iomem=" 内核启动参数，与 strict_iomem 函数关联
