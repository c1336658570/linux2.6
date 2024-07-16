/*
 *  linux/fs/char_dev.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/major.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/seq_file.h>

#include <linux/kobject.h>
#include <linux/kobj_map.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/backing-dev.h>

#include "internal.h"

/*
 * capabilities for /dev/mem, /dev/kmem and similar directly mappable character
 * devices
 * - permits shared-mmap for read, write and/or exec
 * - does not permit private mmap in NOMMU mode (can't do COW)
 * - no readahead or I/O queue unplugging required
 */
// 这是为/dev/mem、/dev/kmem及类似的直接可映射字符设备设置的能力
// - 允许对读、写或执行进行共享内存映射
// - 在无MMU模式下不允许私有内存映射（无法进行写时复制）
// - 无需读取预处理或I/O队列拔除
struct backing_dev_info directly_mappable_cdev_bdi = {
	.name = "char",	 // 设备名为"char"
	.capabilities	= (
#ifdef CONFIG_MMU
		/* permit private copies of the data to be taken */
		BDI_CAP_MAP_COPY |	// 允许对数据进行私有复制
#endif
		/* permit direct mmap, for read, write or exec */
		BDI_CAP_MAP_DIRECT |	// 允许直接内存映射，用于读、写或执行
		BDI_CAP_READ_MAP | BDI_CAP_WRITE_MAP | BDI_CAP_EXEC_MAP),	// 分别为读、写、执行操作提供映射能力
};

// 用于字符设备的全局 kobj_map 指针
static struct kobj_map *cdev_map;

// 用于字符设备注册和注销操作的互斥锁
static DEFINE_MUTEX(chrdevs_lock);

// 一个全局指针数组chrdevs，用来管理分配出去的设备号。chrdevs的每一个元素都是一个指向char_device_struct结构体的指针。
/*
 * chrdevs实际上是一个哈希表，它的关键字为主设备号major，散列函数为index = major % 255。初始状态下，没有设备号被分配出去，
 * chrdevs数组为空。一旦有设备号被分配，则首先为该设备创建一个char_device_struct结构体对象，然后根据主设备号计算得到散列结果，
 * 将结构体对象挂载在chrdevs中的对应位置上。如果该位置已经有节点，则根据次设备号由小到大的寻找到合适的位置，挂载在对应节点的next指针上，构成有序列表。
 */
static struct char_device_struct {
	struct char_device_struct *next;   // 链表中指向下一个节点的指针
	unsigned int major;                // 设备的主设备号
	unsigned int baseminor;            // 次设备号的第一个
	int minorct;                       // 设备的次设备数量
	char name[64];                     // 设备名称
	/* will die */
	struct cdev *cdev;                 // 指向对应的 cdev 结构体，表示已注册的设备（此字段将被废弃）
} *chrdevs[CHRDEV_MAJOR_HASH_SIZE];   // 字符设备结构体的数组，使用主设备号的哈希进行索引


/* index in the above */
// 以下是一个内联函数，用于根据主设备号计算哈希索引
static inline int major_to_index(int major)
{
	return major % CHRDEV_MAJOR_HASH_SIZE;	// 返回主设备号对哈希大小取模的结果
}

// 如果定义了 CONFIG_PROC_FS，那么包含以下代码。这通常表示内核配置了对 /proc 文件系统的支持。
#ifdef CONFIG_PROC_FS

void chrdev_show(struct seq_file *f, off_t offset)
{
	struct char_device_struct *cd;

	// 只有当偏移量小于哈希大小时才处理，这确保了我们不会超出字符设备数组的范围
	if (offset < CHRDEV_MAJOR_HASH_SIZE) {
		mutex_lock(&chrdevs_lock);	// 锁定互斥锁以保证线程安全
		// 遍历位于给定哈希索引下的字符设备链表
		for (cd = chrdevs[offset]; cd; cd = cd->next)
			seq_printf(f, "%3d %s\n", cd->major, cd->name);	// 将每个设备的主设备号和名称格式化后写入序列文件
		mutex_unlock(&chrdevs_lock);	// 解锁互斥锁
	}
}

#endif /* CONFIG_PROC_FS */

/*
 * Register a single major with a specified minor range.
 *
 * If major == 0 this functions will dynamically allocate a major and return
 * its number.
 *
 * If major > 0 this function will attempt to reserve the passed range of
 * minors and will return zero on success.
 *
 * Returns a -ve errno on failure.
 */
// 在 Linux 内核中注册一个字符设备区域。该函数能够为设备动态分配主设备号或保留指定的主设备号和次设备号范围
/*
 * 注册一个指定次设备范围的单一主设备号。
 *
 * 如果 major == 0，此函数将动态分配一个主设备号并返回其编号。
 *
 * 如果 major > 0，此函数将尝试保留传递的次设备号范围，并在成功时返回零。
 *
 * 在失败时返回一个负的 errno。
 */
static struct char_device_struct *
__register_chrdev_region(unsigned int major, unsigned int baseminor,
			   int minorct, const char *name)
{
	struct char_device_struct *cd, **cp;// 声明指向字符设备结构体的指针和指针的指针
	int ret = 0; // 初始化返回值为 0
	int i; // 用于循环的变量

	cd = kzalloc(sizeof(struct char_device_struct), GFP_KERNEL);	// 分配内存并初始化为零
	if (cd == NULL)
		return ERR_PTR(-ENOMEM);	// 如果内存分配失败，则返回错误

	mutex_lock(&chrdevs_lock);	// 加锁以保证线程安全

	/* temporary */
	/* 临时 */
	// 如果未指定主设备号（即主设备号为0），则自动分配一个
	if (major == 0) {
		for (i = ARRAY_SIZE(chrdevs)-1; i > 0; i--) {
			if (chrdevs[i] == NULL)	// 找到第一个空的主设备号位置
				break;	// 寻找一个未使用的主设备号
		}

		if (i == 0) {
			ret = -EBUSY;	// 如果没有可用的设备号，返回设备忙的错误
			goto out;
		}
		major = i;	// 设置动态分配的主设备号
		ret = major;	// 返回新分配的主设备号
	}

	cd->major = major; // 设置主设备号
	cd->baseminor = baseminor; // 次设备号的第一个
	cd->minorct = minorct; // 设置次设备号的数量
	strlcpy(cd->name, name, sizeof(cd->name)); // 复制设备名称

	i = major_to_index(major);	// 获取主设备号的索引

	// 在链表中找到插入新设备的位置
	for (cp = &chrdevs[i]; *cp; cp = &(*cp)->next)
		if ((*cp)->major > major ||	// 找到主设备号更大的位置
		    ((*cp)->major == major &&	// 或者同一主设备号的适当位置
		     (((*cp)->baseminor >= baseminor) ||
		      ((*cp)->baseminor + (*cp)->minorct > baseminor))))
			break;	// 检查并找到正确的插入点以保证顺序

	/* Check for overlapping minor ranges.  */
	/* 检查次设备号范围重叠。 */
	if (*cp && (*cp)->major == major) {
		int old_min = (*cp)->baseminor;	// 已注册设备的起始次设备号
		int old_max = (*cp)->baseminor + (*cp)->minorct - 1;	// 已注册设备的最大次设备号
		int new_min = baseminor;	// 新设备的起始次设备号
		int new_max = baseminor + minorct - 1;	// 新设备的最大次设备号

		/* New driver overlaps from the left.  */
		/* 新驱动从左侧重叠。 */
		if (new_max >= old_min && new_max <= old_max) {
			ret = -EBUSY;	// 如果新的次设备号范围与已存在的范围重叠，返回忙碌
			goto out;
		}

		/* New driver overlaps from the right.  */
		/* 新驱动从右侧重叠。 */
		if (new_min <= old_max && new_min >= old_min) {	// 新设备次设备号从右侧重叠
			ret = -EBUSY;
			goto out;
		}
	}

	cd->next = *cp;	// 将新设备插入链表
	*cp = cd;
	mutex_unlock(&chrdevs_lock);	// 解锁
	return cd;	// 返回新注册的设备结构体
out:
	mutex_unlock(&chrdevs_lock);
	kfree(cd);	// 释放已分配的内存
	return ERR_PTR(ret);	// 返回错误
}

// 注销指定的主设备号和次设备号范围。
// 此函数用于在字符设备数组中找到并移除一个已注册的设备结构体。
// 如果找到并成功移除，则返回指向该字符设备结构体的指针；如果未找到，返回 NULL。
static struct char_device_struct *
__unregister_chrdev_region(unsigned major, unsigned baseminor, int minorct)
{
	struct char_device_struct *cd = NULL, **cp;	// 声明指向字符设备结构体的指针和指针的指针，初始化为 NULL
	int i = major_to_index(major);	// 将主设备号转换为索引

	mutex_lock(&chrdevs_lock);
	// 遍历链表寻找匹配的设备
	for (cp = &chrdevs[i]; *cp; cp = &(*cp)->next)
		if ((*cp)->major == major &&
		    (*cp)->baseminor == baseminor &&
		    (*cp)->minorct == minorct)
			break;	// 如果找到匹配的设备，跳出循环
	if (*cp) {	// 如果找到了设备
		cd = *cp;	// 保存找到的设备指针
		*cp = cd->next;	// 从链表中移除找到的设备
	}
	mutex_unlock(&chrdevs_lock);
	return cd;	// 返回被移除的设备的指针，如果没有找到设备则为 NULL
}

/**
 * register_chrdev_region() - register a range of device numbers
 * @from: the first in the desired range of device numbers; must include
 *        the major number.
 * @count: the number of consecutive device numbers required
 * @name: the name of the device or driver.
 *
 * Return value is zero on success, a negative error code on failure.
 */
/**
 * register_chrdev_region() - 注册一系列设备号
 * @from: 期望范围中的第一个设备号；必须包含主设备号。
 * @count: 所需连续设备号的数量。
 * @name: 设备或驱动的名称。
 *
 * 返回值：成功时返回零，失败时返回负的错误代码。
 */
int register_chrdev_region(dev_t from, unsigned count, const char *name)
{
	struct char_device_struct *cd;// 声明字符设备结构体指针
	dev_t to = from + count; // 计算总的设备号范围
	dev_t n, next; // 声明用于循环的设备号变量

	for (n = from; n < to; n = next) {	// 遍历需要注册的每个设备号
		next = MKDEV(MAJOR(n)+1, 0);	// 计算下一个主设备号的开始位置
		if (next > to)
			next = to;	// 确保不超出总范围
		cd = __register_chrdev_region(MAJOR(n), MINOR(n),
			       next - n, name);	// 尝试注册设备号范围
		if (IS_ERR(cd))	// 如果注册失败，跳转到错误处理
			goto fail;
	}
	return 0;	// 所有设备号注册成功，返回0
fail:
	to = n;	// 设置需要反注册的终止设备号
	for (n = from; n < to; n = next) {	// 遍历已注册的设备号进行清理
		next = MKDEV(MAJOR(n)+1, 0);	// 计算下一个主设备号的开始位置
		kfree(__unregister_chrdev_region(MAJOR(n), MINOR(n), next - n));	// 反注册设备号
	}
	return PTR_ERR(cd);	// 返回注册失败的错误代码
}

/**
 * alloc_chrdev_region() - register a range of char device numbers
 * @dev: output parameter for first assigned number
 * @baseminor: first of the requested range of minor numbers
 * @count: the number of minor numbers required
 * @name: the name of the associated device or driver
 *
 * Allocates a range of char device numbers.  The major number will be
 * chosen dynamically, and returned (along with the first minor number)
 * in @dev.  Returns zero or a negative error code.
 */
/**
 * alloc_chrdev_region() - 注册一系列字符设备号
 * @dev: 用于输出第一个被分配的设备号
 * @baseminor: 请求的次设备号范围的起始号
 * @count: 所需的次设备号数量
 * @name: 关联设备或驱动的名称
 *
 * 分配一系列字符设备号。主设备号将被动态选择，并返回（连同第一个次设备号）在 @dev 中。
 * 返回零或负的错误代码。
 */
int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count,
			const char *name)
{
	struct char_device_struct *cd;	// 声明一个字符设备结构体指针
	// 动态分配主设备号，并尝试注册设备号范围
	cd = __register_chrdev_region(0, baseminor, count, name);
	if (IS_ERR(cd))	// 检查注册结果是否有误
		return PTR_ERR(cd);	// 如果有错误，返回错误代码
	*dev = MKDEV(cd->major, cd->baseminor);	// 设置输出参数，分配的设备号（包括主设备号和起始次设备号）
	return 0;
}

/**
 * __register_chrdev() - create and register a cdev occupying a range of minors
 * @major: major device number or 0 for dynamic allocation
 * @baseminor: first of the requested range of minor numbers
 * @count: the number of minor numbers required
 * @name: name of this range of devices
 * @fops: file operations associated with this devices
 *
 * If @major == 0 this functions will dynamically allocate a major and return
 * its number.
 *
 * If @major > 0 this function will attempt to reserve a device with the given
 * major number and will return zero on success.
 *
 * Returns a -ve errno on failure.
 *
 * The name of this device has nothing to do with the name of the device in
 * /dev. It only helps to keep track of the different owners of devices. If
 * your module name has only one type of devices it's ok to use e.g. the name
 * of the module here.
 */
/**
 * __register_chrdev() - 创建并注册一个占用一定范围次设备号的字符设备
 * @major: 主设备号，如果为0则动态分配
 * @baseminor: 请求范围中的第一个次设备号
 * @count: 需要的次设备号数量
 * @name: 这一范围设备的名称
 * @fops: 与这些设备关联的文件操作
 *
 * 如果 @major == 0，此函数将动态分配一个主设备号并返回其编号。
 * 如果 @major > 0，此函数将尝试为给定的主设备号预留设备，并在成功时返回零。
 * 在失败时返回负的 errno。
 *
 * 这里的设备名称与 /dev 中的设备名称无关。它仅用于追踪不同设备的拥有者。
 * 如果您的模块只有一种类型的设备，使用模块名称作为这里的名称是可以的。
 */
// 实现了字符设备的注册流程，包括设备号的分配、cdev 结构的初始化和注册。它还处理了各种可能的错误情况，并在发生错误时进行适当的清理工作。
int __register_chrdev(unsigned int major, unsigned int baseminor,
		      unsigned int count, const char *name,
		      const struct file_operations *fops)
{
	struct char_device_struct *cd;
	struct cdev *cdev;
	int err = -ENOMEM;	// 初始设置错误码为内存不足

	cd = __register_chrdev_region(major, baseminor, count, name);	// 注册设备号范围
	if (IS_ERR(cd))
		return PTR_ERR(cd);	// 如果注册失败，返回错误代码
	
	cdev = cdev_alloc();	// 分配一个 cdev 结构
	if (!cdev)
		goto out2;	// 如果分配失败，跳转到错误处理代码

	cdev->owner = fops->owner;	// 设置 cdev 的拥有者
	cdev->ops = fops;	// 设置 cdev 的操作函数
	kobject_set_name(&cdev->kobj, "%s", name);	// 设置 cdev 的内核对象名称
		
	err = cdev_add(cdev, MKDEV(cd->major, baseminor), count);	// 将 cdev 添加到系统中
	if (err)
		goto out;	// 如果添加失败，跳转到错误处理代码

	cd->cdev = cdev;	// 将 cdev 结构链接到设备号注册结构

	return major ? 0 : cd->major;	// 如果是动态分配的主设备号，返回分配的主设备号，否则返回0
out:
	kobject_put(&cdev->kobj);	// 释放内核对象
out2:
	kfree(__unregister_chrdev_region(cd->major, baseminor, count));	// 取消注册设备号，并释放资源
	return err;	// 返回错误代码
}

/**
 * unregister_chrdev_region() - return a range of device numbers
 * @from: the first in the range of numbers to unregister
 * @count: the number of device numbers to unregister
 *
 * This function will unregister a range of @count device numbers,
 * starting with @from.  The caller should normally be the one who
 * allocated those numbers in the first place...
 */
/**
 * unregister_chrdev_region() - 注销一系列设备号
 * @from: 要注销的第一个设备号
 * @count: 要注销的设备号数量
 *
 * 该函数将注销从 @from 开始的 @count 个设备号。通常，调用者应该是最初分配这些设备号的实体。
 */
void unregister_chrdev_region(dev_t from, unsigned count)
{
	dev_t to = from + count;	// 计算结束设备号
	dev_t n, next;	// 用于遍历的变量

	for (n = from; n < to; n = next) {	// 遍历需要注销的每个设备号
		next = MKDEV(MAJOR(n)+1, 0);	// 计算下一个主设备号的开始
		if (next > to)
			next = to;	// 确保不超出总范围
		kfree(__unregister_chrdev_region(MAJOR(n), MINOR(n), next - n));	// 注销当前范围内的设备号
	}
}

/**
 * __unregister_chrdev - unregister and destroy a cdev
 * @major: major device number
 * @baseminor: first of the range of minor numbers
 * @count: the number of minor numbers this cdev is occupying
 * @name: name of this range of devices
 *
 * Unregister and destroy the cdev occupying the region described by
 * @major, @baseminor and @count.  This function undoes what
 * __register_chrdev() did.
 */
/**
 * __unregister_chrdev - 注销并销毁一个 cdev
 * @major: 主设备号
 * @baseminor: 次设备号范围的起始号
 * @count: 这个 cdev 占用的次设备号数量
 * @name: 这一范围设备的名称
 *
 * 注销并销毁由 @major, @baseminor 和 @count 描述的区域中的 cdev。此函数撤销了 __register_chrdev() 所做的操作。
 */
void __unregister_chrdev(unsigned int major, unsigned int baseminor,
			 unsigned int count, const char *name)
{
	struct char_device_struct *cd;	// 声明一个字符设备结构体指针

	cd = __unregister_chrdev_region(major, baseminor, count);	// 注销指定范围的设备号
	if (cd && cd->cdev)	// 如果设备结构体存在且包含一个 cdev
		cdev_del(cd->cdev);	// 删除 cdev
	kfree(cd);	// 释放字符设备结构体占用的内存
}

// 定义一个自旋锁，用于同步对字符设备的访问
static DEFINE_SPINLOCK(cdev_lock);

// 获取一个字符设备的 kobject，同时增加其模块的引用计数
static struct kobject *cdev_get(struct cdev *p)
{
	struct module *owner = p->owner;	// 获取字符设备的拥有者模块
	struct kobject *kobj;

	if (owner && !try_module_get(owner))	// 尝试增加拥有者模块的引用计数，如果失败则返回 NULL
		return NULL;
	kobj = kobject_get(&p->kobj);	// 获取并增加字符设备的 kobject 的引用计数
	if (!kobj)	// 如果获取 kobject 失败
		module_put(owner);	// 减少拥有者模块的引用计数，因为之前增加操作成功了
	return kobj;	// 返回获取的 kobject
}

// 释放一个字符设备的 kobject，并减少其模块的引用计数
void cdev_put(struct cdev *p)
{
	if (p) {	// 如果传入的字符设备指针非空
		struct module *owner = p->owner;	// 获取字符设备的拥有者模块
		kobject_put(&p->kobj);	// 减少字符设备 kobject 的引用计数
		module_put(owner);	 // 减少拥有者模块的引用计数
	}
}

/*
 * Called every time a character special file is opened
 */
/*
 * 每次打开字符特殊文件时调用
 */
static int chrdev_open(struct inode *inode, struct file *filp)
{
	struct cdev *p; // 指向 cdev 结构的指针
	struct cdev *new = NULL; // 新 cdev 结构的指针，初始为 NULL
	int ret = 0; // 返回值，默认为 0，表示成功

	spin_lock(&cdev_lock);	// 加锁，保护 cdev 结构
	p = inode->i_cdev;	// 获取 inode 关联的 cdev
	if (!p) {
		struct kobject *kobj;
		int idx;	// kobject 索引
		spin_unlock(&cdev_lock);	// 释放锁，因为需要进行可能阻塞的操作
		// 查找设备对应的 kobject
		kobj = kobj_lookup(cdev_map, inode->i_rdev, &idx);
		if (!kobj)
			return -ENXIO;	// 如果 kobject 不存在，返回错误
		// 从 kobject 获取 cdev
		new = container_of(kobj, struct cdev, kobj);
		spin_lock(&cdev_lock);	// 再次加锁
		/* Check i_cdev again in case somebody beat us to it while
		   we dropped the lock. */
		/* 再次检查 i_cdev 以防在释放锁期间有变化 */
		p = inode->i_cdev;
		if (!p) {	// 如果仍然没有 cdev
			inode->i_cdev = p = new;	// 设置 inode 的 cdev
			list_add(&inode->i_devices, &p->list);	// 将设备添加到设备列表
			new = NULL;	// 设置 new 为 NULL，表示不需要释放
		} else if (!cdev_get(p))
			ret = -ENXIO;	// 如果获取 cdev 失败，设置返回错误
	} else if (!cdev_get(p))
		ret = -ENXIO;	// 如果获取 cdev 失败，设置返回错误
	spin_unlock(&cdev_lock);	// 释放锁
	cdev_put(new);	// 如果 new 非 NULL，释放它
	if (ret)
		return ret;	// 如果有错误，直接返回错误

	ret = -ENXIO;	// 设置默认错误
	filp->f_op = fops_get(p->ops);	// 获取文件操作指针
	if (!filp->f_op)	// 如果操作指针不存在，跳转到错误处理
		goto out_cdev_put;

	if (filp->f_op->open) {
		ret = filp->f_op->open(inode,filp);	// 调用 open 方法
		if (ret)
			goto out_cdev_put;	// 如果 open 方法返回错误，跳转到错误处理
	}

	return 0;	// 返回成功

 out_cdev_put:
	cdev_put(p);	// 释放 cdev
	return ret;		// 返回错误
}

// 获取与inode关联的字符设备的索引
int cdev_index(struct inode *inode)
{
	int idx;	// 用于存储索引的变量
	struct kobject *kobj;

	kobj = kobj_lookup(cdev_map, inode->i_rdev, &idx);	// 在cdev_map中查找inode的设备号对应的kobject
	if (!kobj)
		return -1;	// 如果没有找到，返回-1
	kobject_put(kobj);	// 释放kobject的引用
	return idx;	// 返回找到的索引
}

// 忘记与inode关联的字符设备，通常在释放inode时调用
void cd_forget(struct inode *inode)
{
	spin_lock(&cdev_lock); // 加锁以保护对设备列表的修改
	list_del_init(&inode->i_devices); // 从设备列表中删除并重新初始化列表节点
	inode->i_cdev = NULL; // 清除inode的cdev指针
	spin_unlock(&cdev_lock); // 释放锁
}

// 清除cdev关联的所有inode，用于cdev的清理
static void cdev_purge(struct cdev *cdev)
{
	spin_lock(&cdev_lock); // 加锁以保护对设备列表的修改
	while (!list_empty(&cdev->list)) { // 循环，直到cdev的设备列表为空
		struct inode *inode;
		inode = container_of(cdev->list.next, struct inode, i_devices); // 从列表中获取下一个inode
		list_del_init(&inode->i_devices); // 从设备列表中删除并重新初始化列表节点
		inode->i_cdev = NULL; // 清除inode的cdev指针
	}
	spin_unlock(&cdev_lock); // 释放锁
}

/*
 * Dummy default file-operations: the only thing this does
 * is contain the open that then fills in the correct operations
 * depending on the special file...
 */
/*
 * 用作默认的文件操作的虚拟对象：这里唯一的操作是 open，
 * 它将根据特定文件填充正确的操作...
 */
const struct file_operations def_chr_fops = {
	.open = chrdev_open,	// 指定 open 操作，由 chrdev_open 函数处理
};

// 根据给定的设备号和数据精确匹配 kobject
static struct kobject *exact_match(dev_t dev, int *part, void *data)
{
	struct cdev *p = data;	// 从 data 参数转换得到 cdev 对象
	return &p->kobj;	// 返回 cdev 对象中的 kobject 成员的地址
}

// 尝试锁定一个 cdev 对象，如果成功，返回0；如果失败，返回-1
static int exact_lock(dev_t dev, void *data)
{
	struct cdev *p = data; // 从 data 参数转换得到 cdev 对象
	return cdev_get(p) ? 0 : -1; // 尝试获取 cdev 的引用，成功则返回 0，失败则返回 -1
}

/**
 * cdev_add() - add a char device to the system
 * @p: the cdev structure for the device
 * @dev: the first device number for which this device is responsible
 * @count: the number of consecutive minor numbers corresponding to this
 *         device
 *
 * cdev_add() adds the device represented by @p to the system, making it
 * live immediately.  A negative error code is returned on failure.
 */
/**
 * cdev_add() - 将字符设备添加到系统中
 * @p: 设备的 cdev 结构
 * @dev: 该设备负责的第一个设备号
 * @count: 该设备对应的连续次设备号数量
 *
 * cdev_add() 将由 @p 表示的设备添加到系统中，使其立即生效。失败时返回负的错误代码。
 */
int cdev_add(struct cdev *p, dev_t dev, unsigned count)
{
	p->dev = dev;	// 设置 cdev 的设备号
	p->count = count;	// 设置 cdev 负责的次设备号数量
	// 在内核对象映射中添加设备，使用精确匹配和锁定策略
	return kobj_map(cdev_map, dev, count, NULL, exact_match, exact_lock, p);
}

/**
 * 从系统中移除设备的映射
 */
static void cdev_unmap(dev_t dev, unsigned count)
{
	// 调用 kobj_unmap 从内核对象映射中移除设备
	kobj_unmap(cdev_map, dev, count);
}

/**
 * cdev_del() - remove a cdev from the system
 * @p: the cdev structure to be removed
 *
 * cdev_del() removes @p from the system, possibly freeing the structure
 * itself.
 */
/**
 * cdev_del() - 从系统中移除一个 cdev
 * @p: 要移除的 cdev 结构体
 *
 * cdev_del() 从系统中移除 @p，可能会释放结构体本身。
 */
void cdev_del(struct cdev *p)
{
	cdev_unmap(p->dev, p->count);	// 移除设备映射
	kobject_put(&p->kobj);				// 释放与 cdev 关联的 kobject
}

/**
 * 当默认的 cdev 需要被释放时调用此函数
 */
static void cdev_default_release(struct kobject *kobj)
{
	// 从 kobject 获取 cdev
	struct cdev *p = container_of(kobj, struct cdev, kobj);
	cdev_purge(p);	// 清除 cdev 关联的所有 inode
}

/**
 * 当动态分配的 cdev 需要被释放时调用此函数
 */
static void cdev_dynamic_release(struct kobject *kobj)
{
	struct cdev *p = container_of(kobj, struct cdev, kobj);	// 从 kobject 获取 cdev
	cdev_purge(p); // 清除 cdev 关联的所有 inode
	kfree(p); // 释放 cdev 占用的内存
}

// 定义默认的 cdev kobject 类型
static struct kobj_type ktype_cdev_default = {
	.release	= cdev_default_release,	// 设置释放函数
};

// 定义动态分配的 cdev kobject 类型
static struct kobj_type ktype_cdev_dynamic = {
	.release	= cdev_dynamic_release,	// 设置释放函数
};

/**
 * cdev_alloc() - allocate a cdev structure
 *
 * Allocates and returns a cdev structure, or NULL on failure.
 */
/**
 * cdev_alloc() - 分配一个 cdev 结构体
 *
 * 分配并返回一个 cdev 结构体，如果失败则返回 NULL。
 */
struct cdev *cdev_alloc(void)
{
	struct cdev *p = kzalloc(sizeof(struct cdev), GFP_KERNEL);	// 使用内核内存分配函数分配并初始化为零的 cdev 结构体
	if (p) {	// 如果内存分配成功
		INIT_LIST_HEAD(&p->list);	// 初始化 cdev 结构体中的链表头
		kobject_init(&p->kobj, &ktype_cdev_dynamic);	// 初始化 cdev 的 kobject，并指定其类型为动态释放类型
	}
	return p;	// 返回分配的 cdev 结构体，如果分配失败则为 NULL
}

/**
 * cdev_init() - initialize a cdev structure
 * @cdev: the structure to initialize
 * @fops: the file_operations for this device
 *
 * Initializes @cdev, remembering @fops, making it ready to add to the
 * system with cdev_add().
 */
/**
 * cdev_init() - 初始化一个 cdev 结构体
 * @cdev: 要初始化的结构体
 * @fops: 此设备的文件操作结构体
 *
 * 初始化 @cdev，记录 @fops，使其准备好通过 cdev_add() 添加到系统中。
 */
void cdev_init(struct cdev *cdev, const struct file_operations *fops)
{
	memset(cdev, 0, sizeof *cdev); // 清零整个 cdev 结构体
	INIT_LIST_HEAD(&cdev->list); // 初始化 cdev 的链表头
	kobject_init(&cdev->kobj, &ktype_cdev_default); // 初始化 cdev 的 kobject，使用默认的 kobject 类型
	cdev->ops = fops; // 设置 cdev 的文件操作指针
}

/**
 * 基于设备号动态加载相应的设备模块
 */
static struct kobject *base_probe(dev_t dev, int *part, void *data)
{
	if (request_module("char-major-%d-%d", MAJOR(dev), MINOR(dev)) > 0)
		/* Make old-style 2.4 aliases work */
		/* 为了兼容旧式的 2.4 别名机制 */
		request_module("char-major-%d", MAJOR(dev));
	return NULL;	// 此函数不返回 kobject，仅触发模块加载
}

/**
 * 初始化字符设备驱动
 */
void __init chrdev_init(void)
{
	cdev_map = kobj_map_init(base_probe, &chrdevs_lock);	// 初始化字符设备的 kobject 映射，并设置基础探测函数
	bdi_init(&directly_mappable_cdev_bdi);	// 初始化直接可映射字符设备的后备设备信息
}


/* Let modules do char dev stuff */
EXPORT_SYMBOL(register_chrdev_region);
EXPORT_SYMBOL(unregister_chrdev_region);
EXPORT_SYMBOL(alloc_chrdev_region);
EXPORT_SYMBOL(cdev_init);
EXPORT_SYMBOL(cdev_alloc);
EXPORT_SYMBOL(cdev_del);
EXPORT_SYMBOL(cdev_add);
EXPORT_SYMBOL(cdev_index);
EXPORT_SYMBOL(__register_chrdev);
EXPORT_SYMBOL(__unregister_chrdev);
EXPORT_SYMBOL(directly_mappable_cdev_bdi);
