/*
 * bus.c - bus driver management
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
#include <linux/init.h>
#include <linux/string.h>
#include "base.h"
#include "power/power.h"

/* 宏定义用于从通用属性转换到总线属性 */
#define to_bus_attr(_attr) container_of(_attr, struct bus_attribute, attr)
/* 宏定义用于从通用对象转换到总线类型私有结构 */
#define to_bus(obj) container_of(obj, struct bus_type_private, subsys.kobj)

/*
 * sysfs bindings for drivers
 */
/*
 * 为驱动程序的 sysfs 绑定提供的宏定义
 */

#define to_drv_attr(_attr) container_of(_attr, struct driver_attribute, attr)


/* 帮助函数用于重新扫描总线上的设备 */
static int __must_check bus_rescan_devices_helper(struct device *dev,
						void *data);

/* 获取一个总线类型的引用，如果总线存在，则增加其引用计数并返回该总线 */
static struct bus_type *bus_get(struct bus_type *bus)
{
	if (bus) {
		kset_get(&bus->p->subsys);	// 增加所属 kset 的引用计数
		return bus;	// 返回总线类型对象
	}
	return NULL;	// 如果传入的总线为空，则返回 NULL
}

/* 释放一个总线类型的引用，减少其引用计数 */
static void bus_put(struct bus_type *bus)
{
	if (bus)
		kset_put(&bus->p->subsys);	// 减少所属 kset 的引用计数
}

/* 定义用于显示驱动属性的函数 */
static ssize_t drv_attr_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
	// 将通用属性转换为驱动属性
	struct driver_attribute *drv_attr = to_drv_attr(attr);
	// 从 kobject 转换为驱动的私有结构
	struct driver_private *drv_priv = to_driver(kobj);
	ssize_t ret = -EIO;	// 默认返回错误：输入/输出错误

	if (drv_attr->show)	// 如果存在显示方法
		ret = drv_attr->show(drv_priv->driver, buf);	// 调用显示方法并传递缓冲区
	return ret;	// 返回操作结果
}

/* 定义用于存储驱动属性的函数 */
static ssize_t drv_attr_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	struct driver_attribute *drv_attr = to_drv_attr(attr);	// 将通用属性转换为驱动属性
	struct driver_private *drv_priv = to_driver(kobj);		 // 从 kobject 转换为驱动的私有结构
	ssize_t ret = -EIO;		// 默认返回错误：输入/输出错误

	if (drv_attr->store)	// 如果存在存储方法
		ret = drv_attr->store(drv_priv->driver, buf, count);	// 调用存储方法并传递缓冲区和计数
	return ret;	// 返回操作结果
}

/* 定义 sysfs 操作结构，用于驱动属性的显示和存储 */
static const struct sysfs_ops driver_sysfs_ops = {
	.show	= drv_attr_show,		// 设置显示函数
	.store	= drv_attr_store,	// 设置存储函数
};

/* 驱动程序的释放函数，当驱动程序的 kobject 计数为零时调用 */
static void driver_release(struct kobject *kobj)
{
	struct driver_private *drv_priv = to_driver(kobj);	// 从 kobject 获取驱动程序的私有结构

	pr_debug("driver: '%s': %s\n", kobject_name(kobj), __func__);	// 输出调试信息，包括驱动名和函数名
	kfree(drv_priv);	// 释放驱动程序私有结构的内存
}

/* 定义与驱动程序相关的 kobj 类型，包括 sysfs 操作和释放函数 */
static struct kobj_type driver_ktype = {
	.sysfs_ops	= &driver_sysfs_ops,	// 设置 sysfs 操作，用于属性的显示和存储
	.release	= driver_release,				// 设置释放函数
};

/*
 * sysfs bindings for buses
 */
/*
 * 为总线定义的 sysfs 绑定
 */
/* 定义一个函数，用于在 sysfs 中显示总线属性 */
static ssize_t bus_attr_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
	struct bus_attribute *bus_attr = to_bus_attr(attr);	// 将通用属性对象转换为总线属性对象
	struct bus_type_private *bus_priv = to_bus(kobj);	// 从 kobject 转换为总线类型的私有数据结构
	ssize_t ret = 0;  // 默认返回值为 0，表示成功但没有数据写入

	if (bus_attr->show)	// 如果定义了显示方法
		ret = bus_attr->show(bus_priv->bus, buf);	// 调用显示方法，传递总线对象和缓冲区
	return ret;	// 返回操作结果
}

/* 定义一个函数，用于在 sysfs 中存储总线属性 */
static ssize_t bus_attr_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	struct bus_attribute *bus_attr = to_bus_attr(attr);	// 将通用属性对象转换为总线属性对象
	struct bus_type_private *bus_priv = to_bus(kobj);		// 从 kobject 转换为总线类型的私有数据结构
	ssize_t ret = 0;	// 默认返回值为 0，表示操作成功但没有数据被处理

	if (bus_attr->store)	// 如果定义了存储方法
		ret = bus_attr->store(bus_priv->bus, buf, count);	// 调用存储方法，传递总线对象、缓冲区和数据长度
	return ret;	// 返回操作结果
}

/* 定义总线属性的 sysfs 操作结构 */
static const struct sysfs_ops bus_sysfs_ops = {
	.show	= bus_attr_show,	// 设置显示方法
	.store	= bus_attr_store,	// 设置存储方法
};

/* 在 sysfs 中为指定的总线创建属性文件 */
int bus_create_file(struct bus_type *bus, struct bus_attribute *attr)
{
	int error;
	if (bus_get(bus)) {	// 尝试获取总线引用，成功则继续
		error = sysfs_create_file(&bus->p->subsys.kobj, &attr->attr);	// 创建 sysfs 文件
		bus_put(bus);	// 释放总线引用
	} else
		error = -EINVAL;	// 如果获取总线失败，返回无效参数错误
	return error;
}
EXPORT_SYMBOL_GPL(bus_create_file);

/* 从 sysfs 中删除指定总线的属性文件 */
void bus_remove_file(struct bus_type *bus, struct bus_attribute *attr)
{
	if (bus_get(bus)) {	// 尝试获取总线引用，成功则继续
		sysfs_remove_file(&bus->p->subsys.kobj, &attr->attr);	// 删除 sysfs 文件
		bus_put(bus);	// 释放总线引用
	}
}
EXPORT_SYMBOL_GPL(bus_remove_file);

/* 定义与总线相关的 kobj 类型，指定 sysfs 操作函数 */
static struct kobj_type bus_ktype = {
	.sysfs_ops	= &bus_sysfs_ops,	// 设置 sysfs 操作函数，用于属性的显示和存储
};

/* 总线的 uevent 过滤函数，决定哪些 kobject 的事件应该被发送到用户空间 */
static int bus_uevent_filter(struct kset *kset, struct kobject *kobj)
{
	struct kobj_type *ktype = get_ktype(kobj);	// 获取 kobject 的类型

	if (ktype == &bus_ktype)	// 如果 kobject 的类型是 bus_ktype
		return 1;	// 返回 1 表示允许事件被发送
	return 0;	// 否则返回 0 表示不允许事件被发送
}

/* 定义总线的 uevent 操作，包括上面定义的过滤函数 */
static const struct kset_uevent_ops bus_uevent_ops = {
	.filter = bus_uevent_filter,	// 设置过滤函数
};

/* 定义指向总线 kset 的全局指针 */
static struct kset *bus_kset;


#ifdef CONFIG_HOTPLUG
/* Manually detach a device from its associated driver. */
/* 手动从关联的驱动程序分离设备。 */
static ssize_t driver_unbind(struct device_driver *drv,
			     const char *buf, size_t count)
{
	struct bus_type *bus = bus_get(drv->bus);	// 获取设备所在的总线
	struct device *dev;
	int err = -ENODEV;	// 默认错误码为“设备不存在”

	dev = bus_find_device_by_name(bus, NULL, buf);	// 根据名称在总线上查找设备
	if (dev && dev->driver == drv) {	// 如果找到设备，并且设备的驱动与给定的驱动匹配
		// 如果设备有父设备（例如USB设备常常需要）
		if (dev->parent)	/* Needed for USB */
			device_lock(dev->parent);	// 锁定父设备以进行操作
		device_release_driver(dev);	// 释放设备的驱动
		if (dev->parent)
			device_unlock(dev->parent);	// 解锁父设备
		err = count;	// 如果操作成功，返回写入的字节数
	}
	put_device(dev);	// 释放对设备的引用
	bus_put(bus);	// 释放对总线的引用
	return err;	// 返回结果
}
// 创建一个驱动属性，允许写入，用于调用上面的函数
static DRIVER_ATTR(unbind, S_IWUSR, NULL, driver_unbind);

/*
 * Manually attach a device to a driver.
 * Note: the driver must want to bind to the device,
 * it is not possible to override the driver's id table.
 */
/*
 * 手动将设备附加到驱动。
 * 注意：驱动必须希望绑定到该设备，
 * 不可能覆盖驱动的 ID 表。
 */
static ssize_t driver_bind(struct device_driver *drv,
			   const char *buf, size_t count)
{
	struct bus_type *bus = bus_get(drv->bus);	// 获取与驱动相关的总线
	struct device *dev;
	int err = -ENODEV;	// 默认设备未找到错误

	dev = bus_find_device_by_name(bus, NULL, buf);	// 根据名称查找设备
	if (dev && dev->driver == NULL && driver_match_device(drv, dev)) {	// 如果设备存在，且未绑定驱动，且驱动与设备匹配
		// 如果设备有父设备（对于某些类型如USB是必须的）
		if (dev->parent)	/* Needed for USB */
			device_lock(dev->parent);	// 锁定父设备
		device_lock(dev);	// 锁定设备
		err = driver_probe_device(drv, dev);	// 尝试将设备绑定到驱动
		device_unlock(dev);	// 解锁设备
		if (dev->parent)
			device_unlock(dev->parent);	// 解锁父设备

		/* 绑定成功 */
		if (err > 0) {
			/* success */
			err = count;	 // 返回写入的字节数
		} else if (err == 0) {
			/* driver didn't accept device */
			/* 驱动未接受设备 */
			err = -ENODEV;
		}
	}
	put_device(dev);	// 释放设备的引用
	bus_put(bus);		// 释放总线的引用
	return err;
}
// 创建一个驱动属性，允许写入，用于调用上面的函数
static DRIVER_ATTR(bind, S_IWUSR, NULL, driver_bind);

/* 显示总线是否启用自动驱动程序探测 */
static ssize_t show_drivers_autoprobe(struct bus_type *bus, char *buf)
{
	// 将总线的自动探测状态写入到缓冲区，并返回写入的字节数
	return sprintf(buf, "%d\n", bus->p->drivers_autoprobe);
}

/* 存储设置，用于启用或禁用总线上的自动驱动程序探测 */
static ssize_t store_drivers_autoprobe(struct bus_type *bus,
				       const char *buf, size_t count)
{
	if (buf[0] == '0')	// 如果缓冲区的第一个字符是 '0'，禁用自动探测
		bus->p->drivers_autoprobe = 0;
	else
		bus->p->drivers_autoprobe = 1;	// 否则，启用自动探测
	return count;	// 返回处理的字符数量，即输入的字节数
}

/* 手动触发指定设备的探测过程 */
static ssize_t store_drivers_probe(struct bus_type *bus,
				   const char *buf, size_t count)
{
	struct device *dev;

	dev = bus_find_device_by_name(bus, NULL, buf);	// 根据名称查找设备
	if (!dev)
		return -ENODEV;	// 如果没有找到设备，返回设备不存在的错误
	if (bus_rescan_devices_helper(dev, NULL) != 0)	// 如果设备探测助手函数返回非零值，表示出现错误，返回无效参数错误
		return -EINVAL;
	return count;	// 如果探测成功，返回输入的字节数
}
#endif

/* 从 klist 迭代器中获取下一个设备 */
static struct device *next_device(struct klist_iter *i)
{
	struct klist_node *n = klist_next(i);	// 从迭代器获取下一个 klist 节点
	struct device *dev = NULL;						// 初始化设备指针为 NULL
	struct device_private *dev_prv;

	if (n) {	 // 如果 klist 节点存在
		dev_prv = to_device_private_bus(n);	// 将 klist 节点转换为设备私有数据结构
		dev = dev_prv->device;		// 从设备私有数据结构中获取设备对象的指针
	}
	return dev;	// 返回设备对象指针，如果列表结束则为 NULL
}

/**
 * bus_for_each_dev - device iterator.
 * @bus: bus type.
 * @start: device to start iterating from.
 * @data: data for the callback.
 * @fn: function to be called for each device.
 *
 * Iterate over @bus's list of devices, and call @fn for each,
 * passing it @data. If @start is not NULL, we use that device to
 * begin iterating from.
 *
 * We check the return of @fn each time. If it returns anything
 * other than 0, we break out and return that value.
 *
 * NOTE: The device that returns a non-zero value is not retained
 * in any way, nor is its refcount incremented. If the caller needs
 * to retain this data, it should do so, and increment the reference
 * count in the supplied callback.
 */
/**
 * bus_for_each_dev - 设备迭代器。
 * @bus: 总线类型。
 * @start: 从该设备开始迭代。
 * @data: 传递给回调的数据。
 * @fn: 对每个设备调用的函数。
 *
 * 遍历 @bus 上的设备列表，并为每个设备调用 @fn，
 * 同时传递给它 @data。如果 @start 不为 NULL，则从该设备开始迭代。
 *
 * 我们每次调用 @fn 时都会检查其返回值。如果返回非 0 值，
 * 我们将中断循环并返回该值。
 *
 * 注意：返回非零值的设备不会以任何方式保留，也不会增加其引用计数。
 * 如果调用者需要保留这些数据，它应该自己操作，并在提供的回调中增加引用计数。
 */
int bus_for_each_dev(struct bus_type *bus, struct device *start,
		     void *data, int (*fn)(struct device *, void *))
{
	struct klist_iter i;	// 定义 klist 迭代器
	struct device *dev;
	int error = 0;	// 默认错误码为0，表示无错误

	if (!bus)
		return -EINVAL;	// 如果 bus 为空，返回无效参数错误

	klist_iter_init_node(&bus->p->klist_devices, &i,
			     (start ? &start->p->knode_bus : NULL));	// 初始化 klist 迭代器，从 start 设备开始，如果 start 为空，则从头开始
	while ((dev = next_device(&i)) && !error)	// 循环遍历设备
		error = fn(dev, data);	// 对每个设备调用回调函数，将结果存储在 error 中
	klist_iter_exit(&i);	// 退出迭代器，清理资源
	return error;	// 返回最后的错误码，如果没有错误则返回0
}
EXPORT_SYMBOL_GPL(bus_for_each_dev);

/**
 * bus_find_device - device iterator for locating a particular device.
 * @bus: bus type
 * @start: Device to begin with
 * @data: Data to pass to match function
 * @match: Callback function to check device
 *
 * This is similar to the bus_for_each_dev() function above, but it
 * returns a reference to a device that is 'found' for later use, as
 * determined by the @match callback.
 *
 * The callback should return 0 if the device doesn't match and non-zero
 * if it does.  If the callback returns non-zero, this function will
 * return to the caller and not iterate over any more devices.
 */
/**
 * bus_find_device - 用于定位特定设备的设备迭代器。
 * @bus: 总线类型
 * @start: 开始搜索的设备
 * @data: 传递给匹配函数的数据
 * @match: 用于检查设备的回调函数
 *
 * 这个函数与上面的 bus_for_each_dev() 函数类似，但它返回一个找到的设备的引用，
 * 供后续使用，这是由 @match 回调确定的。
 *
 * 如果设备不匹配，回调应返回 0；如果匹配，则返回非零值。如果回调返回非零，
 * 此函数将返回给调用者并停止迭代其他设备。
 */
struct device *bus_find_device(struct bus_type *bus,
			       struct device *start, void *data,
			       int (*match)(struct device *dev, void *data))
{
	struct klist_iter i;
	struct device *dev;

	if (!bus)
		return NULL;	// 如果 bus 为空，直接返回 NULL

	klist_iter_init_node(&bus->p->klist_devices, &i,
			     (start ? &start->p->knode_bus : NULL));	// 从 start 或列表头开始初始化迭代器
	while ((dev = next_device(&i)))	// 遍历设备列表
		if (match(dev, data) && get_device(dev))	// 如果找到匹配的设备，并成功增加设备引用计数
			break;	// 找到设备后跳出循环
	klist_iter_exit(&i);	// 清理迭代器
	return dev;	// 返回找到的设备，如果没有找到或bus为NULL则为NULL
}
EXPORT_SYMBOL_GPL(bus_find_device);

/* 用于匹配设备名称的回调函数 */
static int match_name(struct device *dev, void *data)
{
	const char *name = data;	// 将传入的 data 参数转换为 const char* 类型的设备名称

	return sysfs_streq(name, dev_name(dev));	// 比较给定名称与设备名称是否相同
}

/**
 * bus_find_device_by_name - device iterator for locating a particular device of a specific name
 * @bus: bus type
 * @start: Device to begin with
 * @name: name of the device to match
 *
 * This is similar to the bus_find_device() function above, but it handles
 * searching by a name automatically, no need to write another strcmp matching
 * function.
 */
/**
 * bus_find_device_by_name - 根据名称定位特定设备的设备迭代器
 * @bus: 总线类型
 * @start: 开始搜索的设备
 * @name: 要匹配的设备名称
 *
 * 这个函数与上面的 bus_find_device() 函数相似，但它自动处理
 * 根据名称搜索，无需编写另一个 strcmp 匹配函数。
 */
struct device *bus_find_device_by_name(struct bus_type *bus,
				       struct device *start, const char *name)
{
	// 调用 bus_find_device 函数，传入设备名称和 match_name 回调
	return bus_find_device(bus, start, (void *)name, match_name);
}
EXPORT_SYMBOL_GPL(bus_find_device_by_name);

/**
 * 从 klist 迭代器获取下一个驱动程序。
 * @i: 指向 klist_iter 结构的指针，它维护当前的迭代状态。
 *
 * 此函数遍历 klist，每次调用返回链表中的下一个驱动程序。
 * 如果没有更多驱动程序，返回 NULL。
 */
static struct device_driver *next_driver(struct klist_iter *i)
{
	struct klist_node *n = klist_next(i);  // 从迭代器获取下一个 klist 节点
	struct driver_private *drv_priv;       // 声明一个指向 driver_private 结构的指针	

	if (n) {	// 检查 klist 节点是否存在
		drv_priv = container_of(n, struct driver_private, knode_bus);	// 从 klist 节点获取驱动程序的私有数据结构
		return drv_priv->driver;	// 返回指向设备驱动程序的指针
	}
	return NULL;	// 如果没有更多节点，返回 NULL
}

/**
 * bus_for_each_drv - driver iterator
 * @bus: bus we're dealing with.
 * @start: driver to start iterating on.
 * @data: data to pass to the callback.
 * @fn: function to call for each driver.
 *
 * This is nearly identical to the device iterator above.
 * We iterate over each driver that belongs to @bus, and call
 * @fn for each. If @fn returns anything but 0, we break out
 * and return it. If @start is not NULL, we use it as the head
 * of the list.
 *
 * NOTE: we don't return the driver that returns a non-zero
 * value, nor do we leave the reference count incremented for that
 * driver. If the caller needs to know that info, it must set it
 * in the callback. It must also be sure to increment the refcount
 * so it doesn't disappear before returning to the caller.
 */
/**
 * bus_for_each_drv - 驱动程序迭代器
 * @bus: 我们正在处理的总线。
 * @start: 开始迭代的驱动程序。
 * @data: 传递给回调的数据。
 * @fn: 对每个驱动程序调用的函数。
 *
 * 这几乎与上面的设备迭代器相同。
 * 我们遍历属于 @bus 的每个驱动程序，并为每个驱动程序调用 @fn。
 * 如果 @fn 返回的不是 0，我们将中断并返回它。
 * 如果 @start 不是 NULL，我们将它用作列表的头部。
 *
 * 注意：我们不返回返回非零值的驱动程序，也不增加该驱动程序的引用计数。
 * 如果调用者需要这些信息，必须在回调中设置。
 * 调用者还必须确保增加引用计数，以便在返回给调用者之前驱动程序不会消失。
 */
int bus_for_each_drv(struct bus_type *bus, struct device_driver *start,
		     void *data, int (*fn)(struct device_driver *, void *))
{
	struct klist_iter i;
	struct device_driver *drv;
	int error = 0;	// 初始化错误码为0，表示无错误

	if (!bus)
		return -EINVAL;	// 如果 bus 为空，返回无效参数错误

	klist_iter_init_node(&bus->p->klist_drivers, &i,
			     start ? &start->p->knode_bus : NULL);	// 初始化 klist 迭代器，从 start 或列表头开始
	while ((drv = next_driver(&i)) && !error)	// 遍历驱动程序列表
		error = fn(drv, data);	// 对每个驱动程序调用回调函数，将结果存储在 error 中
	klist_iter_exit(&i);	// 清理迭代器
	return error;	// 返回最后的错误码，如果没有错误则返回0
}
EXPORT_SYMBOL_GPL(bus_for_each_drv);

/**
 * device_add_attrs - 为设备添加一组属性
 * @bus: 设备所在的总线
 * @dev: 需要添加属性的设备
 *
 * 此函数尝试为总线上的设备添加一组预定义的属性。如果添加属性过程中出现错误，
 * 它会删除所有已添加的属性，确保设备状态的一致性。
 *
 * 返回: 如果成功添加所有属性，返回 0；如果有错误发生，返回错误代码。
 */
static int device_add_attrs(struct bus_type *bus, struct device *dev)
{
	int error = 0;	// 初始化错误码为0，表示无错误
	int i;

	if (!bus->dev_attrs)	// 检查总线是否定义了设备属性
		return 0;	// 如果没有定义属性，直接返回0

	for (i = 0; attr_name(bus->dev_attrs[i]); i++) {	// 遍历所有属性
		error = device_create_file(dev, &bus->dev_attrs[i]);	// 为设备创建文件，对应于一个属性
		if (error) {	// 如果创建属性文件过程中出现错误
			while (--i >= 0)	// 回滚，删除之前添加的所有属性
				device_remove_file(dev, &bus->dev_attrs[i]);
			break;	// 跳出循环
		}
	}
	return error;	// 返回错误码，如果添加过程无误，则返回0
}

/**
 * device_remove_attrs - 从设备中移除属性
 * @bus: 设备所在的总线
 * @dev: 需要移除属性的设备
 *
 * 此函数用于从指定设备中移除所有在总线上定义的属性。它通过遍历总线的属性数组，
 * 并为每个有效的属性调用 device_remove_file 来删除在 sysfs 中创建的文件。
 */
static void device_remove_attrs(struct bus_type *bus, struct device *dev)
{
	int i;

	if (bus->dev_attrs) {	// 检查总线是否定义了设备属性
		for (i = 0; attr_name(bus->dev_attrs[i]); i++)	// 遍历所有属性
			device_remove_file(dev, &bus->dev_attrs[i]);	// 移除每个属性对应的文件
	}
}

/* 如果启用了 SYSFS 的过时特性支持 */
#ifdef CONFIG_SYSFS_DEPRECATED
/* 创建一个指向总线的过时链接 */
static int make_deprecated_bus_links(struct device *dev)
{
	return sysfs_create_link(&dev->kobj,	// 设备的 kobject
				 &dev->bus->p->subsys.kobj, "bus");	// 创建一个名为 "bus" 的链接，指向设备所在总线的 kobject
}

/* 移除之前创建的指向总线的过时链接 */
static void remove_deprecated_bus_links(struct device *dev)
{
	sysfs_remove_link(&dev->kobj, "bus");	// 移除名为 "bus" 的链接
}
/* 如果没有启用 SYSFS 的过时特性支持 */
#else
/* 定义一个空操作的函数，总是返回 0 */
static inline int make_deprecated_bus_links(struct device *dev) { return 0; }
/* 定义一个空操作的函数 */
static inline void remove_deprecated_bus_links(struct device *dev) { }
#endif

/**
 * bus_add_device - add device to bus
 * @dev: device being added
 *
 * - Add device's bus attributes.
 * - Create links to device's bus.
 * - Add the device to its bus's list of devices.
 */
/**
 * bus_add_device - 将设备添加到总线
 * @dev: 正在添加的设备
 *
 * - 为设备添加总线属性。
 * - 创建指向设备总线的链接。
 * - 将设备添加到其总线的设备列表中。
 */
int bus_add_device(struct device *dev)
{
	struct bus_type *bus = bus_get(dev->bus);	// 获取设备所在的总线，并增加总线的引用计数
	int error = 0;

	if (bus) {
		pr_debug("bus: '%s': add device %s\n", bus->name, dev_name(dev));	// 调试信息，显示正在添加的设备和所在的总线
		error = device_add_attrs(bus, dev);	// 为设备添加总线定义的属性
		if (error)
			goto out_put;	// 如果出现错误，跳到错误处理代码
		error = sysfs_create_link(&bus->p->devices_kset->kobj,
						&dev->kobj, dev_name(dev));	// 在 sysfs 中创建一个链接，指向设备的 kobject
		if (error)
			goto out_id;	// 如果出错，跳到另一个错误处理段
		error = sysfs_create_link(&dev->kobj,
				&dev->bus->p->subsys.kobj, "subsystem");	// 创建一个名为 "subsystem" 的链接，指向总线的子系统
		if (error)
			goto out_subsys;	// 如果出错，跳到错误处理段
		error = make_deprecated_bus_links(dev);	// 创建过时的总线链接（如果启用了相应的配置）
		if (error)
			goto out_deprecated;	// 如果出错，跳到错误处理段
		klist_add_tail(&dev->p->knode_bus, &bus->p->klist_devices);	// 将设备添加到总线的设备链表中
	}
	return 0;

out_deprecated:
	sysfs_remove_link(&dev->kobj, "subsystem");	// 回滚：移除 "subsystem" 链接
out_subsys:
	sysfs_remove_link(&bus->p->devices_kset->kobj, dev_name(dev));	// 回滚：移除指向设备的链接
out_id:
	device_remove_attrs(bus, dev);	// 回滚：移除为设备添加的属性
out_put:
	bus_put(dev->bus);	// 释放对总线的引用
	return error;	// 返回错误码
}

/**
 * bus_probe_device - probe drivers for a new device
 * @dev: device to probe
 *
 * - Automatically probe for a driver if the bus allows it.
 */
/**
 * bus_probe_device - 为新设备探测驱动程序
 * @dev: 需要探测的设备
 *
 * - 如果总线允许，自动为设备探测并尝试附加驱动程序。
 */
void bus_probe_device(struct device *dev)
{
	struct bus_type *bus = dev->bus;	// 获取设备所在的总线
	int ret;

	if (bus && bus->p->drivers_autoprobe) {	// 检查总线是否存在且总线是否允许自动探测驱动程序
		ret = device_attach(dev);	// 尝试将驱动程序附加到设备上
		WARN_ON(ret < 0);	// 如果附加过程返回错误，输出警告信息
	}
}

/**
 * bus_remove_device - remove device from bus
 * @dev: device to be removed
 *
 * - Remove symlink from bus's directory.
 * - Delete device from bus's list.
 * - Detach from its driver.
 * - Drop reference taken in bus_add_device().
 */
/**
 * bus_remove_device - 从总线移除设备
 * @dev: 要被移除的设备
 *
 * - 从总线的目录中移除符号链接。
 * - 从总线的设备列表中删除设备。
 * - 从其驱动程序中分离。
 * - 放弃在 bus_add_device() 中获取的引用。
 */
void bus_remove_device(struct device *dev)
{
	if (dev->bus) {	// 检查设备是否有关联的总线
		sysfs_remove_link(&dev->kobj, "subsystem");	// 移除指向 subsystem 的符号链接
		remove_deprecated_bus_links(dev);	// 移除过时的总线链接（如果有）
		sysfs_remove_link(&dev->bus->p->devices_kset->kobj,
				  dev_name(dev));	// 移除指向设备的符号链接
		device_remove_attrs(dev->bus, dev);	 // 移除设备的属性
		if (klist_node_attached(&dev->p->knode_bus))
			klist_del(&dev->p->knode_bus);	// 如果设备仍在总线的 klist 中，则从中删除

		pr_debug("bus: '%s': remove device %s\n",
			 dev->bus->name, dev_name(dev));	// 打印调试信息，显示正在移除的设备和其所在的总线
		device_release_driver(dev);	// 从其驱动程序中分离设备
		bus_put(dev->bus);	// 释放对总线的引用
	}
}

/**
 * driver_add_attrs - 为驱动程序添加属性
 * @bus: 驱动程序所属的总线
 * @drv: 要添加属性的驱动程序
 *
 * 为驱动程序添加一系列属性，这些属性由总线指定。如果在添加过程中遇到错误，
 * 函数将撤销所有已经添加的属性，以确保不留下不完整的状态。
 *
 * 返回: 如果成功添加所有属性，返回 0；如果有错误发生，返回错误代码。
 */
static int driver_add_attrs(struct bus_type *bus, struct device_driver *drv)
{
	int error = 0;	// 初始化错误码为0，表示无错误
	int i;

	if (bus->drv_attrs) {	// 检查总线是否定义了驱动属性
		for (i = 0; attr_name(bus->drv_attrs[i]); i++) {	// 遍历所有属性
			error = driver_create_file(drv, &bus->drv_attrs[i]);	// 为驱动创建属性文件
			if (error)
				goto err;	// 如果出现错误，跳到错误处理代码块
		}
	}
done:
	return error;	// 返回错误码，如果添加过程无误，则返回0
err:
	while (--i >= 0)	// 回滚，删除之前添加的所有属性
		driver_remove_file(drv, &bus->drv_attrs[i]);
	goto done;	// 完成回滚后跳转到 done 标签，返回错误码
}

/**
 * driver_remove_attrs - 从驱动程序中移除属性
 * @bus: 驱动程序所属的总线
 * @drv: 要移除属性的驱动程序
 *
 * 此函数用于从指定驱动程序中移除所有在总线上定义的属性。它通过遍历总线的属性数组，
 * 并为每个有效的属性调用 driver_remove_file 来删除在 sysfs 中创建的文件。
 */
static void driver_remove_attrs(struct bus_type *bus,
				struct device_driver *drv)
{
	int i;

	if (bus->drv_attrs) {	// 检查总线是否定义了驱动属性
		for (i = 0; attr_name(bus->drv_attrs[i]); i++)	// 遍历所有属性
			driver_remove_file(drv, &bus->drv_attrs[i]);	// 移除每个属性对应的文件
	}
}

#ifdef CONFIG_HOTPLUG
/*
 * Thanks to drivers making their tables __devinit, we can't allow manual
 * bind and unbind from userspace unless CONFIG_HOTPLUG is enabled.
 */
/*
 * 由于驱动程序使用了 __devinit 修饰它们的表，我们无法允许从用户空间手动绑定和解绑，
 * 除非启用了 CONFIG_HOTPLUG。
 */
/* 为驱动程序添加绑定和解绑的 sysfs 文件 */
static int __must_check add_bind_files(struct device_driver *drv)
{
	int ret;

	ret = driver_create_file(drv, &driver_attr_unbind);	// 创建解绑文件
	if (ret == 0) {
		ret = driver_create_file(drv, &driver_attr_bind);	// 如果解绑文件创建成功，则尝试创建绑定文件
		if (ret)
			driver_remove_file(drv, &driver_attr_unbind);	// 如果绑定文件创建失败，删除解绑文件
	}
	return ret;	// 返回结果，0表示成功，非0表示失败
}

/* 移除驱动程序的绑定和解绑 sysfs 文件 */
static void remove_bind_files(struct device_driver *drv)
{
	driver_remove_file(drv, &driver_attr_bind);  // 移除绑定文件
	driver_remove_file(drv, &driver_attr_unbind);  // 移除解绑文件
}

/* 定义一个 bus 属性，用于触发驱动程序的手动探测操作，仅写权限 */
static BUS_ATTR(drivers_probe, S_IWUSR, NULL, store_drivers_probe);
/* 定义一个 bus 属性，用于显示和设置驱动程序的自动探测状态，读写权限 */
static BUS_ATTR(drivers_autoprobe, S_IWUSR | S_IRUGO,
		show_drivers_autoprobe, store_drivers_autoprobe);

/**
 * add_probe_files - 在总线上创建探测相关的 sysfs 文件
 * @bus: 要添加文件的总线
 *
 * 创建与驱动程序探测和自动探测相关的 sysfs 文件。
 * 如果创建第一个文件失败，函数直接返回错误。
 * 如果第一个文件创建成功但第二个文件创建失败，将会撤销第一个文件的创建。
 * 
 * 返回: 成功返回 0，失败返回错误码。
 */
static int add_probe_files(struct bus_type *bus)
{
	int retval;

	retval = bus_create_file(bus, &bus_attr_drivers_probe);	// 创建探测驱动的文件
	if (retval)
		goto out;	// 创建失败，直接返回错误

	retval = bus_create_file(bus, &bus_attr_drivers_autoprobe);	// 创建设置自动探测的文件
	if (retval)
		bus_remove_file(bus, &bus_attr_drivers_probe);	// 如果失败，移除之前创建的文件
out:
	return retval;	// 返回结果
}

/**
 * remove_probe_files - 从总线上移除探测相关的 sysfs 文件
 * @bus: 要移除文件的总线
 *
 * 移除与驱动程序探测和自动探测相关的 sysfs 文件。
 */
static void remove_probe_files(struct bus_type *bus)
{
	bus_remove_file(bus, &bus_attr_drivers_autoprobe);	// 移除设置自动探测的文件
	bus_remove_file(bus, &bus_attr_drivers_probe);			// 移除探测驱动的文件
}
#else
/**
 * 在没有启用 CONFIG_HOTPLUG 时提供的空操作函数。
 */
static inline int add_bind_files(struct device_driver *drv) { return 0; }  // 不执行任何操作，返回0
static inline void remove_bind_files(struct device_driver *drv) {}  // 不执行任何操作
static inline int add_probe_files(struct bus_type *bus) { return 0; }  // 不执行任何操作，返回0
static inline void remove_probe_files(struct bus_type *bus) {}  // 不执行任何操作
#endif

/**
 * driver_uevent_store - 通过 sysfs 文件写入触发驱动程序的 kobject 事件
 * @drv: 相关的设备驱动
 * @buf: 用户写入的数据缓冲区
 * @count: 写入数据的字节数
 *
 * 这个函数处理从用户空间写入到驱动的 uevent 属性的数据，
 * 并根据写入的内容触发相应的 kobject 事件。
 *
 * 返回: 返回写入的字节数。
 */
static ssize_t driver_uevent_store(struct device_driver *drv,
				   const char *buf, size_t count)
{
	enum kobject_action action;	// 定义一个 kobject_action 类型的变量来存储解析的动作

	if (kobject_action_type(buf, count, &action) == 0)	// 解析 buf 中的动作，检查是否有效
		kobject_uevent(&drv->p->kobj, action);	// 如果解析成功，则触发对应的 kobject 事件
	return count;	// 返回处理的字节数
}
static DRIVER_ATTR(uevent, S_IWUSR, NULL, driver_uevent_store);	// 定义驱动的 uevent 属性，仅写权限

/**
 * bus_add_driver - Add a driver to the bus.
 * @drv: driver.
 */
/**
 * bus_add_driver - 将驱动程序添加到总线上。
 * @drv: 驱动程序。
 *
 * 此函数为指定的驱动程序在其所属总线上进行注册和初始化。
 * 包括设置内部数据结构、sysfs 属性以及自动探测设备等。
 */
int bus_add_driver(struct device_driver *drv)
{
	struct bus_type *bus;
	struct driver_private *priv;
	int error = 0;

	bus = bus_get(drv->bus);	// 获取驱动程序所属的总线，并增加引用计数
	if (!bus)
		return -EINVAL;	// 如果总线不存在，返回错误

	pr_debug("bus: '%s': add driver %s\n", bus->name, drv->name);	// 打印调试信息

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);	// 为驱动程序分配私有数据结构
	if (!priv) {
		error = -ENOMEM;	// 如果内存分配失败，设置错误码
		goto out_put_bus;	// 跳转到错误处理代码
	}
	klist_init(&priv->klist_devices, NULL, NULL);	// 初始化设备链表
	priv->driver = drv;	// 关联私有数据结构和驱动程序
	drv->p = priv;			// 在驱动程序中设置私有数据指针
	priv->kobj.kset = bus->p->drivers_kset;	// 设置 kobject 的 kset 为驱动集
	error = kobject_init_and_add(&priv->kobj, &driver_ktype, NULL,	// 初始化并添加 kobject 到系统
				     "%s", drv->name);
	if (error)
		goto out_unregister;	// 如果出错，进行撤销操作

	if (drv->bus->p->drivers_autoprobe) {	// 如果总线支持自动探测
		error = driver_attach(drv);	// 尝试附加设备到驱动
		if (error)
			goto out_unregister;	// 如果附加失败，进行撤销
	}
	klist_add_tail(&priv->knode_bus, &bus->p->klist_drivers);	// 将驱动添加到总线的驱动列表中
	module_add_driver(drv->owner, drv);		// 将模块和驱动关联

	error = driver_create_file(drv, &driver_attr_uevent);	// 创建 uevent sysfs 文件
	if (error) {
		printk(KERN_ERR "%s: uevent attr (%s) failed\n",
			__func__, drv->name);	// 如果创建失败，打印错误日志
	}
	error = driver_add_attrs(bus, drv);	// 添加驱动属性
	if (error) {
		/* How the hell do we get out of this pickle? Give up */
		printk(KERN_ERR "%s: driver_add_attrs(%s) failed\n",
			__func__, drv->name);	// 如果添加失败，打印错误日志
	}

	if (!drv->suppress_bind_attrs) {
		error = add_bind_files(drv);	// 如果不抑制绑定属性，添加绑定文件
		if (error) {
			/* Ditto */
			printk(KERN_ERR "%s: add_bind_files(%s) failed\n",
				__func__, drv->name);	// 如果添加失败，打印错误日志
		}
	}

	kobject_uevent(&priv->kobj, KOBJ_ADD);	// 发送 kobject 添加事件
	return 0;	// 成功完成，返回 0

out_unregister:
	kobject_put(&priv->kobj);	// 释放 kobject
	kfree(drv->p);	// 释放私有数据结构
	drv->p = NULL;	// 清空驱动的私有数据指针
out_put_bus:
	bus_put(bus);		// 释放对总线的引用
	return error;		// 返回错误码
}

/**
 * bus_remove_driver - delete driver from bus's knowledge.
 * @drv: driver.
 *
 * Detach the driver from the devices it controls, and remove
 * it from its bus's list of drivers. Finally, we drop the reference
 * to the bus we took in bus_add_driver().
 */
/**
 * bus_remove_driver - 从总线的知识库中删除驱动程序
 * @drv: 驱动程序。
 *
 * 分离该驱动程序控制的设备，并将其从所属总线的驱动列表中移除。
 * 最后，我们放弃在 bus_add_driver() 中获取的对总线的引用。
 */
void bus_remove_driver(struct device_driver *drv)
{
	if (!drv->bus)	// 检查驱动程序是否有关联的总线
		return;	// 如果没有，直接返回

	if (!drv->suppress_bind_attrs)	// 如果没有抑制绑定属性
		remove_bind_files(drv);	// 移除绑定文件
	driver_remove_attrs(drv->bus, drv);	// 移除驱动程序的属性
	driver_remove_file(drv, &driver_attr_uevent);	// 移除 uevent 属性文件
	klist_remove(&drv->p->knode_bus);	// 从总线的驱动列表中移除该驱动程序
	pr_debug("bus: '%s': remove driver %s\n", drv->bus->name, drv->name);	// 打印调试信息，表明正在移除驱动程序
	driver_detach(drv);	// 从它控制的所有设备上分离该驱动程序
	module_remove_driver(drv);	// 从其模块中移除该驱动程序
	kobject_put(&drv->p->kobj);	// 减少 kobject 的引用计数
	bus_put(drv->bus);	// 减少总线的引用计数
}

/* Helper for bus_rescan_devices's iter */
/**
 * bus_rescan_devices_helper - 辅助函数，用于总线重新扫描设备时的迭代
 * @dev: 正在处理的设备
 * @data: 未使用的参数，可用于传递额外数据
 *
 * 此函数用于在总线设备重新扫描过程中为尚未绑定驱动的设备尝试绑定驱动。
 * 对于需要特别处理的USB设备，会先锁定其父设备，以防并发操作。
 *
 * 返回: 如果操作成功，返回 0；如果有错误发生，返回负值错误代码。
 */
static int __must_check bus_rescan_devices_helper(struct device *dev,
						  void *data)
{
	int ret = 0;	// 初始化返回值为 0，表示无错误

	if (!dev->driver) {	// 如果设备尚未绑定驱动
		// 如果设备有父设备（如USB设备）
		if (dev->parent)	/* Needed for USB */
			device_lock(dev->parent);	// 锁定父设备以防并发操作
		ret = device_attach(dev);		// 尝试为设备绑定驱动
		if (dev->parent)
			device_unlock(dev->parent);	// 操作完成后解锁父设备
	}
	return ret < 0 ? ret : 0;	// 如果有错误发生返回错误代码，否则返回 0
}

/**
 * bus_rescan_devices - rescan devices on the bus for possible drivers
 * @bus: the bus to scan.
 *
 * This function will look for devices on the bus with no driver
 * attached and rescan it against existing drivers to see if it matches
 * any by calling device_attach() for the unbound devices.
 */
/**
 * bus_rescan_devices - 重新扫描总线上的设备以寻找可能的驱动
 * @bus: 要扫描的总线。
 *
 * 此函数将搜索总线上未绑定驱动的设备，并通过调用 device_attach()
 * 对这些未绑定的设备进行重新扫描，以查看是否匹配现有的驱动。
 */
int bus_rescan_devices(struct bus_type *bus)
{
	// 对总线上的每个设备执行 bus_rescan_devices_helper 函数，此函数尝试为未绑定驱动的设备绑定驱动
	return bus_for_each_dev(bus, NULL, NULL, bus_rescan_devices_helper);
}
EXPORT_SYMBOL_GPL(bus_rescan_devices);

/**
 * device_reprobe - remove driver for a device and probe for a new driver
 * @dev: the device to reprobe
 *
 * This function detaches the attached driver (if any) for the given
 * device and restarts the driver probing process.  It is intended
 * to use if probing criteria changed during a devices lifetime and
 * driver attachment should change accordingly.
 */
/**
 * device_reprobe - 为设备移除驱动并重新探测新驱动
 * @dev: 需要重新探测的设备
 *
 * 此函数会分离已附加的驱动程序（如果有）并重新启动驱动探测过程。
 * 这用于如果设备在其生命周期中的探测条件发生变化，
 * 并且相应地应更改驱动程序附加时的情况。
 */
int device_reprobe(struct device *dev)
{
	if (dev->driver) {	// 检查设备是否已绑定驱动
		// 如果设备有父设备（例如USB设备需要锁定父设备以防并发问题）
		if (dev->parent)        /* Needed for USB */
			device_lock(dev->parent);	// 锁定父设备
		device_release_driver(dev);	// 释放（分离）当前绑定的驱动
		if (dev->parent)
			device_unlock(dev->parent);	// 解锁父设备
	}
	return bus_rescan_devices_helper(dev, NULL);	// 调用辅助函数重新探测并尝试绑定驱动
}
EXPORT_SYMBOL_GPL(device_reprobe);

/**
 * find_bus - locate bus by name.
 * @name: name of bus.
 *
 * Call kset_find_obj() to iterate over list of buses to
 * find a bus by name. Return bus if found.
 *
 * Note that kset_find_obj increments bus' reference count.
 */
/**
 * find_bus - 根据名称查找总线。
 * @name: 总线的名称。
 *
 * 调用 kset_find_obj() 遍历总线列表以根据名称找到总线。
 * 如果找到，返回总线对象。
 *
 * 注意，kset_find_obj 会增加找到的总线的引用计数。
 */
#if 0
struct bus_type *find_bus(char *name)
{
	struct kobject *k = kset_find_obj(bus_kset, name);	 // 在 bus_kset 中查找具有指定名称的 kobject
	return k ? to_bus(k) : NULL;	// 如果找到 kobject，将其转换为 bus_type 结构，否则返回 NULL
}
#endif  /*  0  */


/**
 * bus_add_attrs - Add default attributes for this bus.
 * @bus: Bus that has just been registered.
 */
/**
 * bus_add_attrs - 为这个总线添加默认属性
 * @bus: 刚刚注册的总线
 *
 * 为新注册的总线添加一组预定义的属性。如果在添加过程中遇到错误，
 * 函数将撤销已经添加的属性，确保不留下不完整的状态。
 */
static int bus_add_attrs(struct bus_type *bus)
{
	int error = 0;	// 初始化错误码为0，表示无错误
	int i;

	if (bus->bus_attrs) {	// 检查总线是否定义了属性
		for (i = 0; attr_name(bus->bus_attrs[i]); i++) {	// 遍历所有属性
			error = bus_create_file(bus, &bus->bus_attrs[i]);	// 为总线创建属性文件
			if (error)
				goto err;	// 如果创建文件出现错误，跳到错误处理代码块
		}
	}
done:
	return error;	// 返回错误码，如果添加过程无误，则返回0
err:
	while (--i >= 0)	// 回滚，删除之前添加的所有属性文件
		bus_remove_file(bus, &bus->bus_attrs[i]);
	goto done;	// 完成回滚后跳转到 done 标签，返回错误码
}

/**
 * bus_remove_attrs - 从总线中移除属性
 * @bus: 需要移除属性的总线
 *
 * 此函数遍历总线定义的属性数组，并为每个属性调用 bus_remove_file 函数
 * 以从 sysfs 中移除相应的文件。这通常在总线被注销或需要清理时执行。
 */
static void bus_remove_attrs(struct bus_type *bus)
{
	int i;

	if (bus->bus_attrs) {	// 检查总线是否定义了属性
		for (i = 0; attr_name(bus->bus_attrs[i]); i++)	// 遍历所有属性
			bus_remove_file(bus, &bus->bus_attrs[i]);			// 移除每个属性对应的文件
	}
}

/**
 * klist_devices_get - 通过 klist 节点增加设备的引用计数
 * @n: 指向 klist_node 的指针，此节点关联到设备的私有数据
 *
 * 此函数从给定的 klist 节点中提取设备的私有数据，然后获取相应的设备对象，
 * 并增加该设备的引用计数。这样做可以确保在处理设备时，设备不会被意外释放。
 */
static void klist_devices_get(struct klist_node *n)
{
	struct device_private *dev_prv = to_device_private_bus(n);	// 从 klist_node 获取设备的私有数据
	struct device *dev = dev_prv->device;	// 从私有数据中获取关联的设备对象

	get_device(dev);	// 增加设备对象的引用计数
}

/**
 * klist_devices_put - 通过 klist 节点减少设备的引用计数
 * @n: 指向 klist_node 的指针，此节点关联到设备的私有数据
 *
 * 此函数从给定的 klist 节点中提取设备的私有数据，然后获取相应的设备对象，
 * 并减少该设备的引用计数。这是在设备不再被使用时进行的操作，有助于正确管理设备的生命周期。
 */
static void klist_devices_put(struct klist_node *n)
{
	struct device_private *dev_prv = to_device_private_bus(n);	// 从 klist_node 获取设备的私有数据
	struct device *dev = dev_prv->device;	// 从私有数据中获取关联的设备对象

	put_device(dev);	// 减少设备对象的引用计数
}

/**
 * bus_uevent_store - 通过 sysfs 接口处理对总线的 uevent 的写入操作
 * @bus: 相关的总线
 * @buf: 用户写入的数据缓冲区
 * @count: 写入数据的字节数
 *
 * 此函数处理从用户空间写入到总线的 uevent 属性的数据，
 * 并根据写入的内容触发相应的 kobject 事件。
 * 如果成功解析用户提供的动作字符串，会触发相应的 kobject 事件。
 *
 * 返回: 总是返回写入的字节数，表示成功处理了写入的数据。
 */
// bus_uevent_store 处理写入到 uevent sysfs 文件的数据。它读取用户空间提供的字符串，解析成 kobject 事件，并触发该事件
static ssize_t bus_uevent_store(struct bus_type *bus,
				const char *buf, size_t count)
{
	enum kobject_action action;	// 定义一个枚举变量来存储解析的动作

	if (kobject_action_type(buf, count, &action) == 0)	// 解析 buf 中的动作，检查是否有效
		kobject_uevent(&bus->p->subsys.kobj, action);			// 如果解析成功，触发对应的 kobject 事件
	return count;	// 返回处理的字节数
}
static BUS_ATTR(uevent, S_IWUSR, NULL, bus_uevent_store);	// 定义总线的 uevent 属性，仅写权限	

/**
 * bus_register - register a bus with the system.
 * @bus: bus.
 *
 * Once we have that, we registered the bus with the kobject
 * infrastructure, then register the children subsystems it has:
 * the devices and drivers that belong to the bus.
 */
/**
 * bus_register - 将一个总线注册到系统中
 * @bus: 总线对象。
 *
 * 注册过程包括使用 kobject 基础设施注册总线，
 * 然后注册它拥有的子系统：属于该总线的设备和驱动程序。
 */
int bus_register(struct bus_type *bus)
{
	int retval;  // 用于存储返回值
	struct bus_type_private *priv;  // 指向总线私有数据的指针

	priv = kzalloc(sizeof(struct bus_type_private), GFP_KERNEL);  // 为总线私有数据分配内存
	if (!priv)
		return -ENOMEM;  // 如果内存分配失败，返回内存不足的错误

	priv->bus = bus;  // 设置私有数据中的总线指针
	bus->p = priv;  // 将总线的私有数据指针设置为新分配的结构

	BLOCKING_INIT_NOTIFIER_HEAD(&priv->bus_notifier);  // 初始化总线的通知头

	retval = kobject_set_name(&priv->subsys.kobj, "%s", bus->name);  // 为总线的 kobject 设置名称
	if (retval)
		goto out;  // 如果设置名称失败，跳到错误处理代码

	priv->subsys.kobj.kset = bus_kset;		// 设置总线的 kset
	priv->subsys.kobj.ktype = &bus_ktype;	// 设置总线的 ktype
	priv->drivers_autoprobe = 1;					// 启用驱动自动探测

	retval = kset_register(&priv->subsys);	// 注册 kset
	if (retval)
		goto out;	// 如果注册失败，跳到错误处理代码

	retval = bus_create_file(bus, &bus_attr_uevent);	// 创建 uevent 文件
	if (retval)
		goto bus_uevent_fail;	// 如果创建失败，跳到创建 uevent 文件失败的处理代码

	priv->devices_kset = kset_create_and_add("devices", NULL,
						 &priv->subsys.kobj);	// 创建并添加设备 kset
	if (!priv->devices_kset) {	// 如果创建失败，跳到设备 kset 创建失败的处理代码
		retval = -ENOMEM;
		goto bus_devices_fail;
	}

	priv->drivers_kset = kset_create_and_add("drivers", NULL,
						 &priv->subsys.kobj);	// 创建并添加驱动 kset
	if (!priv->drivers_kset) {
		retval = -ENOMEM;
		goto bus_drivers_fail;	// 如果创建失败，跳到驱动 kset 创建失败的处理代码
	}

	klist_init(&priv->klist_devices, klist_devices_get, klist_devices_put);	// 初始化设备链表
	klist_init(&priv->klist_drivers, NULL, NULL);	// 初始化驱动链表

	retval = add_probe_files(bus);	// 添加探测文件
	if (retval)
		goto bus_probe_files_fail;	// 如果添加失败，跳到探测文件添加失败的处理代码

	retval = bus_add_attrs(bus);	// 添加总线属性
	if (retval)
		goto bus_attrs_fail;	// 如果添加失败，跳到属性添加失败的处理代码

	pr_debug("bus: '%s': registered\n", bus->name);	// 打印注册成功的调试信息
	return 0;	// 返回成功

// 错误处理部分
bus_attrs_fail:
	remove_probe_files(bus);
bus_probe_files_fail:
	kset_unregister(bus->p->drivers_kset);
bus_drivers_fail:
	kset_unregister(bus->p->devices_kset);
bus_devices_fail:
	bus_remove_file(bus, &bus_attr_uevent);
bus_uevent_fail:
	kset_unregister(&bus->p->subsys);
	kfree(bus->p);
out:
	bus->p = NULL;
	return retval;	// 返回错误代码
}
EXPORT_SYMBOL_GPL(bus_register);

/**
 * bus_unregister - remove a bus from the system
 * @bus: bus.
 *
 * Unregister the child subsystems and the bus itself.
 * Finally, we call bus_put() to release the refcount
 */
/**
 * bus_unregister - 从系统中移除一个总线
 * @bus: 总线对象。
 *
 * 注销与总线关联的子系统和总线本身。
 * 最后，调用 bus_put() 释放引用计数。
 */
void bus_unregister(struct bus_type *bus)
{
	pr_debug("bus: '%s': unregistering\n", bus->name);	// 打印调试信息，表明正在注销的总线名
	bus_remove_attrs(bus);	// 移除总线的属性
	remove_probe_files(bus);	// 移除总线的探测相关文件
	kset_unregister(bus->p->drivers_kset);	// 注销驱动的 kset
	kset_unregister(bus->p->devices_kset);	// 注销设备的 kset
	bus_remove_file(bus, &bus_attr_uevent);	// 移除总线的 uevent 文件
	kset_unregister(&bus->p->subsys);	// 注销总线的子系统 kset
	kfree(bus->p);	// 释放总线的私有数据结构
	bus->p = NULL;	// 将总线的私有数据指针置为 NULL
}
EXPORT_SYMBOL_GPL(bus_unregister);

/**
 * bus_register_notifier - 注册一个通知器到总线
 * @bus: 目标总线
 * @nb: 要注册的通知器
 *
 * 该函数注册一个通知器块到总线的通知链中。当总线上发生特定事件时，已注册的通知器可以被通知。
 * 返回: 成功时返回0，失败时返回错误代码。
 */
int bus_register_notifier(struct bus_type *bus, struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&bus->p->bus_notifier, nb);
}
EXPORT_SYMBOL_GPL(bus_register_notifier);

/**
 * bus_unregister_notifier - 从总线注销一个通知器
 * @bus: 目标总线
 * @nb: 要注销的通知器
 *
 * 该函数从总线的通知链中注销一个通知器块。这是在不再需要接收事件通知时进行的操作。
 * 返回: 成功时返回0，失败时返回错误代码。
 */
int bus_unregister_notifier(struct bus_type *bus, struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&bus->p->bus_notifier, nb);
}
EXPORT_SYMBOL_GPL(bus_unregister_notifier);

/**
 * bus_get_kset - 获取总线的 kset 对象
 * @bus: 目标总线
 *
 * 返回与总线关联的 kset 对象，通常用于管理和组织总线上的 kobjects。
 * 返回: 指向总线 kset 的指针。
 */
struct kset *bus_get_kset(struct bus_type *bus)
{
	return &bus->p->subsys;
}
EXPORT_SYMBOL_GPL(bus_get_kset);

/**
 * bus_get_device_klist - 获取总线的设备 klist
 * @bus: 目标总线
 *
 * 返回与总线关联的设备 klist，这是一个链表，用于追踪总线上的设备。
 * 返回: 指向总线设备 klist 的指针。
 */
struct klist *bus_get_device_klist(struct bus_type *bus)
{
	return &bus->p->klist_devices;
}
EXPORT_SYMBOL_GPL(bus_get_device_klist);

/*
 * Yes, this forcably breaks the klist abstraction temporarily.  It
 * just wants to sort the klist, not change reference counts and
 * take/drop locks rapidly in the process.  It does all this while
 * holding the lock for the list, so objects can't otherwise be
 * added/removed while we're swizzling.
 */
/*
 * 这段代码临时强制打破了 klist 的抽象。它的目的是对 klist 进行排序，
 * 而不是改变引用计数或在此过程中频繁地获取/释放锁。
 * 它在持有列表的锁的情况下完成所有这些操作，因此在我们调整顺序时，
 * 对象不能被添加或移除。
 */
/**
 * device_insertion_sort_klist - 对设备的 klist 进行插入排序
 * @a: 要插入的设备
 * @list: 设备列表的头部
 * @compare: 用于比较两个设备的函数
 *
 * 此函数将一个设备插入到已排序的 klist 中。它通过遍历列表并使用提供的比较函数找到正确的位置，
 * 然后将设备移动到该位置。
 */
static void device_insertion_sort_klist(struct device *a, struct list_head *list,
					int (*compare)(const struct device *a,
							const struct device *b))
{
	struct list_head *pos;  // 用于遍历 list 的位置指针
	struct klist_node *n;   // klist 的节点
	struct device_private *dev_prv;  // 设备的私有数据
	struct device *b;  // 比较中使用的另一个设备

	list_for_each(pos, list) {	// 遍历整个 list
		// 从 list 节点获取 klist_node
		n = container_of(pos, struct klist_node, n_node);
		dev_prv = to_device_private_bus(n);	// 从 klist_node 获取设备的私有数据
		b = dev_prv->device;	// 从私有数据获取设备对象
		if (compare(a, b) <= 0) {	// 如果 a 应该在 b 之前
			list_move_tail(&a->p->knode_bus.n_node,
				       &b->p->knode_bus.n_node);	// 将 a 移动到 b 的位置
			return;	// 完成插入，返回
		}
	}
	list_move_tail(&a->p->knode_bus.n_node, list);	// 如果 a 是最大的，移动到列表末尾
}

/**
 * bus_sort_breadthfirst - 使用广度优先方式对总线上的设备进行排序
 * @bus: 要排序的总线
 * @compare: 用于比较两个设备的函数
 *
 * 此函数用于对总线上的设备列表进行广度优先排序。它会先获取设备列表，
 * 然后使用提供的比较函数对列表中的每个设备进行排序，并将排序结果存储回原列表。
 */
void bus_sort_breadthfirst(struct bus_type *bus,
			   int (*compare)(const struct device *a,
					  const struct device *b))
{
	LIST_HEAD(sorted_devices);  // 初始化一个空的排序列表头
	struct list_head *pos, *tmp;  // 遍历设备列表时使用的位置和临时指针
	struct klist_node *n;  // 指向 klist_node 的指针
	struct device_private *dev_prv;  // 设备私有数据的指针
	struct device *dev;  // 设备的指针
	struct klist *device_klist;  // 指向设备 klist 的指针

	device_klist = bus_get_device_klist(bus);	// 获取总线的设备 klist

	spin_lock(&device_klist->k_lock);	// 对设备列表加锁，防止并发修改
	list_for_each_safe(pos, tmp, &device_klist->k_list) {	// 安全地遍历设备列表
		n = container_of(pos, struct klist_node, n_node);	// 从 list_head 获取 klist_node
		dev_prv = to_device_private_bus(n);	// 从 klist_node 获取设备的私有数据
		dev = dev_prv->device;	// 获取设备对象
		device_insertion_sort_klist(dev, &sorted_devices, compare);	// 将设备插入到排序列表中
	}
	list_splice(&sorted_devices, &device_klist->k_list);	// 将排序后的设备列表拼接回原设备 klist
	spin_unlock(&device_klist->k_lock);	// 解锁
}
EXPORT_SYMBOL_GPL(bus_sort_breadthfirst);

/**
 * __init buses_init - 初始化并创建总线系统的 kset
 *
 * 此函数在系统启动时调用，用于创建一个名为 "bus" 的 kset，
 * 该 kset 在系统内部用于管理所有注册的总线类型。
 *
 * 返回: 如果成功，返回 0；如果内存分配失败，返回 -ENOMEM。
 */
int __init buses_init(void)
{
	bus_kset = kset_create_and_add("bus", &bus_uevent_ops, NULL);	// 创建并添加一个名为 "bus" 的 kset
	if (!bus_kset)
		return -ENOMEM;	// 如果创建失败，返回内存不足错误
	return 0;	// 如果成功，返回 0
}
