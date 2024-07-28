/*
 * driver.c - centralized device driver management
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2007 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2007 Novell Inc.
 *
 * This file is released under the GPLv2
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "base.h"

// 定义一个函数，用于从迭代器中获取下一个设备
static struct device *next_device(struct klist_iter *i)
{
	struct klist_node *n = klist_next(i);  // 从迭代器中获取下一个klist节点
	struct device *dev = NULL;  // 初始化设备指针为NULL
	struct device_private *dev_prv;  // 声明一个设备私有数据的指针

	if (n) {  // 如果从klist中成功获取到了一个节点
		dev_prv = to_device_private_driver(n);  // 将获取的节点转换为设备私有数据结构
		dev = dev_prv->device;  // 从设备私有数据中获取设备指针
	}
	return dev;  // 返回设备指针，如果没有获取到节点则返回NULL
}

/**
 * driver_for_each_device - Iterator for devices bound to a driver.
 * @drv: Driver we're iterating.
 * @start: Device to begin with
 * @data: Data to pass to the callback.
 * @fn: Function to call for each device.
 *
 * Iterate over the @drv's list of devices calling @fn for each one.
 */
/**
 * driver_for_each_device - 对绑定到驱动的设备进行迭代。
 * @drv: 我们正在迭代的驱动程序。
 * @start: 开始的设备
 * @data: 传递给回调的数据。
 * @fn: 对每个设备调用的函数。
 *
 * 遍历 @drv 的设备列表，为每一个设备调用 @fn 函数。
 */
// 定义一个函数，用于迭代绑定到特定驱动程序的所有设备
int driver_for_each_device(struct device_driver *drv, struct device *start,
			   void *data, int (*fn)(struct device *, void *))
{
	struct klist_iter i;  // 声明一个klist迭代器
	struct device *dev;  // 用于存储当前迭代到的设备
	int error = 0;  // 初始化错误码为0

	if (!drv)  // 如果驱动程序指针为空，返回错误
		return -EINVAL;

	klist_iter_init_node(&drv->p->klist_devices, &i,
			     start ? &start->p->knode_driver : NULL);	// 初始化klist迭代器，开始位置由start参数决定
	while ((dev = next_device(&i)) && !error)	// 迭代获取每一个设备，直到没有设备或者回调函数返回错误
		error = fn(dev, data);	// 对每一个设备执行回调函数，传入设备和额外的数据
	klist_iter_exit(&i);	// 退出迭代器
	return error;	// 返回最后的错误码
}
EXPORT_SYMBOL_GPL(driver_for_each_device);

/**
 * driver_find_device - device iterator for locating a particular device.
 * @drv: The device's driver
 * @start: Device to begin with
 * @data: Data to pass to match function
 * @match: Callback function to check device
 *
 * This is similar to the driver_for_each_device() function above, but
 * it returns a reference to a device that is 'found' for later use, as
 * determined by the @match callback.
 *
 * The callback should return 0 if the device doesn't match and non-zero
 * if it does.  If the callback returns non-zero, this function will
 * return to the caller and not iterate over any more devices.
 */
/**
 * driver_find_device - 用于查找特定设备的迭代器。
 * @drv: 设备的驱动程序
 * @start: 开始搜索的设备
 * @data: 传递给匹配函数的数据
 * @match: 用来检查设备的回调函数
 *
 * 这个函数与上面的 driver_for_each_device() 函数相似，但它会返回一个
 * '找到'的设备的引用以供后续使用，这取决于 @match 回调的结果。
 *
 * 如果设备不匹配，回调应该返回0；如果匹配，返回非零值。如果回调返回非零，
 * 此函数将返回给调用者，并不再迭代更多设备。
 */
struct device *driver_find_device(struct device_driver *drv,
				  struct device *start, void *data,
				  int (*match)(struct device *dev, void *data))
{
	struct klist_iter i;  // 定义一个 klist 迭代器
	struct device *dev;   // 用于存储当前迭代的设备

	if (!drv)
		return NULL;  // 如果驱动程序为空，返回 NULL

	klist_iter_init_node(&drv->p->klist_devices, &i,
			     (start ? &start->p->knode_driver : NULL));	// 初始化迭代器，如果 start 不为空，则从 start 的 knode_driver 开始
	while ((dev = next_device(&i)))	// 迭代设备列表
		if (match(dev, data) && get_device(dev))	// 如果当前设备通过匹配函数并且能成功获取设备
			break;	// 跳出循环
	klist_iter_exit(&i);	// 退出迭代器
	return dev;	// 返回找到的设备
}
EXPORT_SYMBOL_GPL(driver_find_device);

/**
 * driver_create_file - create sysfs file for driver.
 * @drv: driver.
 * @attr: driver attribute descriptor.
 */
/**
 * driver_create_file - 为驱动创建sysfs文件。
 * @drv: 驱动。
 * @attr: 驱动属性描述符。
 */
int driver_create_file(struct device_driver *drv,
		       const struct driver_attribute *attr)
{
	int error;	// 定义一个整数变量来存储错误代码
	if (drv)	// 如果驱动指针不为空
		error = sysfs_create_file(&drv->p->kobj, &attr->attr);	// 在系统文件系统中为该驱动创建一个文件
	else
		error = -EINVAL;	// 如果驱动指针为空，设置错误为 -EINVAL（参数无效）

	return error;	// 返回操作的结果
}
EXPORT_SYMBOL_GPL(driver_create_file);

/**
 * driver_remove_file - remove sysfs file for driver.
 * @drv: driver.
 * @attr: driver attribute descriptor.
 */
/**
 * driver_remove_file - 从sysfs中移除驱动的文件。
 * @drv: 驱动。
 * @attr: 驱动属性描述符。
 */
void driver_remove_file(struct device_driver *drv,
			const struct driver_attribute *attr)
{
	if (drv)	// 如果提供的驱动指针不为空
		sysfs_remove_file(&drv->p->kobj, &attr->attr);	// 使用sysfs的API移除指定的文件
}
EXPORT_SYMBOL_GPL(driver_remove_file);

/**
 * driver_add_kobj - add a kobject below the specified driver
 * @drv: requesting device driver
 * @kobj: kobject to add below this driver
 * @fmt: format string that names the kobject
 *
 * You really don't want to do this, this is only here due to one looney
 * iseries driver, go poke those developers if you are annoyed about
 * this...
 */
/**
 * driver_add_kobj - 在指定驱动程序下添加一个kobject
 * @drv: 请求的设备驱动程序
 * @kobj: 要添加的kobject
 * @fmt: 命名kobject的格式字符串
 *
 * 你真的不想这么做，这只是因为一个疯狂的iseries驱动程序才有的，如果你对此感到恼火，
 * 去找那些开发者吧...
 */
int driver_add_kobj(struct device_driver *drv, struct kobject *kobj,
		    const char *fmt, ...)
{
	va_list args;  // 定义一个用于存储额外参数的变量
	char *name;  // 用于存储格式化后的字符串
	int ret;  // 用于存储返回值

	va_start(args, fmt);  // 初始化args变量，以便获取额外参数
	name = kvasprintf(GFP_KERNEL, fmt, args);  // 使用格式字符串和额外参数格式化name
	va_end(args);  // 清理va_list变量

	if (!name)  // 如果内存分配失败
		return -ENOMEM;  // 返回内存不足的错误

	ret = kobject_add(kobj, &drv->p->kobj, "%s", name);	// 将kobject添加到驱动下
	kfree(name);	// 释放格式化字符串所占用的内存
	return ret;		// 返回操作结果
}
EXPORT_SYMBOL_GPL(driver_add_kobj);

/**
 * get_driver - increment driver reference count.
 * @drv: driver.
 */
/**
 * get_driver - 增加驱动程序的引用计数。
 * @drv: 驱动程序。
 */
struct device_driver *get_driver(struct device_driver *drv)
{
	if (drv) {	// 检查是否传入了有效的驱动程序指针
		struct driver_private *priv;	// 定义一个指向驱动程序私有数据的指针
		struct kobject *kobj;	// 定义一个指向kobject的指针

		kobj = kobject_get(&drv->p->kobj);	// 增加kobject的引用计数并获取kobject指针
		priv = to_driver(kobj);	 // 从kobject获取到驱动程序的私有数据结构
		return priv->driver;	// 返回驱动程序结构体指针
	}
	return NULL;	// 如果传入的驱动程序指针无效，返回NULL
}
EXPORT_SYMBOL_GPL(get_driver);

/**
 * put_driver - decrement driver's refcount.
 * @drv: driver.
 */
/**
 * put_driver - 减少驱动程序的引用计数。
 * @drv: 驱动程序。
 */
void put_driver(struct device_driver *drv)
{
	kobject_put(&drv->p->kobj);	// 递减与驱动程序相关联的kobject的引用计数
}
EXPORT_SYMBOL_GPL(put_driver);

/**
 * driver_add_groups - 为驱动程序添加属性组。
 * @drv: 驱动程序。
 * @groups: 指向属性组数组的指针。
 *
 * 该函数为驱动程序添加sysfs属性组。每个属性组由sysfs_create_group创建。
 * 如果在添加过程中发生错误，会回滚之前的添加操作。
 */
static int driver_add_groups(struct device_driver *drv,
			     const struct attribute_group **groups)
{
	int error = 0;  // 错误代码初始化为0，表示无错误
	int i;  // 用于循环计数

	if (groups) {  // 检查是否提供了属性组
		for (i = 0; groups[i]; i++) {  // 遍历所有提供的属性组
			error = sysfs_create_group(&drv->p->kobj, groups[i]);  // 在sysfs中为驱动创建属性组
			if (error) {  // 如果创建属性组过程中出现错误
				while (--i >= 0)  // 回滚之前添加的所有属性组
					sysfs_remove_group(&drv->p->kobj,
							   groups[i]);
				break;  // 跳出循环
			}
		}
	}
	return error;  // 返回错误代码，0表示成功，非0表示发生错误
}

/**
 * driver_remove_groups - 从驱动程序中移除属性组
 * @drv: 驱动程序对象。
 * @groups: 指向属性组数组的指针。
 *
 * 此函数从驱动程序的sysfs目录中移除指定的属性组。
 */
static void driver_remove_groups(struct device_driver *drv,
				 const struct attribute_group **groups)
{
	int i;  // 用于循环的计数变量

	if (groups)  // 检查属性组数组是否存在
		for (i = 0; groups[i]; i++)  // 遍历所有属性组
			sysfs_remove_group(&drv->p->kobj, groups[i]);  // 从sysfs中移除当前属性组
}

/**
 * driver_register - register driver with bus
 * @drv: driver to register
 *
 * We pass off most of the work to the bus_add_driver() call,
 * since most of the things we have to do deal with the bus
 * structures.
 */
/**
 * driver_register - 注册驱动程序到总线
 * @drv: 要注册的驱动程序
 *
 * 大部分工作委托给 bus_add_driver() 函数完成，因为大多数相关操作都与总线结构有关。
 */
int driver_register(struct device_driver *drv)
{
	int ret;  // 用于存储返回值
	struct device_driver *other;  // 用于检查是否有重名的驱动程序

	// 确保驱动程序所在的总线已经初始化
	BUG_ON(!drv->bus->p);

	// 如果驱动程序和总线的某些方法都被定义了，打印警告消息
	if ((drv->bus->probe && drv->probe) ||
	    (drv->bus->remove && drv->remove) ||
	    (drv->bus->shutdown && drv->shutdown))
		printk(KERN_WARNING "Driver '%s' needs updating - please use "
			"bus_type methods\n", drv->name);

	// 检查是否有同名的驱动程序已注册
	other = driver_find(drv->name, drv->bus);
	if (other) {
		put_driver(other);	// 释放找到的驱动程序
		printk(KERN_ERR "Error: Driver '%s' is already registered, "
			"aborting...\n", drv->name);
		return -EBUSY;	// 返回忙碌状态，表示驱动程序已存在
	}

	// 将驱动程序添加到总线
	ret = bus_add_driver(drv);
	if (ret)
		return ret;	// 如果添加失败，返回错误
	ret = driver_add_groups(drv, drv->groups);	// 添加驱动程序的属性组
	if (ret)
		bus_remove_driver(drv);	// 如果添加属性组失败，从总线移除驱动程序
	return ret;	// 返回操作结果
}
EXPORT_SYMBOL_GPL(driver_register);

/**
 * driver_unregister - remove driver from system.
 * @drv: driver.
 *
 * Again, we pass off most of the work to the bus-level call.
 */
/**
 * driver_unregister - 从系统中移除驱动程序。
 * @drv: 要移除的驱动程序。
 *
 * 再次，我们将大部分工作交给总线级调用来处理。
 */
void driver_unregister(struct device_driver *drv)
{
	// 首先检查输入的驱动程序是否有效
	if (!drv || !drv->p) {
		WARN(1, "Unexpected driver unregister!\n");	// 如果驱动程序无效，则打印警告信息
		return;	// 并结束函数执行
	}
	driver_remove_groups(drv, drv->groups);	// 移除与驱动程序关联的所有属性组
	bus_remove_driver(drv);	// 从总线中移除驱动程序
}
EXPORT_SYMBOL_GPL(driver_unregister);

/**
 * driver_find - locate driver on a bus by its name.
 * @name: name of the driver.
 * @bus: bus to scan for the driver.
 *
 * Call kset_find_obj() to iterate over list of drivers on
 * a bus to find driver by name. Return driver if found.
 *
 * Note that kset_find_obj increments driver's reference count.
 */
/**
 * driver_find - 根据名称在总线上查找驱动程序。
 * @name: 驱动程序的名称。
 * @bus: 要搜索的总线。
 *
 * 调用 kset_find_obj() 在总线上的驱动程序列表中迭代查找指定名称的驱动程序。如果找到，则返回驱动程序。
 *
 * 注意，kset_find_obj 会增加驱动程序的引用计数。
 */
struct device_driver *driver_find(const char *name, struct bus_type *bus)
{
	// 使用 kset_find_obj 在总线的驱动程序集中查找具有指定名称的 kobject
	struct kobject *k = kset_find_obj(bus->p->drivers_kset, name);
	struct driver_private *priv;

	// 如果找到了 kobject
	if (k) {
		priv = to_driver(k);	// 将 kobject 转换为 driver_private 结构
		return priv->driver;	// 返回对应的 device_driver 结构
	}
	return NULL;	// 如果未找到，返回 NULL
}
EXPORT_SYMBOL_GPL(driver_find);
