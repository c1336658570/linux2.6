/*
 * drivers/base/dd.c - The core device/driver interactions.
 *
 * This file contains the (sometimes tricky) code that controls the
 * interactions between devices and drivers, which primarily includes
 * driver binding and unbinding.
 *
 * All of this code used to exist in drivers/base/bus.c, but was
 * relocated to here in the name of compartmentalization (since it wasn't
 * strictly code just for the 'struct bus_type'.
 *
 * Copyright (c) 2002-5 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2007-2009 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2007-2009 Novell Inc.
 *
 * This file is released under the GPLv2
 */

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/async.h>
#include <linux/pm_runtime.h>

#include "base.h"
#include "power/power.h"

// 定义一个函数，当设备成功绑定到驱动时调用
static void driver_bound(struct device *dev)
{
	// 检查设备是否已经绑定到一个驱动
	if (klist_node_attached(&dev->p->knode_driver)) {
		// 如果已绑定，则打印警告信息
		printk(KERN_WARNING "%s: device %s already bound\n",
			__func__, kobject_name(&dev->kobj));
		return;	// 终止函数执行
	}

	// 打印调试信息，表明设备成功绑定到驱动
	pr_debug("driver: '%s': %s: bound to device '%s'\n", dev_name(dev),
		 __func__, dev->driver->name);

	// 如果设备有所属的总线
	if (dev->bus)
		// 通知相关方设备已经绑定到驱动
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_BOUND_DRIVER, dev);

	// 将设备加入到驱动的设备列表中
	klist_add_tail(&dev->p->knode_driver, &dev->driver->p->klist_devices);
}

// 定义一个函数，用于给驱动和设备在sysfs中添加相互链接
static int driver_sysfs_add(struct device *dev)
{
	int ret;	// 用于存储返回值

	// 创建一个从驱动到设备的符号链接，链接名为设备的kobject名
	ret = sysfs_create_link(&dev->driver->p->kobj, &dev->kobj,
			  kobject_name(&dev->kobj));
	// 检查链接创建是否成功
	if (ret == 0) {
		 // 如果上一步成功，创建一个从设备到驱动的符号链接，链接名为"driver"
		ret = sysfs_create_link(&dev->kobj, &dev->driver->p->kobj,
					"driver");
		if (ret)	// 如果这一步链接创建失败
			// 删除之前创建的从驱动到设备的链接
			sysfs_remove_link(&dev->driver->p->kobj,
					kobject_name(&dev->kobj));
	}
	return ret;	// 返回操作结果，0 表示成功，非0 表示失败
}

// 定义一个函数，用于从sysfs中移除设备和驱动程序之间的符号链接
static void driver_sysfs_remove(struct device *dev)
{
	struct device_driver *drv = dev->driver;	// 获取设备关联的驱动程序

	if (drv) {	// 检查设备是否有关联的驱动程序
		// 如果设备有关联的驱动程序，从驱动程序的kobject中移除指向设备的符号链接
		sysfs_remove_link(&drv->p->kobj, kobject_name(&dev->kobj));
		// 从设备的kobject中移除指向驱动程序的符号链接
		sysfs_remove_link(&dev->kobj, "driver");
	}
}

/**
 * device_bind_driver - bind a driver to one device.
 * @dev: device.
 *
 * Allow manual attachment of a driver to a device.
 * Caller must have already set @dev->driver.
 *
 * Note that this does not modify the bus reference count
 * nor take the bus's rwsem. Please verify those are accounted
 * for before calling this. (It is ok to call with no other effort
 * from a driver's probe() method.)
 *
 * This function must be called with the device lock held.
 */
/**
 * device_bind_driver - 绑定一个驱动程序到一个设备。
 * @dev: 设备。
 *
 * 允许手动将一个驱动程序附加到一个设备。
 * 调用者必须已经设置了 @dev->driver。
 *
 * 注意，这个操作不会修改总线的引用计数也不会获取总线的读写信号量。
 * 请在调用此函数之前确认这些事项已经处理妥当。
 * （从驱动程序的 probe() 方法中调用此函数是可以的，无需其他操作。）
 *
 * 这个函数必须在持有设备锁的情况下调用。
 */
// 定义一个函数，用于手动将驱动程序绑定到一个设备
int device_bind_driver(struct device *dev)
{
	int ret;	// 用于存储函数返回值

	ret = driver_sysfs_add(dev);	// 添加sysfs链接，将设备与其驱动程序在sysfs中关联起来
	if (!ret)	// 如果添加sysfs链接成功
		driver_bound(dev);	// 调用函数标记设备已绑定到驱动程序
	return ret;	// 返回操作结果，0为成功，非0为失败
}
EXPORT_SYMBOL_GPL(device_bind_driver);

// 定义一个原子变量用于跟踪当前正在进行的probe操作的数量
static atomic_t probe_count = ATOMIC_INIT(0);
// 声明一个等待队列，用于处理probe操作的同步
static DECLARE_WAIT_QUEUE_HEAD(probe_waitqueue);

// 定义一个函数，用于实际尝试将设备和驱动程序绑定
static int really_probe(struct device *dev, struct device_driver *drv)
{
	int ret = 0;	 // 用于存储函数执行结果的变量

	atomic_inc(&probe_count);	// 增加正在进行的probe操作的计数
	// 打印调试信息，显示正在进行的probe操作
	pr_debug("bus: '%s': %s: probing driver %s with device %s\n",
		 drv->bus->name, __func__, drv->name, dev_name(dev));
	WARN_ON(!list_empty(&dev->devres_head));	// 如果设备的资源列表不为空，则发出警告

	dev->driver = drv;	// 将设备的driver字段设置为当前驱动程序
	if (driver_sysfs_add(dev)) {	// 在sysfs中为设备添加驱动程序相关的链接
		printk(KERN_ERR "%s: driver_sysfs_add(%s) failed\n",
			__func__, dev_name(dev));	// 如果添加失败，打印错误信息
		goto probe_failed;	// 跳转到错误处理部分
	}

	if (dev->bus->probe) {	// 如果总线提供了probe函数
		ret = dev->bus->probe(dev);	// 调用总线的probe函数
		if (ret)	// 如果probe失败
			goto probe_failed;	// 跳转到错误处理部分
	} else if (drv->probe) {	// 如果驱动程序提供了probe函数
		ret = drv->probe(dev);	// 调用驱动程序的probe函数
		if (ret)	// 如果probe失败
			goto probe_failed;	// 跳转到错误处理部分
	}

	driver_bound(dev);	// 标记设备已绑定到驱动程序
	ret = 1;	// 设置返回值为1，表示绑定成功
	pr_debug("bus: '%s': %s: bound device %s to driver %s\n",
		 drv->bus->name, __func__, dev_name(dev), drv->name);	// 打印绑定成功的调试信息
	goto done;	// 跳转到函数结束部分

probe_failed:
	devres_release_all(dev);	// 释放设备分配的所有资源
	driver_sysfs_remove(dev);	// 移除sysfs中的设备驱动程序链接
	dev->driver = NULL;	// 清除设备的driver字段

	if (ret != -ENODEV && ret != -ENXIO) {
		/* driver matched but the probe failed */
		// 如果错误不是因为设备不存在或设备不可访问
		printk(KERN_WARNING
		       "%s: probe of %s failed with error %d\n",
		       drv->name, dev_name(dev), ret);	// 打印probe失败的警告信息
	}
	/*
	 * Ignore errors returned by ->probe so that the next driver can try
	 * its luck.
	 */
	ret = 0;	// 设置返回值为0，表示绑定未成功，但不阻止其他驱动尝试
done:
	atomic_dec(&probe_count);	// 减少正在进行的probe操作的计数
	wake_up(&probe_waitqueue);	// 唤醒在等待队列上的进程
	return ret;	// 返回操作结果
}

/**
 * driver_probe_done
 * Determine if the probe sequence is finished or not.
 *
 * Should somehow figure out how to use a semaphore, not an atomic variable...
 */
/**
 * driver_probe_done - 判断驱动探测是否完成
 *
 * 这个函数应该考虑使用信号量来实现，而不是使用原子变量...
 */
int driver_probe_done(void)
{
	pr_debug("%s: probe_count = %d\n", __func__,
		 atomic_read(&probe_count));	// 打印调试信息，显示当前的probe计数
	if (atomic_read(&probe_count))	// 读取probe_count原子变量的值，如果值不为0，则表示仍有探测活动正在进行
		return -EBUSY;	// 返回-EBUSY错误，表示设备或资源忙
	// 如果probe_count为0，表示所有探测活动已经完成
	return 0; // 返回0表示探测完成
}

/**
 * wait_for_device_probe
 * Wait for device probing to be completed.
 */
/**
 * wait_for_device_probe - 等待设备探测完成
 *
 * 此函数会阻塞直到所有已知的设备完成他们的探测过程。
 */
void wait_for_device_probe(void)
{
	/* wait for the known devices to complete their probing */
	// 使用wait_event宏等待，直到probe_count变为0。这个宏会阻塞当前线程，
	// 直到指定的条件（这里是probe_count == 0）成立。
	wait_event(probe_waitqueue, atomic_read(&probe_count) == 0);
	// 调用async_synchronize_full函数来确保所有异步调用的完成。
	// 这是为了确保在设备探测阶段可能触发的所有异步任务都已经完成，
	// 比如可能有的异步初始化或配置任务。
	async_synchronize_full();
}
EXPORT_SYMBOL_GPL(wait_for_device_probe);

/**
 * driver_probe_device - attempt to bind device & driver together
 * @drv: driver to bind a device to
 * @dev: device to try to bind to the driver
 *
 * This function returns -ENODEV if the device is not registered,
 * 1 if the device is bound successfully and 0 otherwise.
 *
 * This function must be called with @dev lock held.  When called for a
 * USB interface, @dev->parent lock must be held as well.
 */
/**
 * driver_probe_device - 尝试将设备和驱动程序绑定在一起
 * @drv: 要绑定的驱动程序
 * @dev: 尝试绑定到驱动程序的设备
 *
 * 如果设备未注册，此函数返回 -ENODEV，
 * 如果设备成功绑定，返回 1，
 * 否则返回 0。
 *
 * 调用此函数时，必须持有 @dev 的锁。
 * 当为 USB 接口调用时，还必须持有 @dev->parent 的锁。
 */
int driver_probe_device(struct device_driver *drv, struct device *dev)
{
	int ret = 0;

	// 检查设备是否已注册，如果没有，返回 -ENODEV
	if (!device_is_registered(dev))
		return -ENODEV;

	// 打印调试信息，说明正在匹配设备和驱动
	pr_debug("bus: '%s': %s: matched device %s with driver %s\n",
		 drv->bus->name, __func__, dev_name(dev), drv->name);

	// 管理设备的电源状态，确保在探测过程中设备不会被挂起
	pm_runtime_get_noresume(dev);
	pm_runtime_barrier(dev);
	// 调用 really_probe 尝试实际绑定设备和驱动
	ret = really_probe(dev, drv);
	// 在探测完成后，同步放回设备的电源状态
	pm_runtime_put_sync(dev);

	return ret;
}

/**
 * __device_attach - 尝试将驱动程序和设备绑定在一起
 * @drv: 需要绑定的驱动程序
 * @data: 指向设备的指针
 *
 * 此函数尝试将指定的设备与驱动程序绑定。
 * 首先检查驱动程序是否与设备匹配，
 * 如果匹配，则调用 driver_probe_device 来尝试绑定。
 * 如果不匹配，返回 0。
 *
 * 返回 driver_probe_device 的返回值，或者在不匹配时返回 0。
 */
static int __device_attach(struct device_driver *drv, void *data)
{
	// 将 void* 类型的 data 强制转换为设备结构体指针
	struct device *dev = data;

	// 调用 driver_match_device 检查驱动程序是否与设备匹配
	if (!driver_match_device(drv, dev))
		return 0;

	// 如果匹配，调用 driver_probe_device 尝试绑定设备与驱动程序
	return driver_probe_device(drv, dev);
}

/**
 * device_attach - try to attach device to a driver.
 * @dev: device.
 *
 * Walk the list of drivers that the bus has and call
 * driver_probe_device() for each pair. If a compatible
 * pair is found, break out and return.
 *
 * Returns 1 if the device was bound to a driver;
 * 0 if no matching driver was found;
 * -ENODEV if the device is not registered.
 *
 * When called for a USB interface, @dev->parent lock must be held.
 */
/**
 * device_attach - 尝试将设备绑定到驱动程序。
 * @dev: 设备。
 *
 * 遍历该总线上所有的驱动程序，并为每一对调用 driver_probe_device()。
 * 如果找到兼容的对，则终止并返回。
 *
 * 如果设备成功绑定到一个驱动程序，则返回 1；
 * 如果没有找到匹配的驱动程序，则返回 0；
 * 如果设备未注册，则返回 -ENODEV。
 *
 * 当为 USB 接口调用时，必须持有 @dev->parent 锁。
 */
int device_attach(struct device *dev)
{
	int ret = 0;

	device_lock(dev);	// 锁定设备，确保在绑定过程中设备状态不会被其他进程改变
	if (dev->driver) {	// 如果设备已经有了驱动程序
		ret = device_bind_driver(dev);	// 尝试绑定设备到该驱动程序
		if (ret == 0)	// 如果绑定成功
			ret = 1;	// 设置返回值为 1，表示成功绑定
		else {	// 如果绑定失败
			dev->driver = NULL;	// 清除设备的驱动程序指针
			ret = 0;	// 设置返回值为 0，表示未绑定
		}
	} else {	// 如果设备没有预绑定的驱动程序
		pm_runtime_get_noresume(dev);	// 获取设备的电源管理引用，不允许设备在此期间自动挂起
		ret = bus_for_each_drv(dev->bus, NULL, dev, __device_attach);	// 遍历总线上的所有驱动，尝试进行绑定
		pm_runtime_put_sync(dev);	// 释放之前获取的电源管理引用，并同步设备的电源状态
	}
	device_unlock(dev);	// 解锁设备
	return ret;	// 返回结果
}
EXPORT_SYMBOL_GPL(device_attach);

/**
 * __driver_attach - 尝试将设备绑定到驱动程序
 * @dev: 设备
 * @data: 传入的驱动程序指针
 *
 * 锁定设备并尝试将其绑定到驱动程序。这里忽略错误并始终返回 0，因为我们需要继续尝试
 * 绑定设备，有些驱动程序如果不支持该设备会返回错误。
 *
 * 如果有错误，driver_probe_device() 将会输出警告。
 */
static int __driver_attach(struct device *dev, void *data)
{
	struct device_driver *drv = data;	// 将传入的数据转换为设备驱动程序指针

	/*
	 * Lock device and try to bind to it. We drop the error
	 * here and always return 0, because we need to keep trying
	 * to bind to devices and some drivers will return an error
	 * simply if it didn't support the device.
	 *
	 * driver_probe_device() will spit a warning if there
	 * is an error.
	 */
	/*
	 * 锁定设备并尝试将其绑定到驱动程序。我们在这里忽略错误，并始终返回0，
	 * 因为我们需要持续尝试绑定设备，有些驱动程序如果不支持该设备会返回错误。
	 *
	 * 如果存在错误，driver_probe_device() 会发出警告。
	 */

	if (!driver_match_device(drv, dev))	// 检查设备和驱动程序是否匹配。
		return 0;	 // 如果不匹配，返回 0 继续下一个设备

	/*
	 * 如果设备有父设备（例如 USB 设备需要），则锁定父设备。
	 */
	if (dev->parent)	/* Needed for USB */
		device_lock(dev->parent);
	// 锁定当前设备，准备绑定操作。
	device_lock(dev);
	// 如果设备当前没有绑定驱动程序，则尝试绑定。
	if (!dev->driver)
		driver_probe_device(drv, dev);
	// 解锁当前设备。
	device_unlock(dev);
	// 如果锁定了父设备，现在解锁父设备。
	if (dev->parent)
		device_unlock(dev->parent);

	return 0;	// 总是返回 0 继续处理列表中的下一个设备
}

/**
 * driver_attach - try to bind driver to devices.
 * @drv: driver.
 *
 * Walk the list of devices that the bus has on it and try to
 * match the driver with each one.  If driver_probe_device()
 * returns 0 and the @dev->driver is set, we've found a
 * compatible pair.
 */
/**
 * driver_attach - 尝试将驱动程序绑定到设备。
 * @drv: 驱动程序。
 *
 * 遍历该驱动程序所属总线上的设备列表，并尝试将驱动程序与每个设备匹配。
 * 如果 driver_probe_device() 返回 0 并且 @dev->driver 被设置，则表示
 * 找到了兼容的设备和驱动程序对。
 */
int driver_attach(struct device_driver *drv)
{
	// 调用 bus_for_each_dev 函数，遍历驱动所属的总线上的每个设备，
	// 并尝试通过 __driver_attach 函数将 drv 绑定到这些设备上。
	return bus_for_each_dev(drv->bus, NULL, drv, __driver_attach);
}
EXPORT_SYMBOL_GPL(driver_attach);

/*
 * __device_release_driver() must be called with @dev lock held.
 * When called for a USB interface, @dev->parent lock must be held as well.
 */
/*
 * __device_release_driver() 必须在持有 @dev 锁的情况下调用。
 * 当用于 USB 接口时，还必须持有 @dev->parent 的锁。
 */
static void __device_release_driver(struct device *dev)
{
	struct device_driver *drv;

	drv = dev->driver;	// 获取设备当前绑定的驱动程序
	if (drv) {	// 如果存在驱动程序
		pm_runtime_get_noresume(dev);	// 增加设备的 PM 使用计数，阻止设备进入低功耗状态
		pm_runtime_barrier(dev);			// 确保设备不在异步转换到低功耗状态

		driver_sysfs_remove(dev);			// 从系统文件中移除与该设备相关的驱动程序链接

		if (dev->bus)	// 如果设备属于某个总线
			blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
						     BUS_NOTIFY_UNBIND_DRIVER,
						     dev);	// 通知相关的观察者该设备即将与驱动程序解绑

		if (dev->bus && dev->bus->remove)
			dev->bus->remove(dev);	// 调用总线的 remove 方法
		else if (drv->remove)
			drv->remove(dev);	// 调用驱动程序的 remove 方法
		devres_release_all(dev);	// 释放设备资源
		dev->driver = NULL;	// 清空设备的驱动程序字段
		klist_remove(&dev->p->knode_driver);	// 从驱动程序的设备列表中移除设备
		if (dev->bus)	// 如果设备属于某个总线
			blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
						     BUS_NOTIFY_UNBOUND_DRIVER,
						     dev);	// 通知相关的观察者该设备已与驱动程序解绑

		pm_runtime_put_sync(dev);	// 递减设备的 PM 使用计数，并同步地执行 PM 状态改变
	}
}

/**
 * device_release_driver - manually detach device from driver.
 * @dev: device.
 *
 * Manually detach device from driver.
 * When called for a USB interface, @dev->parent lock must be held.
 */
/**
 * device_release_driver - 手动从驱动程序中解除设备。
 * @dev: 设备。
 *
 * 手动将设备从其驱动程序中解绑。
 * 如果是为 USB 接口调用此函数，必须持有 @dev->parent 的锁。
 */
void device_release_driver(struct device *dev)
{
	/*
	 * If anyone calls device_release_driver() recursively from
	 * within their ->remove callback for the same device, they
	 * will deadlock right here.
	 */
	/*
   * 如果有人从其 ->remove 回调中递归调用 device_release_driver()
   * 针对相同的设备，他们会在这里死锁。
   */
	device_lock(dev);	// 锁定设备，防止其他操作同时修改设备状态
	__device_release_driver(dev);	// 调用内部函数来执行解绑驱动的实际操作
	device_unlock(dev);	// 解锁设备
}
EXPORT_SYMBOL_GPL(device_release_driver);

/**
 * driver_detach - detach driver from all devices it controls.
 * @drv: driver.
 */
/**
 * driver_detach - 从其控制的所有设备中分离驱动程序。
 * @drv: 驱动程序。
 */
void driver_detach(struct device_driver *drv)
{
	struct device_private *dev_prv;
	struct device *dev;

	for (;;) {	// 无限循环，直到没有设备与驱动关联
		spin_lock(&drv->p->klist_devices.k_lock);	// 锁定设备列表，防止并发访问
		if (list_empty(&drv->p->klist_devices.k_list)) {	// 如果设备列表为空
			spin_unlock(&drv->p->klist_devices.k_lock);	// 解锁设备列表
			break;	// 退出循环
		}
		// 获取设备列表中最后一个设备的私有数据
		dev_prv = list_entry(drv->p->klist_devices.k_list.prev,
				     struct device_private,
				     knode_driver.n_node);
		dev = dev_prv->device;	// 从私有数据中获取设备对象
		get_device(dev);	// 增加设备的引用计数
		spin_unlock(&drv->p->klist_devices.k_lock);	// 解锁设备列表

		// 如果设备有父设备，可能是USB设备，需要特别处理
		if (dev->parent)	/* Needed for USB */
			device_lock(dev->parent);	// 锁定父设备
		device_lock(dev);	// 锁定设备
		if (dev->driver == drv)	// 确认设备当前关联的驱动是否是目标驱动
			__device_release_driver(dev);	// 调用内部函数，分离设备和驱动
		device_unlock(dev);	// 解锁设备
		if (dev->parent)
			device_unlock(dev->parent);	// 解锁父设备
		put_device(dev);	// 减少设备的引用计数
	}
}

/*
 * These exports can't be _GPL due to .h files using this within them, and it
 * might break something that was previously working...
 */
/*
 * 这些导出函数不能使用 _GPL，因为有些 .h 文件中使用了这些函数，
 * 如果改变它们的导出方式，可能会破坏之前能正常工作的代码...
 */
// 从设备结构中获取存储的私有数据
void *dev_get_drvdata(const struct device *dev)
{
	// 确保设备结构及其私有数据指针存在
	if (dev && dev->p)
		// 返回私有数据
		return dev->p->driver_data;
	return NULL;	// 如果设备结构不存在或无私有数据指针，则返回 NULL
}
EXPORT_SYMBOL(dev_get_drvdata);

// 设置设备结构中的私有数据
void dev_set_drvdata(struct device *dev, void *data)
{
	int error;	// 用于捕捉错误码

	if (!dev)	// 如果设备结构不存在，则直接返回
		return;
	// 如果设备结构中没有私有数据结构
	if (!dev->p) {
		// 初始化设备的私有数据结构
		error = device_private_init(dev);
		if (error)	// 如果初始化失败
			return;	// 直接返回
	}
	// 将传入的数据设置为设备的私有数据
	dev->p->driver_data = data;
}
EXPORT_SYMBOL(dev_set_drvdata);
