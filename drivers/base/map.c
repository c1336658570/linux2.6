/*
 *  linux/drivers/base/map.c
 *
 * (C) Copyright Al Viro 2002,2003
 *	Released under GPL v2.
 *
 * NOTE: data structure needs to be changed.  It works, but for large dev_t
 * it will be too slow.  It is isolated, though, so these changes will be
 * local to that file.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/kdev_t.h>
#include <linux/kobject.h>
#include <linux/kobj_map.h>

// 内核对象映射的核心结构。该结构用于管理设备编号到内核对象的映射。
// kobj_map结构体是用来管理设备号及其对应的设备的。
struct kobj_map {
	struct probe {
		// 指向下一个probe结构的指针，形成链表
		struct probe *next;
		// 设备类型和编号
		dev_t dev;
		// 此探测器负责的设备编号范围
		unsigned long range;	 /* 设备号的范围 */
		// 拥有此探测器的模块
		struct module *owner;
		// 函数指针，用于探测设备
		kobj_probe_t *get;
		// 函数指针，用于锁定设备
		int (*lock)(dev_t, void *);
		// 传递给lock函数的额外数据
		void *data;	/* 指向struct cdev对象 */
		// probe数组，可以存储255个设备探测器
	} *probes[255];
	// 互斥锁，用于保护这个结构的修改
	struct mutex *lock;
};

// 定义 kobj_map 函数，用于将设备号映射到 kobject probe。
int kobj_map(struct kobj_map *domain, dev_t dev, unsigned long range,
	     struct module *module, kobj_probe_t *probe,
	     int (*lock)(dev_t, void *), void *data)
{
	// 计算需要映射的主设备号的数量
	unsigned n = MAJOR(dev + range - 1) - MAJOR(dev) + 1;
	// 获取起始设备号的主设备号
	unsigned index = MAJOR(dev);
	unsigned i;
	struct probe *p;
	
	// 如果计算出的主设备号数量大于255，将其限制为255
	if (n > 255)
		n = 255;

	// 分配内存以存放 n 个 probe 结构
	p = kmalloc(sizeof(struct probe) * n, GFP_KERNEL);

	// 如果内存分配失败，返回 -ENOMEM 错误码
	if (p == NULL)
		return -ENOMEM;

	// 初始化分配的每一个 probe 结构
	for (i = 0; i < n; i++, p++) {
		p->owner = module;	// 设置模块所有者
		p->get = probe;			// 设置探测函数
		p->lock = lock;			// 设置锁函数
		p->dev = dev;				// 设置设备号
		p->range = range;		// 设置范围
		p->data = data;			// 设置相关数据
	}
	// 加锁以保护 domain 结构
	mutex_lock(domain->lock);
	// 将初始化的 probe 结构链表插入到 domain 的 probes 数组中
	for (i = 0, p -= n; i < n; i++, p++, index++) {
		// 根据主设备号找到 probes 数组中的位置
		struct probe **s = &domain->probes[index % 255];
		// 确保按 range 大小排序，将当前 probe 插入到正确的位置
		while (*s && (*s)->range < range)
			s = &(*s)->next;
		// 插入当前 probe 到链表中
		p->next = *s;
		*s = p;
	}
	// 解锁
	mutex_unlock(domain->lock);
	return 0;
}

// 定义函数 kobj_unmap，用于从一个 kobj_map 域中移除一个设备号映射。
void kobj_unmap(struct kobj_map *domain, dev_t dev, unsigned long range)
{
	// 计算要解除映射的主设备号数量
	unsigned n = MAJOR(dev + range - 1) - MAJOR(dev) + 1;
	// 获取起始设备号的主设备号
	unsigned index = MAJOR(dev);
	unsigned i;
	struct probe *found = NULL;

	// 如果主设备号数量大于255，则将其限制为255
	if (n > 255)
		n = 255;

	// 加锁，以保护对 domain 的修改
	mutex_lock(domain->lock);
	// 遍历要解除映射的设备号
	for (i = 0; i < n; i++, index++) {
		struct probe **s;
		// 遍历每个设备号对应的 probe 链表
		for (s = &domain->probes[index % 255]; *s; s = &(*s)->next) {
			struct probe *p = *s;
			// 如果找到匹配的设备号和范围
			if (p->dev == dev && p->range == range) {
				// 从链表中移除该 probe
				*s = p->next;
				// 记录找到的第一个符合条件的 probe
				if (!found)
					found = p;
				break;
			}
		}
	}
	// 解锁
	mutex_unlock(domain->lock);
	// 释放找到的 probe 结构
	kfree(found);
}

// 定义函数 kobj_lookup 用于查找和返回与设备号对应的 kobject。
struct kobject *kobj_lookup(struct kobj_map *domain, dev_t dev, int *index)
{
	struct kobject *kobj;	// 声明指向 kobject 的指针。
	struct probe *p;	// 声明探针结构指针。
	// 初始化最优匹配长度为最大值。
	unsigned long best = ~0UL;

retry:
	mutex_lock(domain->lock);	// 上锁保护域的互斥体。
	// 遍历指定设备号主设备号索引下的探针链表。
	for (p = domain->probes[MAJOR(dev) % 255]; p; p = p->next) {
		// 声明探针函数指针。
		struct kobject *(*probe)(dev_t, int *, void *);
		struct module *owner;	// 声明模块所有者。
		void *data;	// 声明关联数据指针。
		
		// 如果设备号不在探针指定的范围内，跳过此探针。
		if (p->dev > dev || p->dev + p->range - 1 < dev)
			continue;
		// 如果此探针的范围不优于之前的最优范围，则终止循环。
		if (p->range - 1 >= best)
			break;
		// 尝试获取探针所有模块的使用权，如果失败则跳过此探针。
		if (!try_module_get(p->owner))
			continue;
		// 设置临时变量。
		owner = p->owner;
		data = p->data;
		probe = p->get;
		best = p->range - 1;
		// 设置索引值，即设备号与探针设备号的偏移。
		*index = dev - p->dev;
		// 如果定义了探针锁函数，并尝试锁定失败，则释放模块并继续循环。
		if (p->lock && p->lock(dev, data) < 0) {
			module_put(owner);
			continue;
		}
		// 解锁并调用探针函数。
		mutex_unlock(domain->lock);
		kobj = probe(dev, index, data);
		/* Currently ->owner protects _only_ ->probe() itself. */
		// 释放模块。
		module_put(owner);
		// 如果探针函数返回 kobject，直接返回此 kobject。
		if (kobj)
			return kobj;
		// 如果探针函数未返回 kobject，重试整个流程。
		goto retry;
	}
	// 解锁并返回 NULL，表示没有找到匹配的 kobject。
	mutex_unlock(domain->lock);
	return NULL;
}

/**
 * 初始化kobj_map结构体。kobj_map结构体用于内核对象（kobject）的映射管理。
 * 该函数接收两个参数：一个探测函数指针和一个互斥锁。函数首先分配内存给kobj_map和
 * probe结构体。如果任一内存分配失败，则释放已分配的内存并返回NULL。成功分配后，
 * 它初始化probe结构体并设置所有的probe指针数组指向这个基本探测器。最后，
 * 它将传入的锁赋值给kobj_map结构体并返回这个初始化的结构体指针。这样，
 * 这个结构体就可以用来管理不同的内核对象映射了。
 */
struct kobj_map *kobj_map_init(kobj_probe_t *base_probe, struct mutex *lock)
{
	// 为kobj_map结构体分配内存
	struct kobj_map *p = kmalloc(sizeof(struct kobj_map), GFP_KERNEL);
	// 为probe结构体分配内存
	struct probe *base = kzalloc(sizeof(*base), GFP_KERNEL);
	int i;

	// 检查分配是否成功
	if ((p == NULL) || (base == NULL)) {
		kfree(p);	// 释放p的内存，如果p不为NULL
		kfree(base);	// 释放base的内存，如果base不为NULL
		return NULL;	// 分配失败，返回NULL
	}

	// 初始化probe的基础设置
	base->dev = 1;	// 设置设备编号为1
	base->range = ~0;	// 设置范围为最大值
	// 设置探测函数为传入的base_probe
	base->get = base_probe;
	// 将所有probe指针指向base
	for (i = 0; i < 255; i++)
		p->probes[i] = base;
	// 设置kobj_map的锁
	p->lock = lock;
	// 返回初始化后的kobj_map指针
	return p;
}
