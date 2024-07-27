/*
 * drivers/base/core.c - core driver model code (device registration, etc)
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2006 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2006 Novell, Inc.
 *
 * This file is released under the GPLv2
 *
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kdev_t.h>
#include <linux/notifier.h>
#include <linux/genhd.h>
#include <linux/kallsyms.h>
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/async.h>

#include "base.h"
#include "power/power.h"

// 定义了两个函数指针，用于通知平台（操作系统）关于设备的添加和移除。
int (*platform_notify)(struct device *dev) = NULL;  // 当设备被添加时触发的回调，初始化为 NULL
int (*platform_notify_remove)(struct device *dev) = NULL;  // 当设备被移除时触发的回调，初始化为 NULL
// 定义了几个全局的 kobject 指针，用于在 sysfs 中表示设备。
static struct kobject *dev_kobj;  // 一个静态全局变量，指向设备的 kobject
struct kobject *sysfs_dev_char_kobj;  // 指向字符设备的 sysfs kobject
struct kobject *sysfs_dev_block_kobj;  // 指向块设备的 sysfs kobject

// 使用条件编译指令根据配置决定如何实现 device_is_not_partition 函数。
#ifdef CONFIG_BLOCK	// 如果定义了 CONFIG_BLOCK，意味着系统支持块设备
static inline int device_is_not_partition(struct device *dev)
{
	return !(dev->type == &part_type);	// 如果设备类型不是分区，返回 1，否则返回 0
}
#else	// 如果没有定义 CONFIG_BLOCK，系统不支持块设备
static inline int device_is_not_partition(struct device *dev)
{
	return 1;	// 总是返回 1，表示设备不是分区
}
#endif

/**
 * dev_driver_string - Return a device's driver name, if at all possible
 * @dev: struct device to get the name of
 *
 * Will return the device's driver's name if it is bound to a device.  If
 * the device is not bound to a device, it will return the name of the bus
 * it is attached to.  If it is not attached to a bus either, an empty
 * string will be returned.
 */
/**
 * dev_driver_string - 返回设备的驱动程序名称（如果可能）
 * @dev: 要获取名称的设备结构体
 *
 * 如果设备绑定了驱动程序，将返回驱动程序的名称。
 * 如果设备没有绑定到驱动程序，它将返回设备所附属的总线的名称。
 * 如果设备既没有绑定驱动程序也没有附属于任何总线，将返回空字符串。
 */
const char *dev_driver_string(const struct device *dev)
{
	struct device_driver *drv;	// 定义一个指向设备驱动结构体的指针

	/* 
	 * dev->driver can change to NULL underneath us because of unbinding,
	 * so be careful about accessing it.  dev->bus and dev->class should
	 * never change once they are set, so they don't need special care.
	 */
	/* 
	 * 设备的驱动程序可能因为解绑而变为 NULL，所以访问它时需要小心。
	 * 设备的 bus 和 class 一旦被设置后就不会改变，因此不需要特别处理。
	 */
	drv = ACCESS_ONCE(dev->driver);	 // 原子地读取 dev->driver，避免在读取过程中该值被修改
	return drv ? drv->name :	// 如果 drv 不为空，返回驱动程序的名称
			(dev->bus ? dev->bus->name :	// 如果设备有总线，返回总线的名称
			(dev->class ? dev->class->name : ""));	// 如果设备有类，返回类的名称，否则返回空字符串
}
EXPORT_SYMBOL(dev_driver_string);

// 定义了两个宏用于从 kobject 和 attribute 获取相应的设备和设备属性结构体
#define to_dev(obj) container_of(obj, struct device, kobj)	// 从 kobject 获取对应的 struct device
#define to_dev_attr(_attr) container_of(_attr, struct device_attribute, attr)	// 从 attribute 获取对应的 struct device_attribute

/**
 * dev_attr_show - 显示设备属性的函数
 * @kobj: 指向设备 kobject 的指针
 * @attr: 指向属性结构的指针
 * @buf: 用于存放属性值的缓冲区
 *
 * 此函数由 sysfs 调用，用于读取设备属性。如果设备属性有对应的 'show' 方法，
 * 则调用该方法来填充 buf。如果读取的数据超过了页面大小（PAGE_SIZE），记录一个错误信息。
 * 返回: 成功时返回读取的字节数，失败时返回错误代码（如 -EIO）。
 */
static ssize_t dev_attr_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
	struct device_attribute *dev_attr = to_dev_attr(attr);	// 获取设备属性结构
	struct device *dev = to_dev(kobj);	// 获取设备结构
	ssize_t ret = -EIO;	// 默认返回值为 -EIO，表示 I/O 错误

	if (dev_attr->show)	// 检查是否有 'show' 方法
		ret = dev_attr->show(dev, dev_attr, buf);	// 调用 'show' 方法填充 buf
	if (ret >= (ssize_t)PAGE_SIZE) {	// 检查返回的字节数是否异常（超过了页面大小）
		print_symbol("dev_attr_show: %s returned bad count\n",
				(unsigned long)dev_attr->show);	// 记录错误信息
	}
	return ret;	// 返回 'show' 方法的结果或错误代码
}

/**
 * dev_attr_store - 写入设备属性的函数
 * @kobj: 指向设备 kobject 的指针
 * @attr: 指向属性结构的指针
 * @buf: 包含要写入的数据的缓冲区
 * @count: 缓冲区中数据的字节数
 *
 * 此函数由 sysfs 调用，用于处理向设备属性写入数据。如果设备属性有对应的 'store' 方法，
 * 则调用该方法来处理数据。如果没有定义 'store' 方法，返回一个错误。
 * 返回: 成功时返回写入的字节数，失败时返回错误代码（如 -EIO）。
 */
static ssize_t dev_attr_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	struct device_attribute *dev_attr = to_dev_attr(attr);	// 获取设备属性结构
	struct device *dev = to_dev(kobj);	// 获取设备结构
	ssize_t ret = -EIO;	// 默认返回值为 -EIO，表示 I/O 错误

	if (dev_attr->store)	// 检查是否有 'store' 方法
		ret = dev_attr->store(dev, dev_attr, buf, count);	// 调用 'store' 方法处理数据
	return ret;	// 返回 'store' 方法的结果或错误代码
}

/**
 * dev_sysfs_ops - 设备 sysfs 操作结构
 *
 * 该结构定义了处理设备 sysfs 属性的函数。
 * .show 指向处理属性读取的函数。
 * .store 指向处理属性写入的函数。
 */
static const struct sysfs_ops dev_sysfs_ops = {
	.show	= dev_attr_show,	// 指向 dev_attr_show 函数，处理读取属性
	.store	= dev_attr_store,	// 指向 dev_attr_store 函数，处理写入属性
};


/**
 *	device_release - free device structure.
 *	@kobj:	device's kobject.
 *
 *	This is called once the reference count for the object
 *	reaches 0. We forward the call to the device's release
 *	method, which should handle actually freeing the structure.
 */
/**
 * device_release - 释放设备结构
 * @kobj: 设备的 kobject
 *
 * 当对象的引用计数达到 0 时调用此函数。我们将调用转发给设备的 release 方法，
 * 该方法应该负责实际释放结构。
 */
static void device_release(struct kobject *kobj)
{
	struct device *dev = to_dev(kobj);	// 从 kobject 获取对应的设备结构
	struct device_private *p = dev->p;	// 获取设备的私有数据

	if (dev->release)
		dev->release(dev);	// 如果设备有自定义的 release 方法，调用它
	else if (dev->type && dev->type->release)
		dev->type->release(dev);	// 如果设备类型有 release 方法，调用它
	else if (dev->class && dev->class->dev_release)
		dev->class->dev_release(dev);	// 如果设备类有 dev_release 方法，调用它
	else
		WARN(1, KERN_ERR "Device '%s' does not have a release() "
			"function, it is broken and must be fixed.\n",
			dev_name(dev));	// 如果没有找到任何释放函数，打印警告信息
	kfree(p);	// 释放设备的私有数据
}

/**
 * device_ktype - 设备 kobject 类型
 *
 * 这个结构定义了设备 kobject 的行为，包括释放函数和 sysfs 操作。
 */
static struct kobj_type device_ktype = {
	.release	= device_release,	// 设定释放函数为 device_release
	.sysfs_ops	= &dev_sysfs_ops,	// 设定 sysfs 操作为 dev_sysfs_ops
};

/**
 * dev_uevent_filter - 过滤设备的 uevent
 * @kset: kset 对象
 * @kobj: kobject 对象
 *
 * 此函数用于决定给定的 kobject 是否应该生成 uevent。
 * 只有当 kobject 的类型为 device_ktype，并且设备属于某个总线或类时，才允许生成 uevent。
 *
 * 返回: 如果设备应该生成 uevent，返回 1；否则返回 0。
 */
static int dev_uevent_filter(struct kset *kset, struct kobject *kobj)
{
	struct kobj_type *ktype = get_ktype(kobj);	// 获取 kobject 的类型

	if (ktype == &device_ktype) {	// 检查 kobject 是否是设备类型
		struct device *dev = to_dev(kobj);	// 从 kobject 获取设备结构体
		if (dev->bus)	// 如果设备属于某个总线，返回 1，允许生成 uevent
			return 1;
		if (dev->class)	// 如果设备属于某个类，返回 1，允许生成 uevent
			return 1;
	}
	return 0;	// 如果不属于特定总线或类，返回 0，不生成 uevent
}

/**
 * dev_uevent_name - 获取生成 uevent 时设备应使用的名称
 * @kset: kset 对象，用于组织 kobject
 * @kobj: 设备的 kobject
 *
 * 此函数根据设备所属的总线或类来确定应使用的 uevent 名称。
 * 如果设备属于某个总线，它将返回总线的名称；如果设备属于某个类，它将返回类的名称。
 * 如果设备既不属于总线也不属于类，返回 NULL。
 *
 * 返回: 设备的总线或类的名称，如果都不适用，则为 NULL。
 */
static const char *dev_uevent_name(struct kset *kset, struct kobject *kobj)
{
	struct device *dev = to_dev(kobj);	// 从 kobject 获取对应的设备结构体

	if (dev->bus)
		return dev->bus->name;	// 如果设备属于某个总线，返回总线的名称
	if (dev->class)
		return dev->class->name;	// 如果设备属于某个类，返回类的名称
	return NULL;	// 如果设备既不属于总线也不属于类，返回 NULL
}

/**
 * dev_uevent - 为设备生成用户空间事件（uevent）
 * @kset: 设备所属的 kset 对象
 * @kobj: 设备对应的 kobject
 * @env: uevent 环境变量结构，用于存储向用户空间发送的信息
 *
 * 此函数负责为一个设备生成 uevent，并根据设备的不同属性向 env 环境变量列表中添加信息。
 * 这包括设备的主次设备号、设备类型、所属驱动程序等信息。
 * 最后，函数还会调用与设备相关联的总线、类或类型的 uevent 函数，以允许它们添加特定信息。
 * 返回: 成功时返回 0，或者返回特定的错误代码。
 */
static int dev_uevent(struct kset *kset, struct kobject *kobj,
		      struct kobj_uevent_env *env)
{
	struct device *dev = to_dev(kobj);	// 从 kobject 获取设备对象
	int retval = 0;	// 初始化返回值为 0

	/* add device node properties if present */
	/* 如果设备有主设备号，添加设备节点属性 */
	/* 如果设备具有有效的主设备号，添加相应的环境变量 */
	if (MAJOR(dev->devt)) {
		const char *tmp;  // 用于临时存储设备节点名称或其他数据
		const char *name;  // 设备名称
		mode_t mode = 0;  // 设备文件的模式（权限）

		add_uevent_var(env, "MAJOR=%u", MAJOR(dev->devt));	// 添加主设备号到环境变量
		add_uevent_var(env, "MINOR=%u", MINOR(dev->devt));	// 添加次设备号到环境变量
		name = device_get_devnode(dev, &mode, &tmp);	// 获取设备节点名称，并可能获取设备文件的模式
		if (name) {
			add_uevent_var(env, "DEVNAME=%s", name);	// 如果有设备名称，则添加 DEVNAME 环境变量
			kfree(tmp);	// 释放由 device_get_devnode 分配的内存
			if (mode)
				add_uevent_var(env, "DEVMODE=%#o", mode & 0777);	// 如果提供了模式信息，则添加 DEVMODE 现场变量
		}
	}

	/* 添加设备类型 */
	// 如果设备有设备类型，并且类型名称有效，则添加 DEVTYPE 环境变量
	if (dev->type && dev->type->name)
		add_uevent_var(env, "DEVTYPE=%s", dev->type->name);

	/* 添加驱动程序名称 */
	// 如果设备绑定到了驱动程序，则添加 DRIVER 环境变量
	if (dev->driver)
		add_uevent_var(env, "DRIVER=%s", dev->driver->name);

#ifdef CONFIG_SYSFS_DEPRECATED
	/* 对于设备类，寻找父设备的总线和驱动信息 */
	// 特定于设备类的处理逻辑
	if (dev->class) {
		struct device *parent = dev->parent;

		/* find first bus device in parent chain */
		/* 在父设备链中查找第一个总线设备 */
		// 查找具有总线信息的最近的父设备
		while (parent && !parent->bus)
			parent = parent->parent;
		if (parent && parent->bus) {
			const char *path;

			// 获取父设备的路径
			path = kobject_get_path(&parent->kobj, GFP_KERNEL);	// 添加物理设备路径
			if (path) {
				add_uevent_var(env, "PHYSDEVPATH=%s", path);	// 添加物理设备路径环境变量
				kfree(path);
			}

			// 添加物理设备总线名称环境变量
			add_uevent_var(env, "PHYSDEVBUS=%s", parent->bus->name);	// 添加物理设备总线名称

			// 如果父设备绑定到了驱动程序，添加驱动程序名称环境变量
			if (parent->driver)
				add_uevent_var(env, "PHYSDEVDRIVER=%s",
					       parent->driver->name);	// 添加物理设备驱动名称
		}
	} else if (dev->bus) {
		// 如果设备属于某个总线，添加总线名称和驱动程序名称环境变量
		add_uevent_var(env, "PHYSDEVBUS=%s", dev->bus->name);

		if (dev->driver)
			add_uevent_var(env, "PHYSDEVDRIVER=%s",
				       dev->driver->name);
	}
#endif

	/* have the bus specific function add its stuff */
	/* 调用总线特定函数添加信息 */
	// 调用总线特定的 uevent 函数，可能会添加额外的环境变量或处理特殊逻辑
	if (dev->bus && dev->bus->uevent) {
		retval = dev->bus->uevent(dev, env);
		if (retval)
			pr_debug("device: '%s': %s: bus uevent() returned %d\n",
				 dev_name(dev), __func__, retval);
	}

	/* have the class specific function add its stuff */
	/* 调用类特定函数添加信息 */
	if (dev->class && dev->class->dev_uevent) {
		retval = dev->class->dev_uevent(dev, env);
		if (retval)
			pr_debug("device: '%s': %s: class uevent() "
				 "returned %d\n", dev_name(dev),
				 __func__, retval);
	}

	/* have the device type specific fuction add its stuff */
	/* 调用设备类型特定函数添加信息 */
	if (dev->type && dev->type->uevent) {
		retval = dev->type->uevent(dev, env);
		if (retval)
			pr_debug("device: '%s': %s: dev_type uevent() "
				 "returned %d\n", dev_name(dev),
				 __func__, retval);
	}

	return retval;	// 返回操作结果
}

/**
 * device_uevent_ops - 设备 kset 的 uevent 操作定义
 *
 * 这个结构体定义了设备 kset 对于 uevent（用户空间事件）的操作方法。
 * 这包括过滤、命名和处理 uevent 的具体函数，它们决定了哪些事件会被发送到用户空间，
 * 事件的名称是什么，以及如何生成这些事件的细节。
 */
static const struct kset_uevent_ops device_uevent_ops = {
	// .filter 函数用于确定哪些 kobject 会生成 uevent。
	// 如果此函数返回 1，uevent 将会生成；如果返回 0，不生成 uevent。
	.filter =	dev_uevent_filter,	// 设备 uevent 的过滤函数
	// .name 函数用于提供 uevent 的名称，这个名称通常基于设备所属的类或总线。
	// 这个名称将用于生成 UEVENT 环境变量中的 NAME 字段。
	.name =		dev_uevent_name,	// 设备 uevent 的命名函数
	// .uevent 函数用于向 uevent 环境中添加具体的变量，这些变量描述了设备的详细状态，
	// 包括设备类型、驱动程序名称等信息。这个函数还可以调用总线、类或设备类型特定的
	// uevent 函数来进一步定制 uevent。
	.uevent =	dev_uevent,	// 设备 uevent 的处理函数
};

/**
 * show_uevent - 显示设备的 uevent 环境变量
 * @dev: 指向设备的指针
 * @attr: 设备属性结构
 * @buf: 用于存储输出的缓冲区
 *
 * 此函数通过搜索设备所属的 kset，调用 kset 的 uevent 函数，
 * 然后将环境变量输出到缓冲区中。这通常用于 sysfs 文件系统，
 * 允许用户空间程序通过读取文件来获取设备的详细状态信息。
 * 返回: 如果成功，返回写入 buf 的字节数；如果内存分配失败，返回 -ENOMEM。
 */
static ssize_t show_uevent(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct kobject *top_kobj; // 设备的顶级 kobject
	struct kset *kset; // kset 对象
	struct kobj_uevent_env *env = NULL; // 用于存储 uevent 环境变量的结构
	int i; // 循环索引
	size_t count = 0; // 缓冲区中的字符计数
	int retval; // 函数返回值

	/* search the kset, the device belongs to */
	/* 寻找设备所属的 kset */
	top_kobj = &dev->kobj;	// 获取设备的 kobject
	while (!top_kobj->kset && top_kobj->parent)	// 当 kobject 没有 kset 且有父对象时
		top_kobj = top_kobj->parent;	// 向上遍历到父对象
	if (!top_kobj->kset)	// 如果顶级 kobject 没有 kset
		goto out;	// 跳到函数末尾

	kset = top_kobj->kset;	// 获取 kset
	if (!kset->uevent_ops || !kset->uevent_ops->uevent)	// 如果 kset 没有定义 uevent 操作
		goto out;	// 跳到函数末尾

	/* respect filter */
	/* 遵循过滤规则 */
	if (kset->uevent_ops && kset->uevent_ops->filter)
		if (!kset->uevent_ops->filter(kset, &dev->kobj))	// 如果过滤函数存在且返回 0
			goto out;	// 表示不生成 uevent，跳到函数末尾

	env = kzalloc(sizeof(struct kobj_uevent_env), GFP_KERNEL);	// 分配环境变量结构
	if (!env)	// 如果内存分配失败
		return -ENOMEM;	// 返回内存错误

	/* let the kset specific function add its keys */
	/* 让 kset 特定的函数添加它的键 */
	retval = kset->uevent_ops->uevent(kset, &dev->kobj, env);	// 调用 kset 的 uevent 函数
	if (retval)	// 如果调用失败
		goto out;	// 跳到函数末尾

	/* copy keys to file */
	for (i = 0; i < env->envp_idx; i++)	// 遍历所有环境变量
		count += sprintf(&buf[count], "%s\n", env->envp[i]);	// 将每个变量加入到缓冲区
out:
	kfree(env);	// 释放环境变量结构
	return count;	// 返回缓冲区中的字符数
}

/**
 * store_uevent - 处理设备的 uevent 文件的写入操作
 * @dev: 目标设备
 * @attr: 设备属性
 * @buf: 用户写入的数据缓冲区
 * @count: 写入的字节数
 *
 * 此函数解析从用户空间传来的 uevent 动作字符串，然后根据这个字符串触发相应的 kobject uevent。
 * 如果传入的动作字符串无法识别，则记录一个错误消息。
 * 返回: 总是返回用户传入的字节数，表示成功接收。
 */
static ssize_t store_uevent(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	enum kobject_action action;

	if (kobject_action_type(buf, count, &action) == 0)	// 解析动作字符串
		kobject_uevent(&dev->kobj, action);	// 如果解析成功，触发对应的 kobject uevent
	else
		dev_err(dev, "uevent: unknown action-string\n");	// 如果动作字符串未知，记录错误
	return count;	// 返回写入的字节数
}

/**
 * uevent_attr - 设备属性定义
 *
 * 定义一个设备属性，使得 `uevent` 可以通过 sysfs 文件系统进行读写。
 * S_IRUGO | S_IWUSR: 文件权限，可读取所有用户，只有所有者可写。
 * show_uevent: 读取此属性时调用的函数。读取会获取到触发用户态事件时的环境变量
 * store_uevent: 写入此属性时调用的函数。写入会触发一个用户态事件
 */
static struct device_attribute uevent_attr =
	__ATTR(uevent, S_IRUGO | S_IWUSR, show_uevent, store_uevent);

/**
 * device_add_attributes - 向设备添加一组属性
 * @dev: 要添加属性的设备
 * @attrs: 属性数组，以 NULL 结尾
 *
 * 这个函数遍历一个属性数组，为每个属性创建一个 sysfs 文件。如果在创建过程中遇到任何错误，
 * 它将回滚所有已经添加的属性，确保不会因为部分添加成功而导致状态不一致。
 * 返回: 成功时返回 0，出错时返回负的错误码。
 */
static int device_add_attributes(struct device *dev,
				 struct device_attribute *attrs)
{
	int error = 0;	// 错误码初始化为 0
	int i;	// 用于遍历属性数组

	if (attrs) {	// 如果属性数组不为空
		for (i = 0; attr_name(attrs[i]); i++) {	// 遍历属性数组，直到遇到名字为空的属性
			error = device_create_file(dev, &attrs[i]);	// 为每个属性创建 sysfs 文件
			if (error)	// 如果创建失败
				break;	// 中断循环
		}
		if (error)	// 如果创建过程中出现错误
			while (--i >= 0)	// 回滚，移除已添加的所有属性
				device_remove_file(dev, &attrs[i]);
	}
	return error;	// 返回错误码，成功为0，失败为具体的错误码
}

/**
 * device_remove_attributes - 从设备中移除一组属性
 * @dev: 目标设备
 * @attrs: 要移除的属性数组
 *
 * 此函数遍历属性数组，为每个属性移除对应的 sysfs 文件。这通常用于设备注销或
 * 清理过程，确保系统的整洁和一致性。
 */
static void device_remove_attributes(struct device *dev,
				     struct device_attribute *attrs)
{
	int i;	// 用于遍历属性数组的索引

	if (attrs)	// 如果属性数组不为空
		for (i = 0; attr_name(attrs[i]); i++)	// 遍历属性数组，直到遇到名字为空的属性
			device_remove_file(dev, &attrs[i]);	// 移除每个属性对应的 sysfs 文件
}

/**
 * device_add_groups - 向设备添加多个属性组
 * @dev: 目标设备
 * @groups: 指向属性组数组的指针数组
 *
 * 此函数遍历属性组数组，为每个属性组创建对应的 sysfs 目录和文件。
 * 如果在添加过程中出现错误，则会回滚之前已成功添加的所有属性组，以保持状态一致性。
 * 返回：成功时返回 0；失败时返回负的错误码。
 */
static int device_add_groups(struct device *dev,
			     const struct attribute_group **groups)
{
	int error = 0;	// 初始化错误码为 0
	int i;	// 循环计数器

	if (groups) {	// 如果属性组数组不为空
		for (i = 0; groups[i]; i++) {	// 遍历属性组数组
			error = sysfs_create_group(&dev->kobj, groups[i]);	// 创建 sysfs 属性组
			if (error) {	// 如果创建属性组失败
				while (--i >= 0)	// 回滚所有已添加的属性组
					sysfs_remove_group(&dev->kobj,
							   groups[i]);
				break;	// 中断循环
			}
		}
	}
	return error;	// 返回错误码，成功为0，失败为具体的错误码
}

/**
 * device_remove_groups - 从设备中移除多个属性组
 * @dev: 目标设备
 * @groups: 指向属性组数组的指针数组
 *
 * 此函数遍历属性组数组，移除每个属性组对应的 sysfs 目录和文件。
 * 这通常用于设备注销或清理过程，确保从 sysfs 中彻底清除所有由设备注册的属性组。
 */
static void device_remove_groups(struct device *dev,
				 const struct attribute_group **groups)
{
	int i;	// 循环计数器

	if (groups)	// 如果属性组数组不为空
		for (i = 0; groups[i]; i++)	// 遍历属性组数组
			sysfs_remove_group(&dev->kobj, groups[i]);	// 移除每个属性组对应的 sysfs 文件
}

/**
 * device_add_attrs - 向设备添加属性和属性组
 * @dev: 需要添加属性的设备
 *
 * 此函数用于将属性和属性组添加到设备。这包括设备所属类的属性、设备类型的属性组，
 * 以及设备本身定义的属性组。如果在添加过程中出现错误，会进行适当的回滚操作，确保不留下不一致的状态。
 * 返回：成功时返回 0，失败时返回错误代码。
 */
static int device_add_attrs(struct device *dev)
{
	struct class *class = dev->class;  // 获取设备所属的类
	struct device_type *type = dev->type;  // 获取设备的类型
	int error;  // 用于存储错误码

	if (class) {	// 如果设备属于某个类
		error = device_add_attributes(dev, class->dev_attrs);	// 添加该类的属性
		if (error)	// 如果添加失败
			return error;	// 返回错误码
	}

	if (type) {	// 如果设备有指定类型
		error = device_add_groups(dev, type->groups);	// 添加该类型的属性组
		if (error)	// 如果添加失败
			goto err_remove_class_attrs;	// 跳转到错误处理代码，移除已添加的类属性
	}

	error = device_add_groups(dev, dev->groups);	// 添加设备自身定义的属性组
	if (error)	// 如果添加失败
		goto err_remove_type_groups;	// 跳转到错误处理代码，移除已添加的类型属性组

	return 0;	// 所有属性和属性组添加成功，返回 0

 err_remove_type_groups:	// 处理添加设备属性组失败的情况
	if (type)
		device_remove_groups(dev, type->groups);	// 移除已添加的类型属性组
 err_remove_class_attrs:	// 处理添加类属性失败的情况
	if (class)
		device_remove_attributes(dev, class->dev_attrs);	// 移除已添加的类属性

	return error;	// 返回错误码
}

/**
 * device_remove_attrs - 从设备中移除所有属性和属性组
 * @dev: 目标设备
 *
 * 此函数负责从设备中移除所有关联的属性和属性组，包括设备自定义的属性组、设备类型定义的属性组
 * 以及设备所属类定义的属性。这是设备清理过程的一部分，确保设备从 sysfs 中被彻底清除。
 */
static void device_remove_attrs(struct device *dev)
{
	struct class *class = dev->class;	// 获取设备所属的类
	struct device_type *type = dev->type;	// 获取设备的类型

	device_remove_groups(dev, dev->groups);	// 移除设备自定义的属性组

	if (type)	// 如果设备有指定的类型
		device_remove_groups(dev, type->groups);	// 移除设备类型定义的属性组

	if (class)	// 如果设备属于某个类
		device_remove_attributes(dev, class->dev_attrs);	// 移除设备类定义的属性
}

/**
 * show_dev - 显示设备的主次设备号
 * @dev: 指向设备结构的指针
 * @attr: 设备属性结构
 * @buf: 用户空间提供的缓冲区，用于存放显示内容
 *
 * 此函数用于输出设备的主次设备号到提供的缓冲区中。主次设备号是用于标识设备文件的唯一标识符，
 * 通常用于块设备和字符设备。输出格式依赖于 print_dev_t 函数的实现。
 * 返回: 写入缓冲区的字节数。
 */
static ssize_t show_dev(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return print_dev_t(buf, dev->devt);	// 调用 print_dev_t 函数输出设备号到 buf
}

/**
 * devt_attr - 设备属性定义
 *
 * 定义一个名为 "dev" 的设备属性。该属性是只读的（S_IRUGO），显示函数为 show_dev，
 * 没有提供修改函数，因此是只读属性。
 */
static struct device_attribute devt_attr =
	__ATTR(dev, S_IRUGO, show_dev, NULL);	// 创建设备属性，绑定 show_dev 函数，无写入函数

/* kset to create /sys/devices/  */
/* 定义一个全局变量 devices_kset，用于创建 sysfs 中的 /sys/devices/ 目录 */
struct kset *devices_kset;

/**
 * device_create_file - create sysfs attribute file for device.
 * @dev: device.
 * @attr: device attribute descriptor.
 */
/**
 * device_create_file - 为设备创建 sysfs 属性文件
 * @dev: 目标设备
 * @attr: 设备属性描述符
 *
 * 此函数为指定的设备创建一个 sysfs 属性文件。sysfs 文件系统允许内核暴露设备、驱动程序
 * 和其他内核结构的信息给用户空间，通过类似文件系统的接口。
 * 返回：成功时返回0，失败时返回错误码。
 */
int device_create_file(struct device *dev,
		       const struct device_attribute *attr)
{
	int error = 0;	// 初始化错误变量为0
	if (dev)	// 如果设备指针非空
		error = sysfs_create_file(&dev->kobj, &attr->attr);	// 创建一个 sysfs 文件
	return error;	// 返回操作的结果，0 表示成功，其他表示错误
}

/**
 * device_remove_file - remove sysfs attribute file.
 * @dev: device.
 * @attr: device attribute descriptor.
 */
/**
 * device_remove_file - 从 sysfs 中移除设备的属性文件
 * @dev: 目标设备
 * @attr: 设备属性描述符
 *
 * 此函数用于从 sysfs 文件系统中移除与指定设备关联的属性文件。如果设备有效，
 * 它将调用 sysfs 的移除函数来删除对应的文件，从而撤销之前通过 device_create_file
 * 函数创建的 sysfs 文件。
 */
void device_remove_file(struct device *dev,
			const struct device_attribute *attr)
{
	if (dev)	// 检查设备指针是否非空
		sysfs_remove_file(&dev->kobj, &attr->attr);	// 调用 sysfs 的移除文件函数
}

/**
 * device_create_bin_file - create sysfs binary attribute file for device.
 * @dev: device.
 * @attr: device binary attribute descriptor.
 */
/**
 * device_create_bin_file - 为设备创建二进制属性文件
 * @dev: 目标设备
 * @attr: 设备的二进制属性描述符
 *
 * 此函数为指定的设备在 sysfs 中创建一个二进制属性文件。这种属性文件允许进行二进制数据的读写，
 * 常用于需要传输大量数据或者非文本格式数据的场景。
 * 返回：成功时返回 0，失败时返回错误码。
 */
int device_create_bin_file(struct device *dev,
			   const struct bin_attribute *attr)
{
	int error = -EINVAL;	// 初始化错误码为 -EINVAL，表示无效参数
	if (dev)	// 检查设备指针是否非空
		error = sysfs_create_bin_file(&dev->kobj, attr);	// 调用 sysfs 函数创建二进制文件
	return error;	// 返回操作结果
}
EXPORT_SYMBOL_GPL(device_create_bin_file);

/**
 * device_remove_bin_file - remove sysfs binary attribute file
 * @dev: device.
 * @attr: device binary attribute descriptor.
 */
/**
 * device_remove_bin_file - 从 sysfs 中移除设备的二进制属性文件
 * @dev: 目标设备
 * @attr: 设备的二进制属性描述符
 *
 * 此函数从 sysfs 中移除与指定设备关联的二进制属性文件。当不再需要该属性文件或设备被注销时调用。
 */
void device_remove_bin_file(struct device *dev,
			    const struct bin_attribute *attr)
{
	if (dev)	// 检查设备指针是否非空
		sysfs_remove_bin_file(&dev->kobj, attr);	// 调用 sysfs 函数移除二进制文件
}
EXPORT_SYMBOL_GPL(device_remove_bin_file);

/**
 * device_schedule_callback_owner - helper to schedule a callback for a device
 * @dev: device.
 * @func: callback function to invoke later.
 * @owner: module owning the callback routine
 *
 * Attribute methods must not unregister themselves or their parent device
 * (which would amount to the same thing).  Attempts to do so will deadlock,
 * since unregistration is mutually exclusive with driver callbacks.
 *
 * Instead methods can call this routine, which will attempt to allocate
 * and schedule a workqueue request to call back @func with @dev as its
 * argument in the workqueue's process context.  @dev will be pinned until
 * @func returns.
 *
 * This routine is usually called via the inline device_schedule_callback(),
 * which automatically sets @owner to THIS_MODULE.
 *
 * Returns 0 if the request was submitted, -ENOMEM if storage could not
 * be allocated, -ENODEV if a reference to @owner isn't available.
 *
 * NOTE: This routine won't work if CONFIG_SYSFS isn't set!  It uses an
 * underlying sysfs routine (since it is intended for use by attribute
 * methods), and if sysfs isn't available you'll get nothing but -ENOSYS.
 */
/**
 * device_schedule_callback_owner - 为设备安排一个回调函数
 * @dev: 设备。
 * @func: 稍后要调用的回调函数。
 * @owner: 拥有回调例程的模块
 *
 * 属性方法不应注销自身或其父设备（这两者归根到底是一回事）。尝试这样做会导致死锁，
 * 因为注销操作与驱动回调是互斥的。
 *
 * 作为替代，属性方法可以调用此函数，该函数会尝试分配并调度一个工作队列请求，
 * 在工作队列的进程上下文中用 @dev 作为参数回调 @func。在 @func 返回之前，@dev 将被固定。
 *
 * 这个例程通常通过内联函数 device_schedule_callback() 调用，
 * 它自动将 @owner 设置为 THIS_MODULE。
 *
 * 如果请求被提交，则返回 0；如果无法分配存储，则返回 -ENOMEM；
 * 如果无法获得对 @owner 的引用，则返回 -ENODEV。
 *
 * 注意：如果没有设置 CONFIG_SYSFS，这个例程将无法工作！它使用一个底层的 sysfs 例程
 *（因为它是为属性方法设计的），如果没有 sysfs，你将得到 -ENOSYS。
 */
int device_schedule_callback_owner(struct device *dev,
		void (*func)(struct device *), struct module *owner)
{
	return sysfs_schedule_callback(&dev->kobj,	// 在 sysfs 中调度一个回调
			(void (*)(void *)) func, dev, owner);	// 将 func 强制转换为接受 void* 参数的函数，并传入设备和模块所有者
}
EXPORT_SYMBOL_GPL(device_schedule_callback_owner);

/**
 * klist_children_get - 增加设备的引用计数
 * @n: klist 节点，表示一个设备
 *
 * 此函数通过获取与 klist 节点关联的设备的设备私有数据，
 * 并对该设备调用 get_device() 来增加设备的引用计数。
 * 这确保了在处理过程中设备对象保持有效。
 */
static void klist_children_get(struct klist_node *n)
{
	struct device_private *p = to_device_private_parent(n);	// 获取设备的私有数据
	struct device *dev = p->device;	// 从私有数据中获取设备指针

	get_device(dev);	// 增加设备的引用计数
}

/**
 * klist_children_put - 减少设备的引用计数
 * @n: klist 节点，表示一个设备
 *
 * 此函数通过获取与 klist 节点关联的设备的设备私有数据，
 * 并对该设备调用 put_device() 来减少设备的引用计数。
 * 当引用计数降到零时，可能会触发设备的释放操作。
 */
static void klist_children_put(struct klist_node *n)
{
	struct device_private *p = to_device_private_parent(n);	// 获取设备的私有数据
	struct device *dev = p->device;	// 从私有数据中获取设备指针

	put_device(dev);	// 减少设备的引用计数
}

/**
 * device_initialize - init device structure.
 * @dev: device.
 *
 * This prepares the device for use by other layers by initializing
 * its fields.
 * It is the first half of device_register(), if called by
 * that function, though it can also be called separately, so one
 * may use @dev's fields. In particular, get_device()/put_device()
 * may be used for reference counting of @dev after calling this
 * function.
 *
 * NOTE: Use put_device() to give up your reference instead of freeing
 * @dev directly once you have called this function.
 */
/**
 * device_initialize - 初始化设备结构
 * @dev: 需要初始化的设备
 *
 * 此函数为设备的其他层次使用做准备，通过初始化其字段。
 * 它是 device_register() 的第一部分，如果由该函数调用的话，
 * 但它也可以单独调用，以便在调用此函数后使用 @dev 的字段。
 * 特别是，调用此函数后，可以使用 get_device()/put_device() 
 * 对 @dev 进行引用计数。
 *
 * 注意：一旦你调用了此函数，使用 put_device() 来放弃你的引用，
 * 而不是直接释放 @dev。
 */
void device_initialize(struct device *dev)
{
	dev->kobj.kset = devices_kset;  // 将设备的 kobject 加入到全局的设备 kset 中
	kobject_init(&dev->kobj, &device_ktype);  // 初始化设备的 kobject
	INIT_LIST_HEAD(&dev->dma_pools);  // 初始化设备的 DMA 池链表头
	init_MUTEX(&dev->sem);  // 初始化设备的信号量
	spin_lock_init(&dev->devres_lock);  // 初始化设备资源锁
	INIT_LIST_HEAD(&dev->devres_head);  // 初始化设备资源链表头
	device_init_wakeup(dev, 0);  // 初始化设备的唤醒功能
	device_pm_init(dev);  // 初始化设备的电源管理
	set_dev_node(dev, -1);  // 设置设备的节点编号为 -1，表示未指定
}

#ifdef CONFIG_SYSFS_DEPRECATED
/**
 * get_device_parent - 获取设备在 sysfs 中的父 kobject
 * @dev: 当前设备
 * @parent: 当前设备的父设备
 *
 * 此函数用于确定设备在 sysfs 中的父目录。
 * - 如果设备属于某个类并且没有父设备或其父设备不属于同一个类，
 *   设备将位于 /sys/class/<classname>/ 下。
 * - 如果设备有父设备，且该父设备与当前设备属于同一类，则保持其父设备。
 * 如果以上条件都不符合，返回 NULL。
 */
static struct kobject *get_device_parent(struct device *dev,
					 struct device *parent)
{
	/* class devices without a parent live in /sys/class/<classname>/ */
	if (dev->class && (!parent || parent->class != dev->class))
		return &dev->class->p->class_subsys.kobj;	// 返回类的 kobject，位于 /sys/class/<classname>/
	/* all other devices keep their parent */
	else if (parent)
		return &parent->kobj;	// 返回父设备的 kobject

	return NULL;	// 无有效父设备，返回 NULL
}

/**
 * cleanup_device_parent - 清理设备父对象
 * @dev: 设备
 *
 * 当 CONFIG_SYSFS_DEPRECATED 定义时，此函数不执行任何操作，作为占位符。
 */
static inline void cleanup_device_parent(struct device *dev) {}
/**
 * cleanup_glue_dir - 清理粘合目录
 * @dev: 设备
 * @glue_dir: 粘合目录的 kobject
 *
 * 当 CONFIG_SYSFS_DEPRECATED 定义时，此函数不执行任何操作，用于兼容旧的 sysfs 结构。
 */
static inline void cleanup_glue_dir(struct device *dev,
				    struct kobject *glue_dir) {}
#else
/**
 * virtual_device_parent - 获取或创建虚拟设备的父目录 kobject
 * @dev: 需要关联的设备（此参数在当前实现中未使用）
 *
 * 此函数检查是否已经存在一个名为 "virtual" 的 kobject，如果不存在，则创建一个。
 * 这个目录用于在 sysfs 中组织那些没有实际硬件对应的虚拟设备。
 * 返回：指向 "virtual" 目录 kobject 的指针。
 */
static struct kobject *virtual_device_parent(struct device *dev)
{
	static struct kobject *virtual_dir = NULL;	// 静态变量，用于存储虚拟目录的 kobject

	if (!virtual_dir)	// 如果虚拟目录的 kobject 尚未创建
		virtual_dir = kobject_create_and_add("virtual",	// 创建并添加名为 "virtual" 的 kobject
						     &devices_kset->kobj);	// 父目录为 devices_kset

	return virtual_dir;	// 返回虚拟目录的 kobject
}

/**
 * get_device_parent - 获取设备在 sysfs 中的父目录
 * @dev: 需要获取父目录的设备
 * @parent: 设备的父设备
 *
 * 此函数用于确定设备在 sysfs 中的父目录。这个父目录可能是虚拟目录、设备的直接父设备或类设备目录。
 * 返回：指向父目录 kobject 的指针，如果无法创建或查找父目录则返回 NULL。
 */
static struct kobject *get_device_parent(struct device *dev,
					 struct device *parent)
{
	int retval;	// 用于存储返回值

	if (dev->class) {	// 检查设备是否属于某个类
		static DEFINE_MUTEX(gdp_mutex);	// 定义一个互斥锁
		struct kobject *kobj = NULL;	// 最终的父 kobject
		struct kobject *parent_kobj;	// 设备父 kobject
		struct kobject *k;

		/*
		 * If we have no parent, we live in "virtual".
		 * Class-devices with a non class-device as parent, live
		 * in a "glue" directory to prevent namespace collisions.
		 */
		if (parent == NULL)
			parent_kobj = virtual_device_parent(dev);	// 如果没有父设备或父设备不是类设备，使用虚拟目录作为父目录
		else if (parent->class)	 // 如果父设备也有类
			return &parent->kobj;	// 直接返回父设备的 kobject
		else
			parent_kobj = &parent->kobj;	// 使用父设备的 kobject

		mutex_lock(&gdp_mutex);	// 锁定互斥锁

		/* find our class-directory at the parent and reference it */
		// 在父目录中查找对应的类目录并引用它
		spin_lock(&dev->class->p->class_dirs.list_lock);
		list_for_each_entry(k, &dev->class->p->class_dirs.list, entry)
			if (k->parent == parent_kobj) {
				kobj = kobject_get(k);	// 引用找到的类目录 kobject
				break;
			}
		spin_unlock(&dev->class->p->class_dirs.list_lock);
		if (kobj) {	// 如果找到了类目录，解锁互斥锁并返回
			mutex_unlock(&gdp_mutex);
			return kobj;
		}

		/* or create a new class-directory at the parent device */
		// 如果没有找到，创建一个新的类目录
		k = kobject_create();
		if (!k) {
			mutex_unlock(&gdp_mutex);
			return NULL;	// 创建失败，返回 NULL
		}
		k->kset = &dev->class->p->class_dirs;
		retval = kobject_add(k, parent_kobj, "%s", dev->class->name);
		if (retval < 0) {	// 添加失败，释放新创建的 kobject
			mutex_unlock(&gdp_mutex);
			kobject_put(k);
			return NULL;
		}
		/* do not emit an uevent for this simple "glue" directory */
		// 新目录创建成功，解锁并返回
		mutex_unlock(&gdp_mutex);
		return k;
	}

	if (parent)
		return &parent->kobj;	// 如果设备有直接父设备，返回其 kobject
	return NULL;	// 否则返回 NULL
}

/**
 * cleanup_glue_dir - 清理与设备关联的“粘合”目录
 * @dev: 被处理的设备
 * @glue_dir: 被认为是“粘合”目录的 kobject
 *
 * 这个函数检查设备是否存在于一个被称为“粘合”目录的临时目录中。
 * 如果是，并且满足特定条件，该函数将释放这个目录的 kobject。
 * 这通常发生在设备从 sysfs 中移除时。
 */
static void cleanup_glue_dir(struct device *dev, struct kobject *glue_dir)
{
	/* see if we live in a "glue" directory */
	// 检查是否存在“粘合”目录，以及设备是否有类，以及该目录是否为设备类目录的一部分
	if (!glue_dir || !dev->class ||
	    glue_dir->kset != &dev->class->p->class_dirs)
		return;	// 如果条件不满足，直接返回

	kobject_put(glue_dir);	// 释放这个“粘合”目录的 kobject
}

/**
 * cleanup_device_parent - 清理设备的父目录
 * @dev: 被处理的设备
 *
 * 这个函数用于清理与设备关联的父目录，特别是那些临时创建的“粘合”目录。
 * 它调用 cleanup_glue_dir 函数，传递设备的父 kobject 作为参数。
 */
static void cleanup_device_parent(struct device *dev)
{
	// 调用 cleanup_glue_dir 清理设备的父目录
	cleanup_glue_dir(dev, dev->kobj.parent);
}
#endif

/**
 * setup_parent - 为设备设置父目录
 * @dev: 需要设置父目录的设备
 * @parent: 设备的预期父设备
 *
 * 此函数通过调用 get_device_parent 来确定设备在 sysfs 中的适当父目录，
 * 并将其设置为设备的父 kobject。这一步是为了正确地将设备组织在 sysfs 中。
 */
static void setup_parent(struct device *dev, struct device *parent)
{
	struct kobject *kobj;	// 定义一个指向 kobject 的指针
	kobj = get_device_parent(dev, parent);	// 获取设备的适当父目录 kobject
	if (kobj)	// 如果成功获取到父 kobject
		dev->kobj.parent = kobj;	// 设置设备的父 kobject
}

/**
 * device_add_class_symlinks - 为设备添加类别符号链接
 * @dev: 设备实例
 *
 * 如果设备有一个类别，则为该设备在 sysfs 中添加必要的符号链接。
 * 链接的创建依赖于设备是否已分配给一个类别，以及设备的父设备和分区状态。
 * 返回：成功返回0，失败返回错误代码。
 */
static int device_add_class_symlinks(struct device *dev)
{
	int error;	// 用于存储错误代码

	if (!dev->class)	// 如果设备没有类别，则无需创建链接，直接返回成功
		return 0;

	// 创建从设备到其类别子系统的符号链接
	error = sysfs_create_link(&dev->kobj,
				  &dev->class->p->class_subsys.kobj,
				  "subsystem");
	if (error)	// 如果创建链接失败，直接返回错误代码
		goto out;	// 跳转到错误处理代码

// 以下代码只在配置了旧版 sysfs 时编译
#ifdef CONFIG_SYSFS_DEPRECATED
	/* stacked class devices need a symlink in the class directory */
	// 以下是针对旧版 sysfs 结构的特殊处理
	// 如果设备的父对象不是类别子系统且设备不是分区，则在类别目录中创建一个指向设备的链接
	if (dev->kobj.parent != &dev->class->p->class_subsys.kobj &&
	    device_is_not_partition(dev)) {
		error = sysfs_create_link(&dev->class->p->class_subsys.kobj,
					  &dev->kobj, dev_name(dev));
		if (error)	// 如果创建链接失败
			goto out_subsys;	// 跳转到子系统链接的清理代码
	}

	// 如果设备有父设备且不是分区
	if (dev->parent && device_is_not_partition(dev)) {
		struct device *parent = dev->parent;
		char *class_name;

		/*
		 * stacked class devices have the 'device' link
		 * pointing to the bus device instead of the parent
		 */
		// 对于层叠的类设备，'device' 链接指向总线设备而不是父设备
		// 查找设备的最顶层非类设备的父设备
		while (parent->class && !parent->bus && parent->parent)
			parent = parent->parent;

		// 创建一个从设备指向其父设备的符号链接
		error = sysfs_create_link(&dev->kobj,
					  &parent->kobj,
					  "device");
		if (error)	// 如果创建链接失败
			goto out_busid;	// 跳转到 bus id 链接的清理代码

		// 生成一个类名并为设备在其父设备的目录中创建一个符号链接
		class_name = make_class_name(dev->class->name,
						&dev->kobj);
		if (class_name)
			error = sysfs_create_link(&dev->parent->kobj,
						&dev->kobj, class_name);
		kfree(class_name);	// 释放临时分配的类名字符串
		if (error)	// 如果创建链接失败
			goto out_device;	// 跳转到设备链接的清理代码
	}
	return 0;

out_device:
	if (dev->parent && device_is_not_partition(dev))
		sysfs_remove_link(&dev->kobj, "device");	// 清理设备链接
out_busid:
	if (dev->kobj.parent != &dev->class->p->class_subsys.kobj &&
	    device_is_not_partition(dev))
		sysfs_remove_link(&dev->class->p->class_subsys.kobj,
				  dev_name(dev));	// 清理 bus id 链接
#else
	/* link in the class directory pointing to the device */
	// 对于非旧版 sysfs 结构，创建一个指向设备的类目录链接
	error = sysfs_create_link(&dev->class->p->class_subsys.kobj,
				  &dev->kobj, dev_name(dev));
	if (error)	// 如果创建链接失败
		goto out_subsys;	// 跳转到子系统链接的清理代码

	// 如果设备有父设备且不是分区，创建从设备到父设备的链接
	if (dev->parent && device_is_not_partition(dev)) {
		error = sysfs_create_link(&dev->kobj, &dev->parent->kobj,
					  "device");
		if (error)	// 如果创建链接失败
			goto out_busid;	// 跳转到 bus id 链接的清理代码
	}
	return 0;	// 成功返回0

out_busid:
	sysfs_remove_link(&dev->class->p->class_subsys.kobj, dev_name(dev));	// 清理 class 目录链接
#endif

out_subsys:
	sysfs_remove_link(&dev->kobj, "subsystem");	// 清理子系统链接
out:
	return error;	// 返回错误代码
}

/**
 * device_remove_class_symlinks - 清除设备类相关的符号链接
 * @dev: 需要清除符号链接的设备
 *
 * 此函数负责清除之前为设备创建的所有类相关的符号链接。这包括链接到设备类别的子系统，
 * 设备到父设备的链接，以及配置为旧版 sysfs 时可能创建的其他链接。
 */
static void device_remove_class_symlinks(struct device *dev)
{
	// 如果设备没有类别，则无需执行任何操作
	if (!dev->class)
		return;

// 以下代码只在配置了旧版 sysfs 时编译
#ifdef CONFIG_SYSFS_DEPRECATED
	// 如果设备有父设备且设备不是分区
	if (dev->parent && device_is_not_partition(dev)) {
		char *class_name;

		// 创建设备类名字符串
		class_name = make_class_name(dev->class->name, &dev->kobj);
		if (class_name) {
			// 删除设备在其父设备的 sysfs 目录中的类名链接
			sysfs_remove_link(&dev->parent->kobj, class_name);
			kfree(class_name);	// 释放创建的类名字符串
		}
		// 删除从设备到其父设备的符号链接
		sysfs_remove_link(&dev->kobj, "device");
	}

	// 如果设备的父 kobject 不是类别子系统且设备不是分区
	if (dev->kobj.parent != &dev->class->p->class_subsys.kobj &&
	    device_is_not_partition(dev))
		// 删除在类别目录中创建的指向设备的链接
		sysfs_remove_link(&dev->class->p->class_subsys.kobj,
				  dev_name(dev));
#else
	// 对于非旧版 sysfs 结构，执行以下操作：
	// 如果设备有父设备且不是分区，删除从设备指向其父设备的符号链接
	if (dev->parent && device_is_not_partition(dev))
		sysfs_remove_link(&dev->kobj, "device");

	// 删除从设备类别的子系统指向设备的符号链接
	sysfs_remove_link(&dev->class->p->class_subsys.kobj, dev_name(dev));
#endif
	// 无论配置如何，都删除从设备指向其类别子系统的符号链接
	sysfs_remove_link(&dev->kobj, "subsystem");
}

/**
 * dev_set_name - set a device name
 * @dev: device
 * @fmt: format string for the device's name
 */
/**
 * dev_set_name - 设置设备名称
 * @dev: 设备实例
 * @fmt: 设备名称的格式化字符串
 *
 * 此函数用于设置设备的名称，使用类似 printf 的格式化功能。它接受一个格式字符串和可变数量的参数，
 * 并将其传递给 kobject_set_name_vargs 来实际设置设备的 kobject 名称。
 * 返回：成功时返回0，失败时返回错误代码。
 */
int dev_set_name(struct device *dev, const char *fmt, ...)
{
	va_list vargs;	// 定义一个用于存储可变参数列表的变量
	int err;	// 用于存储函数执行的结果

	va_start(vargs, fmt);	// 初始化 vargs，使其指向参数列表中的第一个可选参数
	// 调用 kobject_set_name_vargs 函数设置设备的 kobject 名称
	err = kobject_set_name_vargs(&dev->kobj, fmt, vargs);
	va_end(vargs);	// 清理 vargs，结束可变参数的获取
	return err;	// 返回操作结果，0 表示成功，负数表示错误代码
}
EXPORT_SYMBOL_GPL(dev_set_name);

/**
 * device_to_dev_kobj - select a /sys/dev/ directory for the device
 * @dev: device
 *
 * By default we select char/ for new entries.  Setting class->dev_obj
 * to NULL prevents an entry from being created.  class->dev_kobj must
 * be set (or cleared) before any devices are registered to the class
 * otherwise device_create_sys_dev_entry() and
 * device_remove_sys_dev_entry() will disagree about the the presence
 * of the link.
 */
/**
 * device_to_dev_kobj - 选择设备的/sys/dev/目录
 * @dev: 设备
 *
 * 默认情况下，我们为新条目选择char/目录。如果class->dev_kobj设置为NULL，
 * 则不会创建条目。class->dev_kobj必须在向类注册任何设备之前设置（或清除），
 * 否则device_create_sys_dev_entry()和device_remove_sys_dev_entry()
 * 将对链接的存在性存在分歧。
 */
static struct kobject *device_to_dev_kobj(struct device *dev)
{
	struct kobject *kobj;

	if (dev->class)
		// 如果设备有类，则使用该类的dev_kobj
		kobj = dev->class->dev_kobj;
	else
		// 否则，默认使用系统提供的字符设备目录对象
		kobj = sysfs_dev_char_kobj;

	return kobj;
}

/**
 * 创建设备的sysfs条目
 */
static int device_create_sys_dev_entry(struct device *dev)
{
	// 获取设备的kobject
	struct kobject *kobj = device_to_dev_kobj(dev);
	int error = 0;
	char devt_str[15];

	// 如果kobject存在
	if (kobj) {
		// 格式化设备号为字符串
		format_dev_t(devt_str, dev->devt);
		// 创建一个到设备kobject的链接
		error = sysfs_create_link(kobj, &dev->kobj, devt_str);
	}

	return error;
}

/**
 * 删除设备的sysfs条目
 */
static void device_remove_sys_dev_entry(struct device *dev)
{
	// 获取设备的kobject
	struct kobject *kobj = device_to_dev_kobj(dev);
	char devt_str[15];

	if (kobj) {	// 如果kobject存在
		// 格式化设备号为字符串
		format_dev_t(devt_str, dev->devt);
		sysfs_remove_link(kobj, devt_str);
	}
}

/**
 * 初始化设备私有数据结构
 */
int device_private_init(struct device *dev)
{
	// 分配私有数据结构
	dev->p = kzalloc(sizeof(*dev->p), GFP_KERNEL);
	if (!dev->p)
		return -ENOMEM;	// 内存分配失败，返回错误
	dev->p->device = dev;	// 设置私有数据中的设备指针
	// 初始化子设备链表
	klist_init(&dev->p->klist_children, klist_children_get,
		   klist_children_put);
	return 0;
}

/**
 * device_add - add device to device hierarchy.
 * @dev: device.
 *
 * This is part 2 of device_register(), though may be called
 * separately _iff_ device_initialize() has been called separately.
 *
 * This adds @dev to the kobject hierarchy via kobject_add(), adds it
 * to the global and sibling lists for the device, then
 * adds it to the other relevant subsystems of the driver model.
 *
 * NOTE: _Never_ directly free @dev after calling this function, even
 * if it returned an error! Always use put_device() to give up your
 * reference instead.
 */
/**
 * device_add - 将设备添加到设备层次结构中。
 * @dev: 需要添加的设备。
 *
 * 这是 device_register() 的第二部分，如果之前已单独调用了 device_initialize()，也可以单独调用此函数。
 * 该函数通过 kobject_add() 将 @dev 添加到 kobject 层次结构中，然后将其添加到全局和兄弟设备列表，
 * 最后将其添加到驱动模型的其他相关子系统中。
 *
 * 注意：即使此函数返回错误，_绝对不要_ 直接释放 @dev！总是使用 put_device() 来放弃你的引用。
 */
int device_add(struct device *dev)
{
	struct device *parent = NULL; // 父设备指针
	struct class_interface *class_intf; // 类接口指针
	int error = -EINVAL; // 默认错误代码

	dev = get_device(dev); // 增加设备的引用计数
	if (!dev) // 如果获取设备失败，跳转到处理结束
		goto done;

	if (!dev->p) { // 如果设备的私有数据未初始化
		error = device_private_init(dev); // 初始化设备私有数据
		if (error) // 如果初始化失败则结束
			goto done;
	}

	/*
	 * for statically allocated devices, which should all be converted
	 * some day, we need to initialize the name. We prevent reading back
	 * the name, and force the use of dev_name()
	 */
	if (dev->init_name) {	// 如果设备有初始化名称，设置设备名称
		dev_set_name(dev, "%s", dev->init_name);	// 设置设备名
		dev->init_name = NULL;	// 清除初始化名
	}

	// 检查设备名称是否设置成功
	if (!dev_name(dev)) {	// 如果设备名为空
		error = -EINVAL;	// 设置错误代码
		goto name_error;	// 跳转到错误处理
	}

	pr_debug("device: '%s': %s\n", dev_name(dev), __func__);	// 打印设备添加信息

	parent = get_device(dev->parent);	// 获取并增加父设备的引用计数
	setup_parent(dev, parent);	// 设置设备的父设备

	/* use parent numa_node */
	if (parent)	// 使用父设备的 numa_node
		set_dev_node(dev, dev_to_node(parent));

	/* first, register with generic layer. */
	/* we require the name to be set before, and pass NULL */
	// 首先，向通用层注册。
	// 首先，在 kobject 层次结构中注册设备
	error = kobject_add(&dev->kobj, dev->kobj.parent, NULL);	// 添加 kobject
	if (error)	// 如果添加失败
		goto Error;

	/* notify platform of device entry */
	// 通知平台有设备添加
	if (platform_notify)	// 通知平台设备添加事件
		platform_notify(dev);

	error = device_create_file(dev, &uevent_attr);	// 创建设备的 uevent 文件
	if (error)
		goto attrError;

	if (MAJOR(dev->devt)) {	// 如果设备有主设备号，创建相关的 sysfs 文件
		error = device_create_file(dev, &devt_attr);	// 创建设备号文件
		if (error)
			goto ueventattrError;

		error = device_create_sys_dev_entry(dev);	// 创建系统设备入口
		if (error)
			goto devtattrError;

		devtmpfs_create_node(dev);	// 在 devtmpfs 中创建节点
	}

	error = device_add_class_symlinks(dev);	// 添加类符号链接
	if (error)
		goto SymlinkError;
	error = device_add_attrs(dev);	// 添加设备属性
	if (error)
		goto AttrsError;
	error = bus_add_device(dev);	// 将设备添加到其所属的总线
	if (error)
		goto BusError;
	error = dpm_sysfs_add(dev);	// 添加设备到电源管理系统
	if (error)
		goto DPMError;
	device_pm_add(dev);	// 添加设备到电源管理

	/* Notify clients of device addition.  This call must come
	 * after dpm_sysf_add() and before kobject_uevent().
	 */
	// 通知客户端设备已添加。这个调用必须在 dpm_sysfs_add() 之后，并在 kobject_uevent() 之前。
	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_ADD_DEVICE, dev);	// 在 kobject_uevent() 前通知客户端设备已添加

	kobject_uevent(&dev->kobj, KOBJ_ADD);	// 发送添加设备的 uevent
	bus_probe_device(dev);	// 探测设备
	if (parent)
		klist_add_tail(&dev->p->knode_parent,
			       &parent->p->klist_children);

	if (dev->class) {
		mutex_lock(&dev->class->p->class_mutex);	// 锁定类互斥锁
		/* tie the class to the device */
		klist_add_tail(&dev->knode_class,
			       &dev->class->p->class_devices);	// 将类与设备关联

		/* notify any interfaces that the device is here */
		// 通知任何接口设备已存在
		list_for_each_entry(class_intf,
				    &dev->class->p->class_interfaces, node)
			if (class_intf->add_dev)
				class_intf->add_dev(dev, class_intf);
		mutex_unlock(&dev->class->p->class_mutex);
	}
done:
	put_device(dev);	// 减少设备的引用计数
	return error;
// 错误处理部分，逐步回滚已执行的操作
 DPMError:
	bus_remove_device(dev);
 BusError:
	device_remove_attrs(dev);
 AttrsError:
	device_remove_class_symlinks(dev);
 SymlinkError:
	if (MAJOR(dev->devt))
		devtmpfs_delete_node(dev);
	if (MAJOR(dev->devt))
		device_remove_sys_dev_entry(dev);
 devtattrError:
	if (MAJOR(dev->devt))
		device_remove_file(dev, &devt_attr);
 ueventattrError:
	device_remove_file(dev, &uevent_attr);
 attrError:
	kobject_uevent(&dev->kobj, KOBJ_REMOVE);
	kobject_del(&dev->kobj);
 Error:
	cleanup_device_parent(dev);
	if (parent)
		put_device(parent);
name_error:
	kfree(dev->p);
	dev->p = NULL;
	goto done;
}

/**
 * device_register - register a device with the system.
 * @dev: pointer to the device structure
 *
 * This happens in two clean steps - initialize the device
 * and add it to the system. The two steps can be called
 * separately, but this is the easiest and most common.
 * I.e. you should only call the two helpers separately if
 * have a clearly defined need to use and refcount the device
 * before it is added to the hierarchy.
 *
 * NOTE: _Never_ directly free @dev after calling this function, even
 * if it returned an error! Always use put_device() to give up the
 * reference initialized in this function instead.
 */
/**
 * device_register - 向系统注册一个设备。
 * @dev: 指向设备结构体的指针。
 *
 * 这个过程分为两个清晰的步骤——初始化设备和将其添加到系统中。
 * 这两个步骤可以分开调用，但这种方式是最简单和最常见的。
 * 即，只有在您需要在设备被添加到层次结构之前使用和引用计数设备时，
 * 才应该分别调用这两个辅助函数。
 *
 * 注意：在调用此函数后，即使返回错误，_永远不要_ 直接释放 @dev！
 * 应该始终使用 put_device() 放弃此函数中初始化的引用。
 */
int device_register(struct device *dev)
{
	device_initialize(dev);	// 初始化设备
	return device_add(dev);	// 将设备添加到系统中
}

/**
 * get_device - increment reference count for device.
 * @dev: device.
 *
 * This simply forwards the call to kobject_get(), though
 * we do take care to provide for the case that we get a NULL
 * pointer passed in.
 */
/**
 * get_device - 为设备增加引用计数。
 * @dev: 设备。
 *
 * 这个函数简单地将调用转发到 kobject_get()，尽管我们确实考虑了
 * 可能会传入一个 NULL 指针的情况。
 */
struct device *get_device(struct device *dev)
{
	// 如果设备不是 NULL，调用 kobject_get 增加设备的引用计数并返回设备；否则返回 NULL
	return dev ? to_dev(kobject_get(&dev->kobj)) : NULL;
}

/**
 * put_device - decrement reference count.
 * @dev: device in question.
 */
/**
 * put_device - 减少引用计数。
 * @dev: 相关设备。
 *
 * 这个函数用于在设备对象上减少引用计数。当引用计数减至0时，会触发设备对象的释放操作。
 * 通常与 get_device() 配对使用，确保设备在使用期间不被释放。
 */
void put_device(struct device *dev)
{
	/* might_sleep(); */
	// 这个注释的函数调用表示在可能的情况下，此函数可能会使进程休眠。通常用于提醒开发者注意此函数
	// 在某些情况下可能会阻塞。这里被注释掉可能是因为在最终实现中确认了该函数不会导致休眠，或者
	// 考虑到实际使用环境不需担心此问题。
	if (dev)	// 检查传入的设备指针是否为非空
		kobject_put(&dev->kobj);	// 调用 kobject_put 减少设备的 kobject 的引用计数
}

/**
 * device_del - delete device from system.
 * @dev: device.
 *
 * This is the first part of the device unregistration
 * sequence. This removes the device from the lists we control
 * from here, has it removed from the other driver model
 * subsystems it was added to in device_add(), and removes it
 * from the kobject hierarchy.
 *
 * NOTE: this should be called manually _iff_ device_add() was
 * also called manually.
 */
/**
 * device_del - 从系统中删除设备。
 * @dev: 设备。
 *
 * 这是设备注销序列的第一部分。此函数会将设备从我们控制的列表中移除，从在 device_add() 中添加到的
 * 其他驱动模型子系统中移除，并将其从 kobject 层次结构中移除。
 *
 * 注意：只有在 device_add() 也是手动调用的情况下，才应手动调用此函数。
 */
void device_del(struct device *dev)
{
	struct device *parent = dev->parent;	// 获取设备的父设备
	struct class_interface *class_intf;		// 设备类接口变量

	/* Notify clients of device removal.  This call must come
	 * before dpm_sysfs_remove().
	 */
	// 通知客户端设备即将被移除。此调用必须在 dpm_sysfs_remove() 之前。
	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_DEL_DEVICE, dev);
	device_pm_remove(dev);	// 从电源管理中移除设备
	dpm_sysfs_remove(dev);	// 从设备电源管理系统中移除相关的 sysfs 条目
	if (parent)
		klist_del(&dev->p->knode_parent);	// 从父设备的子设备列表中删除
	if (MAJOR(dev->devt)) {
		devtmpfs_delete_node(dev);	// 从 devtmpfs 中删除设备节点
		device_remove_sys_dev_entry(dev);	// 移除系统设备条目
		device_remove_file(dev, &devt_attr);	// 移除设备文件属性
	}
	if (dev->class) {
		device_remove_class_symlinks(dev);	// 移除类符号链接

		mutex_lock(&dev->class->p->class_mutex);	// 锁定设备类的互斥锁
		/* notify any interfaces that the device is now gone */
		// 通知任何接口设备现在已经不存在
		list_for_each_entry(class_intf,
				    &dev->class->p->class_interfaces, node)
			if (class_intf->remove_dev)
				class_intf->remove_dev(dev, class_intf);
		/* remove the device from the class list */
		// 从类设备列表中移除设备
		klist_del(&dev->knode_class);
		mutex_unlock(&dev->class->p->class_mutex);
	}
	device_remove_file(dev, &uevent_attr);	// 移除设备的 uevent 文件
	device_remove_attrs(dev);	// 移除设备属性
	bus_remove_device(dev);	// 从总线中移除设备

	/*
	 * Some platform devices are driven without driver attached
	 * and managed resources may have been acquired.  Make sure
	 * all resources are released.
	 */
	// 有些平台设备在没有驱动的情况下可能被激活，并且可能已获取一些管理资源。确保释放所有资源。
	devres_release_all(dev);	

	/* Notify the platform of the removal, in case they
	 * need to do anything...
	 */
	// 通知平台设备已移除，以防它们需要做些什么...
	if (platform_notify_remove)
		platform_notify_remove(dev);
	kobject_uevent(&dev->kobj, KOBJ_REMOVE);	// 发送设备移除的 kobject 事件
	cleanup_device_parent(dev);	// 清理设备的父级关系
	kobject_del(&dev->kobj);		// 从 kobject 层次结构中删除设备
	put_device(parent);					// 减少父设备的引用计数
}

/**
 * device_unregister - unregister device from system.
 * @dev: device going away.
 *
 * We do this in two parts, like we do device_register(). First,
 * we remove it from all the subsystems with device_del(), then
 * we decrement the reference count via put_device(). If that
 * is the final reference count, the device will be cleaned up
 * via device_release() above. Otherwise, the structure will
 * stick around until the final reference to the device is dropped.
 */
/**
 * device_unregister - 从系统中注销设备。
 * @dev: 即将被移除的设备。
 *
 * 我们像设备注册那样，分两部分来进行设备的注销。首先，我们通过 device_del()
 * 将设备从所有子系统中移除，然后通过 put_device() 减少设备的引用计数。
 * 如果这是最后一个引用计数，设备将通过之前的 device_release() 函数被清理。
 * 否则，该结构将继续存在，直到设备的最后一个引用被放弃。
 */
void device_unregister(struct device *dev)
{
	pr_debug("device: '%s': %s\n", dev_name(dev), __func__);	// 打印调试信息，显示正在注销的设备名和函数名
	device_del(dev);		// 从所有子系统中移除设备
	put_device(dev);		// 减少设备的引用计数
}

/**
 * next_device - 在设备列表中获取下一个设备。
 * @i: 设备列表迭代器。
 *
 * 返回设备列表中的下一个设备，如果没有更多设备，则返回 NULL。
 */
static struct device *next_device(struct klist_iter *i)
{
	struct klist_node *n = klist_next(i);	// 从迭代器获取下一个 klist 节点
	struct device *dev = NULL;	// 初始化设备指针为 NULL
	struct device_private *p;

	if (n) {	// 如果存在下一个节点
		p = to_device_private_parent(n);	// 从节点获取设备私有数据
		dev = p->device;	// 从设备私有数据获取设备实例
	}
	return dev;	// 返回获取到的设备，如果没有更多设备则为 NULL
}

/**
 * device_get_devnode - path of device node file
 * @dev: device
 * @mode: returned file access mode
 * @tmp: possibly allocated string
 *
 * Return the relative path of a possible device node.
 * Non-default names may need to allocate a memory to compose
 * a name. This memory is returned in tmp and needs to be
 * freed by the caller.
 */
/**
 * device_get_devnode - 获取设备节点文件的路径
 * @dev: 设备
 * @mode: 返回的文件访问模式
 * @tmp: 可能分配的字符串
 *
 * 返回可能的设备节点的相对路径。非默认名称可能需要分配内存来组成
 * 一个名称。这部分内存在 tmp 中返回，并且需要由调用者释放。
 */
const char *device_get_devnode(struct device *dev,
			       mode_t *mode, const char **tmp)
{
	char *s;	// 用于替换字符的指针

	*tmp = NULL;	// 初始化 tmp 为空，表示默认不需要分配内存

	/* the device type may provide a specific name */
	// 设备类型可能提供一个特定的名称
	if (dev->type && dev->type->devnode)
		*tmp = dev->type->devnode(dev, mode);	// 调用设备类型的 devnode 方法获取设备名称
	if (*tmp)	// 如果获取到了名称，直接返回
		return *tmp;

	/* the class may provide a specific name */
	// 设备类可能提供一个特定的名称
	if (dev->class && dev->class->devnode)
		*tmp = dev->class->devnode(dev, mode);	// 调用设备类的 devnode 方法获取设备名称
	if (*tmp)	// 如果获取到了名称，直接返回
		return *tmp;

	/* return name without allocation, tmp == NULL */
	// 如果设备名称中不包含字符 '!'，则返回不需要分配的名称，tmp 保持为 NULL
	if (strchr(dev_name(dev), '!') == NULL)
		return dev_name(dev);

	/* replace '!' in the name with '/' */
	// 如果设备名称中包含 '!'，需要替换为 '/'
	*tmp = kstrdup(dev_name(dev), GFP_KERNEL);	// 复制设备名称到新的内存中
	if (!*tmp)	// 如果内存分配失败，返回 NULL
		return NULL;
	while ((s = strchr(*tmp, '!')))	// 查找并替换所有 '!' 字符为 '/'
		s[0] = '/';
	return *tmp;	// 返回修改后的设备名称
}

/**
 * device_for_each_child - device child iterator.
 * @parent: parent struct device.
 * @data: data for the callback.
 * @fn: function to be called for each device.
 *
 * Iterate over @parent's child devices, and call @fn for each,
 * passing it @data.
 *
 * We check the return of @fn each time. If it returns anything
 * other than 0, we break out and return that value.
 */
/**
 * device_for_each_child - 遍历设备的子设备。
 * @parent: 父设备的结构体。
 * @data: 传递给回调函数的数据。
 * @fn: 需要为每个设备调用的函数。
 *
 * 遍历 @parent 的子设备，并为每个子设备调用 @fn，
 * 同时将 @data 传递给它。
 *
 * 我们每次检查 @fn 的返回值。如果它返回的不是 0，
 * 我们将中断循环并返回该值。
 */
int device_for_each_child(struct device *parent, void *data,
			  int (*fn)(struct device *dev, void *data))
{
	struct klist_iter i;	// 子设备列表的迭代器
	struct device *child;	// 用于存储当前迭代的子设备
	int error = 0;		// 错误码初始化为 0，表示没有错误

	if (!parent->p)	// 如果父设备的私有数据不存在，直接返回 0
		return 0;

	klist_iter_init(&parent->p->klist_children, &i);  // 初始化子设备列表的迭代器
	while ((child = next_device(&i)) && !error)  // 遍历所有子设备，直到遇到错误或遍历完成
		error = fn(child, data);  // 对每个子设备调用给定的函数 fn
	klist_iter_exit(&i);  // 清理迭代器，完成迭代过程
	return error;  // 返回最终的错误码
}

/**
 * device_find_child - device iterator for locating a particular device.
 * @parent: parent struct device
 * @data: Data to pass to match function
 * @match: Callback function to check device
 *
 * This is similar to the device_for_each_child() function above, but it
 * returns a reference to a device that is 'found' for later use, as
 * determined by the @match callback.
 *
 * The callback should return 0 if the device doesn't match and non-zero
 * if it does.  If the callback returns non-zero and a reference to the
 * current device can be obtained, this function will return to the caller
 * and not iterate over any more devices.
 */
/**
 * device_find_child - 用于定位特定设备的设备迭代器。
 * @parent: 父设备的结构体
 * @data: 传递给匹配函数的数据
 * @match: 回调函数，用于检查设备
 *
 * 这个函数与上面的 device_for_each_child() 函数相似，但它返回一个“找到”的设备的引用，
 * 供以后使用，这由 @match 回调决定。
 *
 * 如果设备不符合条件，回调应返回 0；如果符合，则返回非零值。如果回调返回非零值，
 * 并且可以获取当前设备的引用，这个函数将返回给调用者，并且不再迭代更多设备。
 */
struct device *device_find_child(struct device *parent, void *data,
				 int (*match)(struct device *dev, void *data))
{
	struct klist_iter i;  // 子设备列表的迭代器
	struct device *child;  // 用于存储当前迭代的子设备

	if (!parent)  // 如果父设备为空，则直接返回 NULL
		return NULL;

	klist_iter_init(&parent->p->klist_children, &i);  // 初始化子设备列表的迭代器
	while ((child = next_device(&i)))  // 遍历所有子设备
		if (match(child, data) && get_device(child))  // 如果找到匹配的设备，并成功增加其引用计数
			break;  // 中断循环
	klist_iter_exit(&i);  // 清理迭代器，完成迭代过程
	return child;  // 返回找到的设备，如果没有找到，则返回 NULL
}

/**
 * devices_init - 初始化设备子系统。
 * 
 * 此函数创建并添加了几个核心 kobject 和 kset，用于管理设备。
 * 它设置了全局的 kset 和设备的顶级 kobject。
 *
 * 返回 0 表示成功，返回 -ENOMEM 表示内存不足错误。
 */
int __init devices_init(void)
{
	// 创建并添加一个名为 "devices" 的 kset，使用 device_uevent_ops 作为其事件操作函数。
	// 这个 kset 用于全局管理所有设备的 kobjects。
	devices_kset = kset_create_and_add("devices", &device_uevent_ops, NULL);
	if (!devices_kset)
		return -ENOMEM;
	// 创建并添加一个名为 "dev" 的顶级 kobject，用于作为其他设备类型 kobject 的父对象。
	dev_kobj = kobject_create_and_add("dev", NULL);
	if (!dev_kobj)
		goto dev_kobj_err;	// 如果创建失败，跳转到错误处理代码
	// 创建并添加一个名为 "block" 的 kobject，作为块设备的父对象，属于 "dev" kobject。
	sysfs_dev_block_kobj = kobject_create_and_add("block", dev_kobj);
	if (!sysfs_dev_block_kobj)
		goto block_kobj_err;	// 如果创建失败，跳转到错误处理代码
	// 创建并添加一个名为 "char" 的 kobject，作为字符设备的父对象，属于 "dev" kobject。
	sysfs_dev_char_kobj = kobject_create_and_add("char", dev_kobj);
	if (!sysfs_dev_char_kobj)
		goto char_kobj_err;	// 如果创建失败，跳转到错误处理代码

	return 0;	// 所有创建操作成功，返回 0

// 错误处理部分，用于逐步撤销之前的操作，确保系统资源不会泄露
 char_kobj_err:
	kobject_put(sysfs_dev_block_kobj);	// 移除并释放 "block" kobject
 block_kobj_err:
	kobject_put(dev_kobj);	// 移除并释放 "dev" kobject
 dev_kobj_err:
	kset_unregister(devices_kset);	 // 注销并释放 "devices" kset
	return -ENOMEM;	// 返回内存不足错误
}

EXPORT_SYMBOL_GPL(device_for_each_child);
EXPORT_SYMBOL_GPL(device_find_child);

EXPORT_SYMBOL_GPL(device_initialize);
EXPORT_SYMBOL_GPL(device_add);
EXPORT_SYMBOL_GPL(device_register);

EXPORT_SYMBOL_GPL(device_del);
EXPORT_SYMBOL_GPL(device_unregister);
EXPORT_SYMBOL_GPL(get_device);
EXPORT_SYMBOL_GPL(put_device);

EXPORT_SYMBOL_GPL(device_create_file);
EXPORT_SYMBOL_GPL(device_remove_file);

// 定义一个名为 root_device 的结构体，用于表示根设备
struct root_device
{
	struct device dev; // 继承自通用的设备结构体，包含设备的基本信息和操作
	struct module *owner; // 指向拥有这个设备的模块的指针
};

// to_root_device 宏用于从 device 结构体获取包含它的 root_device 结构体
// 这是一个典型的容器宏，常用于驱动中从部分结构获取整体结构的指针
#define to_root_device(dev) container_of(dev, struct root_device, dev)

// root_device_release 是一个释放 root_device 结构的函数
// 它被注册到设备结构中，当设备的引用计数降到0时被调用
static void root_device_release(struct device *dev)
{
	kfree(to_root_device(dev));	// 释放包含设备结构体的 root_device 结构体
}

/**
 * __root_device_register - allocate and register a root device
 * @name: root device name
 * @owner: owner module of the root device, usually THIS_MODULE
 *
 * This function allocates a root device and registers it
 * using device_register(). In order to free the returned
 * device, use root_device_unregister().
 *
 * Root devices are dummy devices which allow other devices
 * to be grouped under /sys/devices. Use this function to
 * allocate a root device and then use it as the parent of
 * any device which should appear under /sys/devices/{name}
 *
 * The /sys/devices/{name} directory will also contain a
 * 'module' symlink which points to the @owner directory
 * in sysfs.
 *
 * Returns &struct device pointer on success, or ERR_PTR() on error.
 *
 * Note: You probably want to use root_device_register().
 */
/**
 * __root_device_register - 分配并注册一个根设备
 * @name: 根设备的名称
 * @owner: 根设备的拥有者模块，通常是 THIS_MODULE
 *
 * 本函数分配一个根设备并使用 device_register() 进行注册。
 * 若要释放返回的设备，请使用 root_device_unregister()。
 *
 * 根设备是虚拟设备，允许其他设备在 /sys/devices 下被分组。
 * 使用此函数分配一个根设备，然后将其作为任何应该出现在
 * /sys/devices/{name} 下的设备的父设备。
 *
 * /sys/devices/{name} 目录也将包含一个指向 @owner 目录的
 * 'module' 符号链接。
 *
 * 成功时返回 &struct device 指针，错误时返回 ERR_PTR()。
 *
 * 注意：你可能想要使用 root_device_register()。
 */
struct device *__root_device_register(const char *name, struct module *owner)
{
	struct root_device *root;	// 定义根设备指针
	int err = -ENOMEM;	// 默认错误为内存不足

	root = kzalloc(sizeof(struct root_device), GFP_KERNEL);	// 为根设备分配内存
	if (!root)	// 如果分配失败，返回错误
		return ERR_PTR(err);

	err = dev_set_name(&root->dev, "%s", name);	// 设置根设备的名称
	if (err) {	// 如果设置名称失败，释放已分配的内存并返回错误
		kfree(root);
		return ERR_PTR(err);
	}

	root->dev.release = root_device_release;	// 设置根设备的释放函数

	err = device_register(&root->dev);	// 注册设备
	if (err) {	// 如果注册失败，释放设备并返回错误
		put_device(&root->dev);
		return ERR_PTR(err);
	}

// 如果定义了 CONFIG_MODULE，创建一个指向模块的符号链接
#ifdef CONFIG_MODULE	/* gotta find a "cleaner" way to do this */
	if (owner) {
		struct module_kobject *mk = &owner->mkobj;

		err = sysfs_create_link(&root->dev.kobj, &mk->kobj, "module");
		if (err) {	// 如果创建符号链接失败，注销设备并返回错误
			device_unregister(&root->dev);
			return ERR_PTR(err);
		}
		root->owner = owner;	// 保存设备的拥有者模块
	}
#endif

	return &root->dev;	// 返回根设备的设备结构
}
EXPORT_SYMBOL_GPL(__root_device_register);

/**
 * root_device_unregister - unregister and free a root device
 * @dev: device going away
 *
 * This function unregisters and cleans up a device that was created by
 * root_device_register().
 */
/**
 * root_device_unregister - 注销并释放一个根设备
 * @dev: 即将被注销的设备
 *
 * 该函数用于注销并清理通过 root_device_register 创建的设备。
 */
void root_device_unregister(struct device *dev)
{
	struct root_device *root = to_root_device(dev);	// 从给定的设备结构体中获取包含它的 root_device 结构体

	if (root->owner)	// 如果该设备有拥有者模块，则移除指向该模块的符号链接
		sysfs_remove_link(&root->dev.kobj, "module");

	device_unregister(dev);	// 调用 device_unregister 来注销设备
}
EXPORT_SYMBOL_GPL(root_device_unregister);

/**
 * device_create_release - 释放设备结构
 * @dev: 被释放的设备
 *
 * 当设备引用计数归零时，释放设备结构的内存。
 */
static void device_create_release(struct device *dev)
{
	pr_debug("device: '%s': %s\n", dev_name(dev), __func__);	// 输出调试信息，显示正在释放的设备名和函数名
	kfree(dev);	// 释放设备结构体占用的内存
}

/**
 * device_create_vargs - creates a device and registers it with sysfs
 * @class: pointer to the struct class that this device should be registered to
 * @parent: pointer to the parent struct device of this new device, if any
 * @devt: the dev_t for the char device to be added
 * @drvdata: the data to be added to the device for callbacks
 * @fmt: string for the device's name
 * @args: va_list for the device's name
 *
 * This function can be used by char device classes.  A struct device
 * will be created in sysfs, registered to the specified class.
 *
 * A "dev" file will be created, showing the dev_t for the device, if
 * the dev_t is not 0,0.
 * If a pointer to a parent struct device is passed in, the newly created
 * struct device will be a child of that device in sysfs.
 * The pointer to the struct device will be returned from the call.
 * Any further sysfs files that might be required can be created using this
 * pointer.
 *
 * Returns &struct device pointer on success, or ERR_PTR() on error.
 *
 * Note: the struct class passed to this function must have previously
 * been created with a call to class_create().
 */
/**
 * device_create_vargs - 创建一个设备并将其注册到 sysfs
 * @class: 此设备应注册到的类的指针
 * @parent: 此新设备的父设备的指针（如果有）
 * @devt: 要添加的字符设备的 dev_t
 * @drvdata: 要添加到设备的回调数据
 * @fmt: 设备名称的字符串
 * @args: 设备名称的 va_list
 *
 * 此函数可由字符设备类使用。将在 sysfs 中创建一个 struct device，并注册到指定的类。
 *
 * 如果 dev_t 不为 0,0，则将创建一个 "dev" 文件，显示设备的 dev_t。
 * 如果传入了父设备的指针，则新创建的 struct device 将在 sysfs 中作为该设备的子设备。
 * 从调用返回的指针指向 struct device。
 * 可以使用此指针创建可能需要的任何其他 sysfs 文件。
 *
 * 成功时返回 &struct device 指针，错误时返回 ERR_PTR()。
 *
 * 注意：传递给此函数的 struct class 必须先前已经通过 class_create() 调用创建。
 */
struct device *device_create_vargs(struct class *class, struct device *parent,
				   dev_t devt, void *drvdata, const char *fmt,
				   va_list args)
{
	struct device *dev = NULL;	// 初始化设备指针为 NULL
	int retval = -ENODEV;	// 默认错误码为无效设备

	// 检查 class 参数是否为空或错误
	if (class == NULL || IS_ERR(class))
		goto error;

	// 为设备分配内存
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {	// 如果内存分配失败
		retval = -ENOMEM;	// 设置错误码为内存不足
		goto error;
	}

	// 设置设备的基本属性
	dev->devt = devt;  // 设置设备类型
	dev->class = class;  // 设置设备类
	dev->parent = parent;  // 设置父设备
	dev->release = device_create_release;  // 设置释放函数
	dev_set_drvdata(dev, drvdata);  // 设置设备驱动数据

	retval = kobject_set_name_vargs(&dev->kobj, fmt, args);	// 设置设备的名称
	if (retval)
		goto error;

	retval = device_register(dev);	// 注册设备
	if (retval)
		goto error;

	return dev;	// 返回设备指针

error:
	put_device(dev);	// 错误处理，释放设备
	return ERR_PTR(retval);	// 返回错误码
}
EXPORT_SYMBOL_GPL(device_create_vargs);

/**
 * device_create - creates a device and registers it with sysfs
 * @class: pointer to the struct class that this device should be registered to
 * @parent: pointer to the parent struct device of this new device, if any
 * @devt: the dev_t for the char device to be added
 * @drvdata: the data to be added to the device for callbacks
 * @fmt: string for the device's name
 *
 * This function can be used by char device classes.  A struct device
 * will be created in sysfs, registered to the specified class.
 *
 * A "dev" file will be created, showing the dev_t for the device, if
 * the dev_t is not 0,0.
 * If a pointer to a parent struct device is passed in, the newly created
 * struct device will be a child of that device in sysfs.
 * The pointer to the struct device will be returned from the call.
 * Any further sysfs files that might be required can be created using this
 * pointer.
 *
 * Returns &struct device pointer on success, or ERR_PTR() on error.
 *
 * Note: the struct class passed to this function must have previously
 * been created with a call to class_create().
 */
/**
 * device_create - 创建并注册设备到 sysfs
 * @class: 此设备应注册到的类的指针
 * @parent: 此新设备的父设备的指针（如果有）
 * @devt: 要添加的字符设备的设备号
 * @drvdata: 要添加到设备的回调数据
 * @fmt: 设备名称的格式字符串
 *
 * 此函数可以被字符设备类使用。将在 sysfs 中创建一个 struct device，并注册到指定的类。
 *
 * 如果设备号 (dev_t) 不是 0,0，则会创建一个 "dev" 文件，显示设备的设备号。
 * 如果传入了父设备的指针，则新创建的 struct device 将在 sysfs 中作为该设备的子设备。
 * 调用后将返回指向 struct device 的指针。
 * 可以使用此指针创建可能需要的任何其他 sysfs 文件。
 *
 * 成功时返回 &struct device 指针，错误时返回 ERR_PTR()。
 *
 * 注意：传递给此函数的 struct class 必须先前已经通过 class_create() 调用创建。
 */
struct device *device_create(struct class *class, struct device *parent,
			     dev_t devt, void *drvdata, const char *fmt, ...)
{
	va_list vargs;  // 定义一个用于处理可变参数的变量
	struct device *dev;  // 定义指向设备的指针

	va_start(vargs, fmt);  // 初始化 vargs 以访问可变参数列表
	// 调用 device_create_vargs 来实际创建并注册设备
	dev = device_create_vargs(class, parent, devt, drvdata, fmt, vargs);
	va_end(vargs);  // 清理 vargs
	return dev;  // 返回设备指针
}
EXPORT_SYMBOL_GPL(device_create);

// __match_devt 是一个辅助函数，用于比较设备的设备号与指定的设备号是否匹配。
static int __match_devt(struct device *dev, void *data)
{
	dev_t *devt = data;	// 将 void 指针转换为 dev_t 类型的指针

	return dev->devt == *devt;	// 返回比较结果，如果设备号相等，则返回 true
}

/**
 * device_destroy - removes a device that was created with device_create()
 * @class: pointer to the struct class that this device was registered with
 * @devt: the dev_t of the device that was previously registered
 *
 * This call unregisters and cleans up a device that was created with a
 * call to device_create().
 */
/**
 * device_destroy - 移除通过 device_create 创建的设备
 * @class: 设备注册时使用的类的指针
 * @devt: 先前注册的设备的设备号
 *
 * 此调用注销并清理通过 device_create() 创建的设备。
 */
void device_destroy(struct class *class, dev_t devt)
{
	struct device *dev;

	// 使用 class_find_device 在给定类中查找匹配的设备
	dev = class_find_device(class, NULL, &devt, __match_devt);
	if (dev) {	// 如果找到设备
		put_device(dev);	// 减少设备的引用计数
		device_unregister(dev);	// 注销设备
	}
}
EXPORT_SYMBOL_GPL(device_destroy);

/**
 * device_rename - renames a device
 * @dev: the pointer to the struct device to be renamed
 * @new_name: the new name of the device
 *
 * It is the responsibility of the caller to provide mutual
 * exclusion between two different calls of device_rename
 * on the same device to ensure that new_name is valid and
 * won't conflict with other devices.
 */
/**
 * device_rename - 重命名一个设备
 * @dev: 要被重命名的设备的指针
 * @new_name: 设备的新名称
 *
 * 调用者有责任在两次对同一设备调用 device_rename 时提供互斥，
 * 以确保 new_name 有效且不会与其他设备冲突。
 */
int device_rename(struct device *dev, char *new_name)
{
	char *old_class_name = NULL;  // 用于存储旧的类名（如果适用）
	char *new_class_name = NULL;  // 用于存储新的类名（如果适用）
	char *old_device_name = NULL;  // 用于存储旧的设备名
	int error;  // 用于存储错误码

	dev = get_device(dev);	// 增加设备的引用计数
	if (!dev)
		return -EINVAL;	// 如果设备不存在，返回无效参数错误

	// 打印调试信息，显示正在重命名的设备
	pr_debug("device: '%s': %s: renaming to '%s'\n", dev_name(dev),
		 __func__, new_name);

#ifdef CONFIG_SYSFS_DEPRECATED
	// 如果设备类存在，并且设备有父设备，获取设备的旧类名
	if ((dev->class) && (dev->parent))
		old_class_name = make_class_name(dev->class->name, &dev->kobj);
#endif
	// 复制旧设备名
	old_device_name = kstrdup(dev_name(dev), GFP_KERNEL);
	if (!old_device_name) {	// 如果内存分配失败
		error = -ENOMEM;	// 设置错误码为内存不足
		goto out;
	}

	// 重命名 kobject
	error = kobject_rename(&dev->kobj, new_name);
	if (error)	// 如果重命名失败
		goto out;

#ifdef CONFIG_SYSFS_DEPRECATED
	// 如果使用了旧的 sysfs 链接方法
	if (old_class_name) {
		// 创建新的类名
		new_class_name = make_class_name(dev->class->name, &dev->kobj);
		if (new_class_name) {
			// 重命名 sysfs 链接
			error = sysfs_rename_link(&dev->parent->kobj,
						  &dev->kobj,
						  old_class_name,
						  new_class_name);
		}
	}
#else
	if (dev->class) {
		// 如果设备类存在，重命名类中的 sysfs 链接
		error = sysfs_rename_link(&dev->class->p->class_subsys.kobj,
					  &dev->kobj, old_device_name, new_name);
	}
#endif

out:
	put_device(dev);	// 减少设备的引用计数

	// 清理资源
	kfree(new_class_name);
	kfree(old_class_name);
	kfree(old_device_name);

	return error;	// 返回错误码
}
EXPORT_SYMBOL_GPL(device_rename);

/**
 * device_move_class_links - 在设备的父对象改变时更新类链接
 * @dev: 设备的指针
 * @old_parent: 设备的旧父对象
 * @new_parent: 设备的新父对象
 *
 * 此函数用于当设备在 sysfs 中的父对象发生变化时，更新与设备相关的类链接。
 * 如果启用了 CONFIG_SYSFS_DEPRECATED，还会处理设备的类名称。
 */
static int device_move_class_links(struct device *dev,
				   struct device *old_parent,
				   struct device *new_parent)
{
	int error = 0;	// 错误码初始化为0
#ifdef CONFIG_SYSFS_DEPRECATED
	char *class_name;

	// 生成设备类名
	class_name = make_class_name(dev->class->name, &dev->kobj);
	if (!class_name) {
		error = -ENOMEM;	// 分配内存失败，返回内存不足错误
		goto out;
	}
	// 如果存在旧父对象，移除与之相关的链接
	if (old_parent) {
		sysfs_remove_link(&dev->kobj, "device");
		sysfs_remove_link(&old_parent->kobj, class_name);
	}
	// 如果存在新父对象，创建新的链接
	if (new_parent) {
		error = sysfs_create_link(&dev->kobj, &new_parent->kobj,
					  "device");
		if (error)	// 创建链接失败
			goto out;
		error = sysfs_create_link(&new_parent->kobj, &dev->kobj,
					  class_name);
		if (error)	// 创建链接失败，移除之前创建的链接
			sysfs_remove_link(&dev->kobj, "device");
	} else
		error = 0;	// 如果没有新父对象，不进行操作
out:
	kfree(class_name);	// 释放类名字符串
	return error;
#else
	// 在非 CONFIG_SYSFS_DEPRECATED 情况下的操作
	if (old_parent)
		sysfs_remove_link(&dev->kobj, "device");	// 移除旧父对象的链接
	if (new_parent)
		error = sysfs_create_link(&dev->kobj, &new_parent->kobj,
					  "device");	// 创建新父对象的链接
	return error;
#endif
}

/**
 * device_move - moves a device to a new parent
 * @dev: the pointer to the struct device to be moved
 * @new_parent: the new parent of the device (can by NULL)
 * @dpm_order: how to reorder the dpm_list
 */
/**
 * device_move - 将设备移动到新的父设备
 * @dev: 要移动的设备
 * @new_parent: 新的父设备（可以是 NULL）
 * @dpm_order: 动态电源管理（DPM）列表的重排序方式
 */
int device_move(struct device *dev, struct device *new_parent,
		enum dpm_order dpm_order)
{
	int error;  // 错误码
	struct device *old_parent;  // 旧的父设备
	struct kobject *new_parent_kobj;  // 新的父设备对象

	// 增加设备的引用计数
	dev = get_device(dev);
	if (!dev)
		return -EINVAL;  // 如果设备无效，则返回错误

	// 锁定设备的电源管理系统，以确保在移动过程中电源状态不会被改变
	device_pm_lock();
	// 获取新的父设备，并增加引用计数
	new_parent = get_device(new_parent);
	// 根据设备和新父设备获取新的父设备kobject
	new_parent_kobj = get_device_parent(dev, new_parent);

	// 记录设备移动操作
	pr_debug("device: '%s': %s: moving to '%s'\n", dev_name(dev),
		 __func__, new_parent ? dev_name(new_parent) : "<NULL>");
	 // 尝试将设备的kobject移动到新的父kobject
	error = kobject_move(&dev->kobj, new_parent_kobj);	// 移动设备的 kobject
	if (error) {
		// 如果移动失败，清理新生成的目录并释放新父设备的引用
		cleanup_glue_dir(dev, new_parent_kobj);
		put_device(new_parent);
		goto out;
	}
	// 记录旧的父设备
	old_parent = dev->parent;
	// 更新设备的父设备信息
	dev->parent = new_parent;
	// 如果有旧父设备，从其子设备列表中移除当前设备
	if (old_parent)
		klist_remove(&dev->p->knode_parent);	// 从旧父设备的子设备列表中删除
	// 如果有新父设备，添加到其子设备列表
	if (new_parent) {
		// 添加到新父设备的子设备列表
		klist_add_tail(&dev->p->knode_parent,
			       &new_parent->p->klist_children);
		// 设置设备的NUMA节点信息
		set_dev_node(dev, dev_to_node(new_parent));
	}

	// 如果设备没有关联类，则跳到out_put标签处理
	if (!dev->class)
		goto out_put;
	// 移动设备的类链接
	error = device_move_class_links(dev, old_parent, new_parent);	// 尝试移动设备相关的类链接
	// 如果移动类链接失败，则尝试恢复原状
	if (error) {
		/* We ignore errors on cleanup since we're hosed anyway... */
		// 如果移动类链接失败，则尝试将链接恢复到原始状态
		device_move_class_links(dev, new_parent, old_parent);
		// 尝试将设备的kobject移回到原父设备的kobject
		if (!kobject_move(&dev->kobj, &old_parent->kobj)) {
			// 如果新的父设备存在，则从其子设备列表中移除当前设备
			if (new_parent)
				klist_remove(&dev->p->knode_parent);
			// 将设备的父设备设置回原来的父设备
			dev->parent = old_parent;
			// 如果原父设备存在，将当前设备添加到原父设备的子设备列表中
			if (old_parent) {
				klist_add_tail(&dev->p->knode_parent,
					       &old_parent->p->klist_children);
				// 设置设备的NUMA节点信息，从原父设备继承
				set_dev_node(dev, dev_to_node(old_parent));
			}
		}
		// 清理由于移动操作而创建的任何临时目录
		cleanup_glue_dir(dev, new_parent_kobj);
		put_device(new_parent);	// 释放新父设备的引用计数
		goto out;	// 跳转到错误处理代码
	}
	// 根据指定的动态电源管理顺序调整设备
	switch (dpm_order) {
	case DPM_ORDER_NONE:	// 不调整设备的顺序
		break;
	case DPM_ORDER_DEV_AFTER_PARENT:
		// 将设备移动到新父设备之后
		device_pm_move_after(dev, new_parent);
		break;
	case DPM_ORDER_PARENT_BEFORE_DEV:
		// 将新父设备移动到设备之前
		device_pm_move_before(new_parent, dev);
		break;
	case DPM_ORDER_DEV_LAST:
		// 将设备移动到列表的末尾
		device_pm_move_last(dev);
		break;
	}
out_put:
	// 减少旧父设备的引用计数
	put_device(old_parent);
out:
	// 解锁设备电源管理
	device_pm_unlock();
	// 减少设备的引用计数
	put_device(dev);
	return error;	// 返回操作结果
}
EXPORT_SYMBOL_GPL(device_move);

/**
 * device_shutdown - call ->shutdown() on each device to shutdown.
 */
// 定义一个设备关机的函数
void device_shutdown(void)
{
	struct device *dev, *devn;

	// 从设备列表的末尾开始，安全地遍历每一个设备，这可以避免在关机过程中删除设备引起的问题
	list_for_each_entry_safe_reverse(dev, devn, &devices_kset->list,
				kobj.entry) {
		// 检查设备是否属于某个总线，并且该总线有定义shutdown方法
		if (dev->bus && dev->bus->shutdown) {
			// 如果有，打印调试信息，表明该设备通过总线的shutdown方法进行关闭
			dev_dbg(dev, "shutdown\n");
			// 调用总线的shutdown方法关闭设备
			dev->bus->shutdown(dev);
			// 检查设备是否有驱动，并且驱动中定义了shutdown方法
		} else if (dev->driver && dev->driver->shutdown) {
			// 如果有，打印调试信息，表明该设备通过驱动的shutdown方法进行关闭
			dev_dbg(dev, "shutdown\n");
			dev->driver->shutdown(dev);	// 调用驱动的shutdown方法关闭设备
		}
	}
	async_synchronize_full();	// 等待所有异步关闭操作完成，确保设备状态一致
}
