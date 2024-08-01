/*
 * platform.c - platform 'pseudo' bus for legacy devices
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 *
 * This file is released under the GPLv2
 *
 * Please see Documentation/driver-model/platform.txt for more
 * information.
 */

#include <linux/string.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/bootmem.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>

#include "base.h"

// 定义一个宏，将device_driver转换为platform_driver。这是一个常用的技巧，使用container_of来从包含结构体中获取到特定类型的指针。
#define to_platform_driver(drv)	(container_of((drv), struct platform_driver, \
				 driver))

// 定义并导出一个名为"platform"的设备结构体。这个结构体代表平台总线，是平台设备的根。
struct device platform_bus = {
	.init_name	= "platform",
};
EXPORT_SYMBOL_GPL(platform_bus);

/**
 * platform_get_resource - get a resource for a device
 * @dev: platform device
 * @type: resource type
 * @num: resource index
 */
/**
 * platform_get_resource - 为设备获取资源
 * @dev: 平台设备
 * @type: 资源类型
 * @num: 资源索引
 */
// 获取指定类型和索引的资源。这个函数通过遍历设备的资源数组来实现。
struct resource *platform_get_resource(struct platform_device *dev,
				       unsigned int type, unsigned int num)
{
	int i; // 循环索引

	// 遍历设备的所有资源
	for (i = 0; i < dev->num_resources; i++) {
		struct resource *r = &dev->resource[i]; // 获取当前资源

		// 如果当前资源的类型匹配，并且索引符合要求
		if (type == resource_type(r) && num-- == 0)
			return r; // 返回匹配的资源
	}
	return NULL; // 如果没有找到匹配的资源，返回NULL
}
EXPORT_SYMBOL_GPL(platform_get_resource);

/**
 * platform_get_irq - get an IRQ for a device
 * @dev: platform device
 * @num: IRQ number index
 */
/**
 * platform_get_irq - 为设备获取中断号
 * @dev: 平台设备
 * @num: 中断号索引
 */
// 函数定义，接受平台设备指针和中断号索引作为参数。
int platform_get_irq(struct platform_device *dev, unsigned int num)
{
	// 调用platform_get_resource函数尝试获取指定索引的IRQ资源。
	struct resource *r = platform_get_resource(dev, IORESOURCE_IRQ, num);

	// 如果资源存在，则返回资源的起始地址，即中断号；如果资源不存在，返回-ENXIO（无此设备或地址）。
	return r ? r->start : -ENXIO;
}
EXPORT_SYMBOL_GPL(platform_get_irq);

/**
 * platform_get_resource_byname - get a resource for a device by name
 * @dev: platform device
 * @type: resource type
 * @name: resource name
 */
/**
 * platform_get_resource_byname - 根据名称获取设备资源
 * @dev: 平台设备
 * @type: 资源类型
 * @name: 资源名称
 */
// 定义函数，接受平台设备指针、资源类型和资源名称作为参数。
struct resource *platform_get_resource_byname(struct platform_device *dev,
					      unsigned int type,
					      const char *name)
{
	int i;	// 用于循环遍历设备的所有资源

	// 遍历设备的所有资源
	for (i = 0; i < dev->num_resources; i++) {
		struct resource *r = &dev->resource[i];	// 获取当前资源

		// 检查资源类型是否匹配，并且资源名称是否与给定名称相同
		if (type == resource_type(r) && !strcmp(r->name, name))
			return r;	// 如果匹配，返回当前资源
	}
	return NULL;	// 如果没有找到匹配的资源，返回NULL
}
EXPORT_SYMBOL_GPL(platform_get_resource_byname);

/**
 * platform_get_irq - get an IRQ for a device
 * @dev: platform device
 * @name: IRQ name
 */
/**
 * platform_get_irq_byname - 根据名称获取设备的IRQ
 * @dev: 平台设备
 * @name: IRQ名称
 */
// 定义函数，接受平台设备指针和IRQ的名称作为参数。
int platform_get_irq_byname(struct platform_device *dev, const char *name)
{
	// 调用platform_get_resource_byname()函数来获取名为name的IRQ资源
	struct resource *r = platform_get_resource_byname(dev, IORESOURCE_IRQ,
							  name);

	// 如果获取到资源，则返回资源的起始地址，通常是IRQ编号；如果未找到资源，则返回-ENXIO（设备不存在或不可用错误）
	return r ? r->start : -ENXIO;
}
EXPORT_SYMBOL_GPL(platform_get_irq_byname);

/**
 * platform_add_devices - add a numbers of platform devices
 * @devs: array of platform devices to add
 * @num: number of platform devices in array
 */
/**
 * platform_add_devices - 添加一组平台设备
 * @devs: 要添加的平台设备数组
 * @num: 数组中平台设备的数量
 */
// 定义函数，接受一个平台设备指针数组和数组中设备的数量。
int platform_add_devices(struct platform_device **devs, int num)
{
	int i, ret = 0; // 初始化索引和返回值

	// 遍历设备数组
	for (i = 0; i < num; i++) {
		// 尝试注册当前设备，将返回值存储在ret中
		ret = platform_device_register(devs[i]);
		if (ret) { // 如果注册失败
			// 回滚之前注册的所有设备
			while (--i >= 0)
				platform_device_unregister(devs[i]);
			break; // 跳出循环
		}
	}

	return ret; // 返回最后操作的结果，成功为0，失败为错误码
}
EXPORT_SYMBOL_GPL(platform_add_devices);

struct platform_object {
	struct platform_device pdev;  // 嵌入的平台设备结构体
	char name[1];  // 灵活数组成员，用于存储设备名称
};

/**
 * platform_device_put - destroy a platform device
 * @pdev: platform device to free
 *
 * Free all memory associated with a platform device.  This function must
 * _only_ be externally called in error cases.  All other usage is a bug.
 */
/**
 * platform_device_put - 销毁一个平台设备
 * @pdev: 要释放的平台设备
 *
 * 释放与平台设备相关联的所有内存。此函数仅应在错误处理情况下被外部调用。
 * 在所有其他情况下使用都是错误的。
 */
// 定义函数，接受一个平台设备指针
void platform_device_put(struct platform_device *pdev)
{
	if (pdev)	// 如果设备指针非空
		put_device(&pdev->dev);	// 调用put_device来减少设备的引用计数
}
EXPORT_SYMBOL_GPL(platform_device_put);

// 定义静态函数platform_device_release，它接受一个设备结构体指针
static void platform_device_release(struct device *dev)
{
	// 使用container_of宏从设备的指针获取包含它的平台对象的指针
	struct pla1tform_object *pa = container_of(dev, struct platform_object,
						  pdev.dev);

	kfree(pa->pdev.dev.platform_data);	// 释放平台设备中存储的平台数据
	kfree(pa->pdev.resource);	// 释放平台设备相关的资源数组
	kfree(pa);	// 最后释放平台设备对象本身
}

/**
 * platform_device_alloc - create a platform device
 * @name: base name of the device we're adding
 * @id: instance id
 *
 * Create a platform device object which can have other objects attached
 * to it, and which will have attached objects freed when it is released.
 */
/**
 * 创建一个平台设备
 * @name: 我们正在添加的设备的基础名称
 * @id: 实例标识
 *
 * 创建一个平台设备对象，可以附加其他对象，释放时也将释放这些附加的对象。
 */
// 定义用于创建平台设备的函数
struct platform_device *platform_device_alloc(const char *name, int id)
{
	// 定义指向平台对象的指针
	struct platform_object *pa;

	// 为平台对象及设备名称分配内存
	pa = kzalloc(sizeof(struct platform_object) + strlen(name), GFP_KERNEL);
	if (pa) {
		// 将设备名称复制到平台对象的名称字段
		strcpy(pa->name, name);
		// 设置平台设备的名称指针
		pa->pdev.name = pa->name;
		// 设置平台设备的实例ID
		pa->pdev.id = id;
		// 初始化平台设备对象
		device_initialize(&pa->pdev.dev);
		// 设置平台设备的释放函数
		pa->pdev.dev.release = platform_device_release;
	}

	// 如果平台对象被成功创建，则返回平台设备的指针，否则返回NULL
	return pa ? &pa->pdev : NULL;
}
EXPORT_SYMBOL_GPL(platform_device_alloc);

/**
 * platform_device_add_resources - add resources to a platform device
 * @pdev: platform device allocated by platform_device_alloc to add resources to
 * @res: set of resources that needs to be allocated for the device
 * @num: number of resources
 *
 * Add a copy of the resources to the platform device.  The memory
 * associated with the resources will be freed when the platform device is
 * released.
 */
/**
 * 为平台设备添加资源
 * @pdev: 被 platform_device_alloc 分配，用于添加资源的平台设备
 * @res: 设备需要分配的资源集
 * @num: 资源的数量
 * 
 * 将资源的副本添加到平台设备中。当平台设备被释放时，与资源相关联的内存将被释放。
 */
// 定义一个函数，用于给平台设备添加资源
int platform_device_add_resources(struct platform_device *pdev,
				  struct resource *res, unsigned int num)
{
	// 定义一个指向资源的指针
	struct resource *r;

	// 为资源数组分配内存
	r = kmalloc(sizeof(struct resource) * num, GFP_KERNEL);
	if (r) {
		// 如果内存分配成功，将传入的资源数组复制到新分配的内存中
		memcpy(r, res, sizeof(struct resource) * num);
		// 将平台设备的资源指针设置为新分配的资源数组
		pdev->resource = r;
		// 设置平台设备的资源数量
		pdev->num_resources = num;
	}
	// 如果资源分配成功，返回0；否则返回-ENOMEM（内存不足错误）
	return r ? 0 : -ENOMEM;
}
EXPORT_SYMBOL_GPL(platform_device_add_resources);

/**
 * platform_device_add_data - add platform-specific data to a platform device
 * @pdev: platform device allocated by platform_device_alloc to add resources to
 * @data: platform specific data for this platform device
 * @size: size of platform specific data
 *
 * Add a copy of platform specific data to the platform device's
 * platform_data pointer.  The memory associated with the platform data
 * will be freed when the platform device is released.
 */
/**
 * platform_device_add_data - 向平台设备添加平台特定数据
 * @pdev: 通过 platform_device_alloc 分配的平台设备
 * @data: 此平台设备的平台特定数据
 * @size: 平台特定数据的大小
 *
 * 向平台设备的 platform_data 指针添加一份平台特定数据的副本。
 * 当平台设备被释放时，与平台数据相关联的内存也会被释放。
 */
int platform_device_add_data(struct platform_device *pdev, const void *data,
			     size_t size)
{
	// 通过 kmemdup 函数复制传入的数据，返回新分配的内存指针
	void *d = kmemdup(data, size, GFP_KERNEL);

	// 检查数据是否复制成功
	if (d) {
		// 如果复制成功，设置平台设备的 platform_data 指针为新的数据副本
		pdev->dev.platform_data = d;
		return 0;	// 返回0表示成功
	}
	return -ENOMEM;	// 如果数据复制失败，返回 -ENOMEM 表示内存不足
}
EXPORT_SYMBOL_GPL(platform_device_add_data);

/**
 * platform_device_add - add a platform device to device hierarchy
 * @pdev: platform device we're adding
 *
 * This is part 2 of platform_device_register(), though may be called
 * separately _iff_ pdev was allocated by platform_device_alloc().
 */
/**
 * platform_device_add - 将平台设备添加到设备层级结构中
 * @pdev: 正在添加的平台设备
 *
 * 这是 platform_device_register() 的第二部分，但如果 pdev 是通过 platform_device_alloc() 分配的，
 * 可以单独调用。
 */
int platform_device_add(struct platform_device *pdev)
{
	int i, ret = 0;	// 定义索引变量和返回状态变量，初始返回值设为0表示成功

	if (!pdev)	// 如果设备指针为空，返回无效参数错误
		return -EINVAL;

	if (!pdev->dev.parent)	// 如果设备没有父设备，将平台总线作为其父设备
		pdev->dev.parent = &platform_bus;

	pdev->dev.bus = &platform_bus_type;	// 设置设备所属的总线类型为平台总线

	if (pdev->id != -1)	// 如果设备ID不是-1，设置设备名称为 名称.id 格式
		dev_set_name(&pdev->dev, "%s.%d", pdev->name,  pdev->id);
	else	// 否则，设备名称就是设备提供的名称
		dev_set_name(&pdev->dev, "%s", pdev->name);

	for (i = 0; i < pdev->num_resources; i++) {	// 遍历设备资源，准备将它们注册到系统中
		struct resource *p, *r = &pdev->resource[i];	// 获取当前资源

		if (r->name == NULL)	// 如果资源没有名称，将设备名称设置为资源名称
			r->name = dev_name(&pdev->dev);

		p = r->parent;	// 获取资源的父资源
		if (!p) {	// 如果没有父资源，根据资源类型设置父资源
			if (resource_type(r) == IORESOURCE_MEM)
				p = &iomem_resource;	// 内存资源使用iomem_resource作为父资源
			else if (resource_type(r) == IORESOURCE_IO)
				p = &ioport_resource;	// IO资源使用ioport_resource作为父资源
		}

		// 尝试将资源插入到资源树中，如果失败，输出错误信息并设置返回状态
		if (p && insert_resource(p, r)) {	// 将资源插入到父资源中，如果失败打印错误信息
			printk(KERN_ERR
			       "%s: failed to claim resource %d\n",
			       dev_name(&pdev->dev), i);
			ret = -EBUSY;
			goto failed;
		}
	}

	pr_debug("Registering platform device '%s'. Parent at %s\n",	// 打印调试信息
		 dev_name(&pdev->dev), dev_name(pdev->dev.parent));

	// 注册设备，如果注册失败，执行失败处理
	ret = device_add(&pdev->dev);	// 添加设备
	if (ret == 0)	// 如果设备添加成功，返回0
		return ret;

 failed:
	while (--i >= 0) {	// 如果添加失败，释放已经分配的资源
		struct resource *r = &pdev->resource[i];
		unsigned long type = resource_type(r);

		if (type == IORESOURCE_MEM || type == IORESOURCE_IO)
			release_resource(r);	// 释放内存或IO资源
	}

	return ret;	// 返回结果
}
EXPORT_SYMBOL_GPL(platform_device_add);

/**
 * platform_device_del - remove a platform-level device
 * @pdev: platform device we're removing
 *
 * Note that this function will also release all memory- and port-based
 * resources owned by the device (@dev->resource).  This function must
 * _only_ be externally called in error cases.  All other usage is a bug.
 */
/**
 * platform_device_del - 移除一个平台级别的设备
 * @pdev: 正在移除的平台设备
 *
 * 注意，这个函数还会释放设备所拥有的所有基于内存和端口的资源（@dev->resource）。这个函数
 * 只能在错误情况下被外部调用。任何其他用法都是错误的。
 */
void platform_device_del(struct platform_device *pdev)
{
	int i;

	if (pdev) {
		device_del(&pdev->dev);	// 删除设备，从系统中注销

		// 遍历设备的所有资源
		for (i = 0; i < pdev->num_resources; i++) {
			struct resource *r = &pdev->resource[i];
			unsigned long type = resource_type(r);

			// 如果资源类型是内存或IO资源，则释放它
			if (type == IORESOURCE_MEM || type == IORESOURCE_IO)
				release_resource(r);
		}
	}
}
EXPORT_SYMBOL_GPL(platform_device_del);

/**
 * platform_device_register - add a platform-level device
 * @pdev: platform device we're adding
 */
/**
 * platform_device_register - 添加一个平台级设备
 * @pdev: 我们正在添加的平台设备
 *
 * 这个函数初始化并添加一个平台设备。这通常在设备设置阶段或在驱动初始化期间调用。
 */
int platform_device_register(struct platform_device *pdev)
{
	device_initialize(&pdev->dev);	// 初始化设备，准备它以便添加
	return platform_device_add(pdev);	// 添加平台设备到系统，处理注册过程中的资源分配和设备创建
}
EXPORT_SYMBOL_GPL(platform_device_register);

/**
 * platform_device_unregister - unregister a platform-level device
 * @pdev: platform device we're unregistering
 *
 * Unregistration is done in 2 steps. First we release all resources
 * and remove it from the subsystem, then we drop reference count by
 * calling platform_device_put().
 */
/**
 * platform_device_unregister - 注销一个平台级设备
 * @pdev: 正在注销的平台设备
 *
 * 注销操作分为两步进行。首先释放所有资源并将其从子系统中移除，然后通过调用 platform_device_put() 减少引用计数。
 */
void platform_device_unregister(struct platform_device *pdev)
{
	// 第一步：调用 platform_device_del() 来删除设备，这包括从系统中移除设备并释放关联的资源
	platform_device_del(pdev);
	// 第二步：调用 platform_device_put() 以减少设备的引用计数，当引用计数达到0时，设备的内存会被释放
	platform_device_put(pdev);
}
EXPORT_SYMBOL_GPL(platform_device_unregister);

/**
 * platform_device_register_simple - add a platform-level device and its resources
 * @name: base name of the device we're adding
 * @id: instance id
 * @res: set of resources that needs to be allocated for the device
 * @num: number of resources
 *
 * This function creates a simple platform device that requires minimal
 * resource and memory management. Canned release function freeing memory
 * allocated for the device allows drivers using such devices to be
 * unloaded without waiting for the last reference to the device to be
 * dropped.
 *
 * This interface is primarily intended for use with legacy drivers which
 * probe hardware directly.  Because such drivers create sysfs device nodes
 * themselves, rather than letting system infrastructure handle such device
 * enumeration tasks, they don't fully conform to the Linux driver model.
 * In particular, when such drivers are built as modules, they can't be
 * "hotplugged".
 *
 * Returns &struct platform_device pointer on success, or ERR_PTR() on error.
 */
/**
 * platform_device_register_simple - 添加一个平台级设备及其资源
 * @name: 我们正在添加的设备的基本名称
 * @id: 实例id
 * @res: 需要为设备分配的资源集
 * @num: 资源的数量
 *
 * 这个函数创建一个简单的平台设备，该设备需要最小的资源和内存管理。内置的释放函数释放为设备分配的内存，
 * 允许使用这些设备的驱动程序在不等待设备的最后一个引用被放弃的情况下卸载。
 *
 * 这个接口主要用于直接探测硬件的遗留驱动程序。因为这些驱动程序自己创建 sysfs 设备节点，
 * 而不是让系统基础设施处理这样的设备枚举任务，它们并不完全符合 Linux 驱动模型。
 * 特别是，当这些驱动程序作为模块构建时，它们不能被“热插拔”。
 *
 * 成功时返回 &struct platform_device 指针，错误时返回 ERR_PTR()。
 */
struct platform_device *platform_device_register_simple(const char *name,
							int id,
							struct resource *res,
							unsigned int num)
{
	struct platform_device *pdev;
	int retval;

	pdev = platform_device_alloc(name, id);	// 分配一个平台设备
	if (!pdev) {
		retval = -ENOMEM;	// 内存不足
		goto error;
	}

	// 如果指定了资源数量，尝试添加资源
	if (num) {
		retval = platform_device_add_resources(pdev, res, num);
		if (retval)
			goto error;
	}

	// 将设备添加到系统中
	retval = platform_device_add(pdev);
	if (retval)
		goto error;

	return pdev;

error:
	// 如果过程中出现任何错误，清理已分配的平台设备
	platform_device_put(pdev);
	return ERR_PTR(retval);
}
EXPORT_SYMBOL_GPL(platform_device_register_simple);

/**
 * platform_device_register_data - add a platform-level device with platform-specific data
 * @parent: parent device for the device we're adding
 * @name: base name of the device we're adding
 * @id: instance id
 * @data: platform specific data for this platform device
 * @size: size of platform specific data
 *
 * This function creates a simple platform device that requires minimal
 * resource and memory management. Canned release function freeing memory
 * allocated for the device allows drivers using such devices to be
 * unloaded without waiting for the last reference to the device to be
 * dropped.
 *
 * Returns &struct platform_device pointer on success, or ERR_PTR() on error.
 */
/**
 * platform_device_register_data - 添加一个带有平台特定数据的平台级设备
 * @parent: 我们正在添加的设备的父设备
 * @name: 我们正在添加的设备的基本名称
 * @id: 实例id
 * @data: 该平台设备的平台特定数据
 * @size: 平台特定数据的大小
 *
 * 此函数创建一个简单的平台设备，该设备需要最少的资源和内存管理。内置的释放函数释放为设备分配的内存，
 * 允许使用这些设备的驱动程序在不等待设备的最后一个引用被放弃的情况下卸载。
 *
 * 成功时返回 &struct platform_device 指针，错误时返回 ERR_PTR()。
 */
struct platform_device *platform_device_register_data(
		struct device *parent,
		const char *name, int id,
		const void *data, size_t size)
{
	struct platform_device *pdev;
	int retval;

	// 分配一个新的平台设备
	pdev = platform_device_alloc(name, id);
	if (!pdev) {
		retval = -ENOMEM;	// 内存不足错误
		goto error;
	}

	// 设置新设备的父设备
	pdev->dev.parent = parent;

	// 如果提供了平台特定数据的大小，则添加这些数据
	if (size) {
		retval = platform_device_add_data(pdev, data, size);
		if (retval)
			goto error;	// 添加数据失败
	}

	// 将设备添加到系统中
	retval = platform_device_add(pdev);
	if (retval)
		goto error;	// 添加设备失败

	return pdev;

error:
	// 发生错误时，释放已分配的平台设备资源
	platform_device_put(pdev);
	return ERR_PTR(retval);
}
EXPORT_SYMBOL_GPL(platform_device_register_data);

/**
 * platform_drv_probe - 调用平台驱动的探测方法
 * @ _dev: 设备结构体
 *
 * 此函数包装了对平台驱动探测方法的调用。
 * 它从通用设备结构中提取平台驱动和平台设备，
 * 并调用平台驱动的特定探测方法。
 *
 * 返回平台驱动探测方法的返回值。
 */
static int platform_drv_probe(struct device *_dev)
{
	// 将通用设备驱动转换为平台驱动
	struct platform_driver *drv = to_platform_driver(_dev->driver);
	// 将通用设备转换为平台设备
	struct platform_device *dev = to_platform_device(_dev);

	// 调用平台驱动的探测方法，并返回其返回值
	return drv->probe(dev);
}

/**
 * platform_drv_probe_fail - 平台驱动的失败探测函数
 * @ _dev: 设备结构体
 *
 * 这个函数是一个占位符，总是返回 -ENXIO（没有这样的设备或地址）。
 * 当没有合适的探测函数可用或者不需要探测函数时使用。
 *
 * 返回 -ENXIO 以指示失败。
 */
static int platform_drv_probe_fail(struct device *_dev)
{
	return -ENXIO;
}

/**
 * platform_drv_remove - 调用平台驱动的移除方法
 * @ _dev: 设备结构体
 *
 * 此函数调用平台驱动的移除方法，允许驱动执行必要的清理工作。
 * 在设备被移除或驱动与设备解绑时调用。
 *
 * 返回平台驱动移除方法的返回值。
 */
static int platform_drv_remove(struct device *_dev)
{
	struct platform_driver *drv = to_platform_driver(_dev->driver);
	struct platform_device *dev = to_platform_device(_dev);

	return drv->remove(dev);
}

/**
 * platform_drv_shutdown - 调用平台驱动的关闭方法
 * @ _dev: 设备结构体
 *
 * 此函数调用平台驱动的关闭方法，通常用于在系统断电或重启时处理设备关闭。
 * 这是一种使设备进入静止状态的方法。
 */
static void platform_drv_shutdown(struct device *_dev)
{
	struct platform_driver *drv = to_platform_driver(_dev->driver);
	struct platform_device *dev = to_platform_device(_dev);

	drv->shutdown(dev);
}

/**
 * platform_driver_register - register a driver for platform-level devices
 * @drv: platform driver structure
 */
/**
 * platform_driver_register - 为平台级设备注册一个驱动程序
 * @drv: 平台驱动结构
 *
 * 此函数用于注册一个平台驱动到平台总线类型。
 * 它基于驱动程序中相应函数的存在，设置设备探测、移除和关闭的必要钩子。
 * 它封装了底层的 driver_register 调用。
 *
 * 成功时返回0，失败时返回负错误代码。
 */
int platform_driver_register(struct platform_driver *drv)
{
	drv->driver.bus = &platform_bus_type;	// 将驱动的总线类型设置为平台总线类型
	if (drv->probe)	// 如果驱动定义了探测函数，设置驱动的探测钩子
		drv->driver.probe = platform_drv_probe;
	if (drv->remove)	// 如果驱动定义了移除函数，设置驱动的移除钩子
		drv->driver.remove = platform_drv_remove;
	if (drv->shutdown)	// 如果驱动定义了关闭函数，设置驱动的关闭钩子
		drv->driver.shutdown = platform_drv_shutdown;

	// 调用 driver_register 函数注册驱动，并返回其返回值
	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(platform_driver_register);

/**
 * platform_driver_unregister - unregister a driver for platform-level devices
 * @drv: platform driver structure
 */
/**
 * platform_driver_unregister - 为平台级设备注销一个驱动程序
 * @drv: 平台驱动结构
 *
 * 此函数用于注销一个平台驱动，有效地逆转由 platform_driver_register() 执行的操作。
 * 这包括将驱动程序从系统的驱动程序列表中移除并清理任何关联的数据。
 *
 * 它直接调用 driver_unregister()，该函数处理所有与驱动程序注销相关的底层细节。
 */
void platform_driver_unregister(struct platform_driver *drv)
{
	// 调用 driver_unregister() 来注销驱动程序
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(platform_driver_unregister);

/**
 * platform_driver_probe - register driver for non-hotpluggable device
 * @drv: platform driver structure
 * @probe: the driver probe routine, probably from an __init section
 *
 * Use this instead of platform_driver_register() when you know the device
 * is not hotpluggable and has already been registered, and you want to
 * remove its run-once probe() infrastructure from memory after the driver
 * has bound to the device.
 *
 * One typical use for this would be with drivers for controllers integrated
 * into system-on-chip processors, where the controller devices have been
 * configured as part of board setup.
 *
 * Returns zero if the driver registered and bound to a device, else returns
 * a negative error code and with the driver not registered.
 */
/**
 * platform_driver_probe - 为非热插拔设备注册驱动
 * @drv: 平台驱动结构
 * @probe: 驱动的探测函数，通常位于 __init 段
 *
 * 当你知道设备不是热插拔的，并且已经被注册，且希望在驱动绑定到设备后，
 * 从内存中移除其一次性运行的 probe() 基础设施时，使用这个函数替代 platform_driver_register()。
 *
 * 这种用法的一个典型场景是用于系统级芯片内集成的控制器的驱动，其中控制器设备已作为
 * 板级设置的一部分被配置。
 *
 * 如果驱动注册并绑定到设备，返回零；否则返回一个负的错误代码，并且驱动未注册。
 */

int __init_or_module platform_driver_probe(struct platform_driver *drv,
		int (*probe)(struct platform_device *))
{
	int retval, code;

	/* make sure driver won't have bind/unbind attributes */
	// 确保驱动不会有绑定/解绑属性
	drv->driver.suppress_bind_attrs = true;	// 确保驱动没有绑定/解绑属性，避免在后期解绑时出现问题

	/* temporary section violation during probe() */
	// 在探测期间临时违反段
	drv->probe = probe;	// 将传入的探测函数直接赋值给驱动程序结构，违反了正常的驱动注册流程
	retval = code = platform_driver_register(drv);	// 尝试注册驱动程序，如果失败，platform_driver_register 返回错误代码

	/*
	 * Fixup that section violation, being paranoid about code scanning
	 * the list of drivers in order to probe new devices.  Check to see
	 * if the probe was successful, and make sure any forced probes of
	 * new devices fail.
	 */
	/*
	 * 修正这种段违规情况，假设有代码扫描驱动列表以探测新设备。
	 * 检查以确定探测是否成功，并确保任何强制探测新设备的操作失败。
	 */
	spin_lock(&platform_bus_type.p->klist_drivers.k_lock);
	// 清除驱动的探测函数，避免在驱动注册后还被再次调用
	drv->probe = NULL;
	// 检查驱动是否注册成功且驱动没有绑定任何设备
	if (code == 0 && list_empty(&drv->driver.p->klist_devices.k_list))
		retval = -ENODEV;
	// 设置失败探测函数，确保后续对该驱动的探测调用失败
	drv->driver.probe = platform_drv_probe_fail;
	spin_unlock(&platform_bus_type.p->klist_drivers.k_lock);

	// 如果注册与实际探测结果不一致，则取消驱动注册
	if (code != retval)
		platform_driver_unregister(drv);
	return retval;	// 返回最终操作结果
}
EXPORT_SYMBOL_GPL(platform_driver_probe);

/**
 * platform_create_bundle - register driver and create corresponding device
 * @driver: platform driver structure
 * @probe: the driver probe routine, probably from an __init section
 * @res: set of resources that needs to be allocated for the device
 * @n_res: number of resources
 * @data: platform specific data for this platform device
 * @size: size of platform specific data
 *
 * Use this in legacy-style modules that probe hardware directly and
 * register a single platform device and corresponding platform driver.
 *
 * Returns &struct platform_device pointer on success, or ERR_PTR() on error.
 */
/**
 * platform_create_bundle - 注册驱动程序并创建对应的设备
 * @driver: 平台驱动结构体
 * @probe: 驱动程序的探测函数，通常来自 __init 段
 * @res: 需要为设备分配的资源集合
 * @n_res: 资源的数量
 * @data: 平台特定数据，用于该平台设备
 * @size: 平台特定数据的大小
 *
 * 在直接探测硬件并注册单个平台设备及其相应平台驱动程序的传统风格模块中使用此函数。
 *
 * 成功时返回指向 struct platform_device 的指针，失败时返回 ERR_PTR()。
 */
struct platform_device * __init_or_module platform_create_bundle(
			struct platform_driver *driver,	// 平台驱动结构体
			int (*probe)(struct platform_device *),	// 驱动程序的探测函数
			struct resource *res, unsigned int n_res,	// 设备所需的资源和数量
			const void *data, size_t size)	// 平台特定数据及其大小
{
	struct platform_device *pdev;	// 平台设备指针
	int error;	// 错误码

	// 使用平台驱动的名称分配一个新的平台设备，ID 设置为 -1 表示未指定
	pdev = platform_device_alloc(driver->driver.name, -1);
	if (!pdev) {
		// 如果分配失败，则设置错误码并跳转到错误处理部分
		error = -ENOMEM;
		goto err_out;
	}

	// 如果有资源需要分配，则添加资源到平台设备
	if (res) {
		error = platform_device_add_resources(pdev, res, n_res);
		if (error)
			goto err_pdev_put;	// 如果添加资源失败，则跳转到错误处理部分
	}

	// 如果有数据需要添加，则将平台特定数据添加到平台设备
	if (data) {
		error = platform_device_add_data(pdev, data, size);
		if (error)
			goto err_pdev_put;	// 如果添加数据失败，则跳转到错误处理部分
	}

	// 将平台设备添加到设备层次结构中
	error = platform_device_add(pdev);
	if (error)
		goto err_pdev_put;	// 如果添加设备失败，则跳转到错误处理部分

	// 使用指定的探测函数注册平台驱动程序
	error = platform_driver_probe(driver, probe);
	if (error)
		goto err_pdev_del;	// 如果驱动程序探测失败，则删除已添加的平台设备

	return pdev;	// 返回指向成功创建的平台设备的指针

err_pdev_del:
	// 注册失败时，删除已添加的平台设备
	platform_device_del(pdev);
err_pdev_put:
	// 出错时释放平台设备
	platform_device_put(pdev);
err_out:
	// 返回错误指针
	return ERR_PTR(error);
}
EXPORT_SYMBOL_GPL(platform_create_bundle);

/* modalias support enables more hands-off userspace setup:
 * (a) environment variable lets new-style hotplug events work once system is
 *     fully running:  "modprobe $MODALIAS"
 * (b) sysfs attribute lets new-style coldplug recover from hotplug events
 *     mishandled before system is fully running:  "modprobe $(cat modalias)"
 */
/* modalias 支持启用更自动化的用户空间设置：
 * (a) 环境变量允许新式热插拔事件在系统完全运行后工作： "modprobe $MODALIAS"
 * (b) sysfs 属性允许新式冷插拔在系统完全运行之前从热插拔事件中恢复： "modprobe $(cat modalias)"
 */

/* modalias_show - 读取 modalias 属性的显示函数
 * @dev: 设备结构体
 * @a: 设备属性
 * @buf: 用于存储输出的缓冲区
 *
 * 将平台设备的 modalias 字符串格式化到缓冲区中。modalias 是
 * 用于识别设备的字符串，通常包含设备的名称。
 *
 * 返回值是缓冲区中实际写入的字符数，或者如果缓冲区溢出，则返回 PAGE_SIZE - 1。
 */
static ssize_t modalias_show(struct device *dev, struct device_attribute *a,
			     char *buf)
{
	struct platform_device	*pdev = to_platform_device(dev); // 将设备转换为平台设备
	int len = snprintf(buf, PAGE_SIZE, "platform:%s\n", pdev->name); // 将平台设备名称格式化为 "platform:name" 的形式并写入缓冲区

	return (len >= PAGE_SIZE) ? (PAGE_SIZE - 1) : len; // 返回缓冲区中实际写入的字符数，确保不超过 PAGE_SIZE
}

/* platform_dev_attrs - 平台设备的属性数组
 *
 * 定义了一个包含 platform_device 的 sysfs 属性的数组，其中包括
 * modalias 属性，允许用户空间通过读取 modalias 属性来获得设备的 modalias 字符串。
 */
static struct device_attribute platform_dev_attrs[] = {
	__ATTR_RO(modalias),	// 定义只读的 modalias 属性
	__ATTR_NULL,	// 数组的结束标志
};

/* platform_uevent - 生成平台设备的 uevent 环境变量
 * @dev: 设备结构体
 * @env: uevent 环境变量
 *
 * 这个函数为平台设备生成一个 uevent 环境变量。特别是，它将
 * 设备的 modalias 信息添加到 uevent 环境变量中。modalias 用于
 * 识别设备，并且可以帮助用户空间工具进行设备的热插拔处理。
 *
 * 返回0表示成功。
 */
static int platform_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct platform_device	*pdev = to_platform_device(dev); // 将通用设备指针转换为平台设备指针

	add_uevent_var(env, "MODALIAS=%s%s", PLATFORM_MODULE_PREFIX,
		(pdev->id_entry) ? pdev->id_entry->name : pdev->name); // 将设备的 modalias 信息添加到 uevent 环境变量中
	return 0; // 返回0表示成功
}

/* platform_match_id - 根据平台设备 ID 匹配平台设备
 * @id: 平台设备 ID 数组
 * @pdev: 平台设备
 *
 * 遍历平台设备 ID 数组，寻找与给定平台设备名称匹配的 ID。
 * 如果找到匹配项，更新平台设备的 id_entry 并返回匹配的 ID。
 *
 * 返回匹配的 ID 指针，如果没有找到匹配的 ID，则返回 NULL。
 */
static const struct platform_device_id *platform_match_id(
			const struct platform_device_id *id,
			struct platform_device *pdev)
{
	while (id->name[0]) { // 遍历 ID 数组，直到遇到结束标志
		if (strcmp(pdev->name, id->name) == 0) { // 比较平台设备名称与当前 ID 的名称
			pdev->id_entry = id; // 更新平台设备的 id_entry
			return id; // 返回匹配的 ID
		}
		id++; // 移动到下一个 ID
	}
	return NULL; // 没有找到匹配的 ID，返回 NULL
}

/**
 * platform_match - bind platform device to platform driver.
 * @dev: device.
 * @drv: driver.
 *
 * Platform device IDs are assumed to be encoded like this:
 * "<name><instance>", where <name> is a short description of the type of
 * device, like "pci" or "floppy", and <instance> is the enumerated
 * instance of the device, like '0' or '42'.  Driver IDs are simply
 * "<name>".  So, extract the <name> from the platform_device structure,
 * and compare it against the name of the driver. Return whether they match
 * or not.
 */
/**
 * platform_match - 绑定平台设备到平台驱动程序。
 * @dev: 设备。
 * @drv: 驱动程序。
 *
 * 平台设备ID被假定为这样编码的：“<name><instance>”，其中<name>是设备类型的简短描述，
 * 如"pci"或"floppy"，而<instance>是设备的枚举实例，如'0'或'42'。驱动程序ID简单地为“<name>”。
 * 因此，从platform_device结构中提取<name>，并将其与驱动程序的名称比较。返回它们是否匹配。
 */
static int platform_match(struct device *dev, struct device_driver *drv)
{
	struct platform_device *pdev = to_platform_device(dev);	// 将device结构转换为platform_device结构
	struct platform_driver *pdrv = to_platform_driver(drv);	// 将device_driver结构转换为platform_driver结构

	/* match against the id table first */
	// 首先尝试使用ID表进行匹配
	if (pdrv->id_table)
		// 如果ID表存在，使用platform_match_id函数检查设备与驱动程序ID是否匹配
		return platform_match_id(pdrv->id_table, pdev) != NULL;

	/* fall-back to driver name match */
	// 如果没有ID表或ID表匹配失败，退回到驱动程序名称匹配
	return (strcmp(pdev->name, drv->name) == 0);	 // 比较设备名称和驱动程序名称，如果相同则返回1（匹配），否则返回0（不匹配）
}

#ifdef CONFIG_PM_SLEEP

// 定义一个函数，用于处理平台设备的挂起操作
static int platform_legacy_suspend(struct device *dev, pm_message_t mesg)
{
	// 将device结构转换为platform_driver结构
	struct platform_driver *pdrv = to_platform_driver(dev->driver);
	// 将device结构转换为platform_device结构
	struct platform_device *pdev = to_platform_device(dev);
	int ret = 0;	// 初始化返回值为0

	// 如果设备的驱动程序存在并且该驱动程序有挂起函数，则调用驱动程序的挂起函数
	if (dev->driver && pdrv->suspend)
		ret = pdrv->suspend(pdev, mesg);	// 调用挂起函数，并传递平台设备和消息

	return ret;	// 返回挂起操作的结果
}

// 定义一个函数，用于处理平台设备的恢复操作
static int platform_legacy_resume(struct device *dev)
{
	// 将device结构转换为platform_driver结构
	struct platform_driver *pdrv = to_platform_driver(dev->driver);
	// 将device结构转换为platform_device结构
	struct platform_device *pdev = to_platform_device(dev);
	int ret = 0;	// 初始化返回值为0

	// 如果设备的驱动程序存在并且该驱动程序有恢复函数，则调用驱动程序的恢复函数
	if (dev->driver && pdrv->resume)
		ret = pdrv->resume(pdev);	// 调用恢复函数，并传递平台设备

	return ret;	// 返回恢复操作的结果
}

// 定义一个函数，用于准备设备进入电源管理状态
static int platform_pm_prepare(struct device *dev)
{
	struct device_driver *drv = dev->driver;	// 获取设备的驱动程序
	int ret = 0;

	// 检查驱动程序是否存在，并且检查驱动程序是否具有电源管理回调函数，且具体的准备函数是否被设置
	if (drv && drv->pm && drv->pm->prepare)
		ret = drv->pm->prepare(dev);	// 如果条件满足，则调用准备函数，并将设备作为参数传递

	return ret;	// 返回准备操作的结果
}

// 定义一个函数，用于完成设备从电源管理状态恢复的操作
static void platform_pm_complete(struct device *dev)
{
	struct device_driver *drv = dev->driver;	// 获取设备的驱动程序

	// 检查驱动程序是否存在，并且检查驱动程序是否具有电源管理回调函数，且具体的完成函数是否被设置
	if (drv && drv->pm && drv->pm->complete)
		drv->pm->complete(dev);	// 如果条件满足，则调用完成函数，并将设备作为参数传递
}

#else /* !CONFIG_PM_SLEEP */

// 如果未配置 CONFIG_PM_SLEEP，则将 platform_pm_prepare 和 platform_pm_complete 定义为 NULL
#define platform_pm_prepare		NULL
#define platform_pm_complete		NULL

#endif /* !CONFIG_PM_SLEEP */

#ifdef CONFIG_SUSPEND	// 如果定义了 CONFIG_SUSPEND，表示支持系统挂起功能

// 定义一个函数，用于挂起设备，处理设备的电源管理
static int platform_pm_suspend(struct device *dev)
{
	struct device_driver *drv = dev->driver;	// 获取设备对应的驱动
	int ret = 0;	// 初始化返回值为0，表示默认成功

	if (!drv)	// 如果没有驱动与设备关联
		return 0;	// 直接返回0，表示无操作但成功

	if (drv->pm) {	// 如果驱动有定义电源管理结构
		if (drv->pm->suspend)	// 并且定义了挂起函数
			ret = drv->pm->suspend(dev);	// 调用挂起函数，并将返回值赋给ret
	} else {
		// 如果驱动没有定义电源管理结构，使用传统的挂起函数
		ret = platform_legacy_suspend(dev, PMSG_SUSPEND);
	}

	return ret;	// 返回操作结果
}

// 定义一个函数，用于在没有中断的情况下挂起设备
static int platform_pm_suspend_noirq(struct device *dev)
{
	struct device_driver *drv = dev->driver;  // 获取设备对应的驱动
	int ret = 0;  // 初始化返回值为0，表示默认成功

	if (!drv)  // 如果没有驱动与设备关联
		return 0;  // 直接返回0，表示无操作但成功

	if (drv->pm) {  // 如果驱动有定义电源管理结构
		if (drv->pm->suspend_noirq)  // 并且定义了无中断的挂起函数
			ret = drv->pm->suspend_noirq(dev);  // 调用该函数，并将返回值赋给ret
	}

	return ret;  // 返回操作结果
}

/**
 * platform_pm_resume - 为平台设备执行恢复操作
 * @dev: 设备的指针
 * 
 * 此函数用于恢复平台设备。它首先检查设备是否有关联的驱动程序。
 * 如果驱动程序具有电源管理结构，并定义了恢复方法，则调用此方法。
 * 如果驱动程序没有定义恢复方法，则调用平台的传统恢复方法。
 * 返回执行恢复操作的结果。
 */
static int platform_pm_resume(struct device *dev)
{
	struct device_driver *drv = dev->driver;  // 获取与设备相关联的驱动程序
	int ret = 0;  // 初始化返回值为0，表示默认成功

	if (!drv)  // 如果设备没有关联的驱动程序
		return 0;  // 直接返回成功，因为没有恢复操作要执行

	if (drv->pm) {  // 如果驱动程序有电源管理功能
		if (drv->pm->resume)  // 并且定义了一个恢复函数
			ret = drv->pm->resume(dev);  // 执行这个函数，并将结果赋给ret
	} else {
		ret = platform_legacy_resume(dev);  // 否则，执行平台的传统恢复方法
	}

	return ret;  // 返回恢复操作的结果
}

/**
 * platform_pm_resume_noirq - 执行设备的无中断恢复操作
 * @dev: 设备的指针
 * 
 * 此函数用于在无中断的情况下恢复平台设备。这是电源管理框架中非常重要的一部分，
 * 用于在不允许中断的情况下恢复设备。这通常在系统的早期恢复阶段使用，
 * 在大多数中断还未恢复前需要恢复关键设备的驱动程序。
 * 
 * 如果设备驱动程序定义了 `resume_noirq` 方法，则此方法将被调用。
 * 
 * 返回执行恢复操作的结果，如果没有定义恢复方法，返回 0 表示无操作执行。
 */
static int platform_pm_resume_noirq(struct device *dev)
{
	struct device_driver *drv = dev->driver;  // 获取与设备关联的驱动程序
	int ret = 0;  // 初始化返回值为0，表示默认成功

	if (!drv)  // 如果设备没有关联的驱动程序
		return 0;  // 直接返回成功，因为没有特定的恢复操作要执行

	if (drv->pm) {  // 如果驱动程序有电源管理结构
		if (drv->pm->resume_noirq)  // 并且定义了无中断的恢复函数
			ret = drv->pm->resume_noirq(dev);  // 执行这个函数，并将结果赋给ret
	}

	return ret;  // 返回恢复操作的结果
}

// 针对未启用 CONFIG_SUSPEND 配置项的条件编译部分
#else /* !CONFIG_SUSPEND */

// 如果未启用 CONFIG_SUSPEND，则将所有平台电源管理操作定义为 NULL
#define platform_pm_suspend		NULL
#define platform_pm_resume		NULL
#define platform_pm_suspend_noirq	NULL
#define platform_pm_resume_noirq	NULL

#endif /* !CONFIG_SUSPEND */

#ifdef CONFIG_HIBERNATION

/**
 * platform_pm_freeze - 冻结设备的操作
 * @dev: 需要冻结的设备
 *
 * 冻结操作是在系统进入休眠状态前执行，用于将设备状态保存到内存中。
 * 这允许设备在系统从休眠状态恢复后，能迅速回到之前的状态。
 *
 * 如果设备的驱动定义了 freeze 方法，该方法会被调用来冻结设备。
 * 如果没有定义 freeze 方法，将回退使用 platform_legacy_suspend 函数
 * 来尝试冻结设备。
 *
 * 返回值：如果冻结成功返回0，否则返回错误代码。
 */
static int platform_pm_freeze(struct device *dev)
{
	struct device_driver *drv = dev->driver; // 获取设备关联的驱动程序
	int ret = 0; // 初始化返回值为0，表示默认成功

	if (!drv) // 如果设备没有关联的驱动程序
		return 0; // 直接返回成功，因为没有特定的冻结操作要执行

	if (drv->pm) { // 如果驱动程序有电源管理结构
		if (drv->pm->freeze) // 并且定义了冻结函数
			ret = drv->pm->freeze(dev); // 执行这个函数，并将结果赋给ret
	} else {
		// 如果没有定义冻结函数，尝试调用传统的暂停函数来冻结设备
		ret = platform_legacy_suspend(dev, PMSG_FREEZE);
	}

	return ret; // 返回冻结操作的结果
}

/**
 * platform_pm_freeze_noirq - 在没有中断的情况下冻结设备
 * @dev: 需要冻结的设备
 *
 * 这个函数是在系统进入休眠状态前的最后阶段执行，此时系统中断已经被禁用。
 * 这一步骤确保在设备冻结过程中不会有中断发生，这对于需要确保数据完整性和
 * 设备状态一致性的设备尤为重要。
 *
 * 如果设备驱动程序提供了一个无中断的冻结方法（freeze_noirq），则调用它。
 * 如果没有提供，这个函数将简单地返回0，表示没有执行任何操作。
 *
 * 返回值：如果冻结成功或无操作执行返回0，否则返回错误代码。
 */
static int platform_pm_freeze_noirq(struct device *dev)
{
	struct device_driver *drv = dev->driver; // 获取设备的驱动程序
	int ret = 0; // 初始化返回值为0，表示默认操作成功

	if (!drv) // 如果没有驱动程序
		return 0; // 直接返回成功，因为没有特定的冻结操作要执行

	if (drv->pm) { // 如果驱动程序包含电源管理结构
		if (drv->pm->freeze_noirq) // 并且定义了无中断的冻结函数
			ret = drv->pm->freeze_noirq(dev); // 调用该函数，并保存返回值
	}

	return ret; // 返回操作结果
}

/**
 * platform_pm_thaw - 使设备从冻结状态恢复正常工作
 * @dev: 需要恢复的设备
 *
 * 此函数在设备从休眠中唤醒后调用，目的是解除之前调用 platform_pm_freeze 时设置的设备冻结状态。
 * 此函数将检查设备的驱动程序是否提供了 thaw 函数，如果提供了，则调用该函数恢复设备状态。
 * 如果没有提供 thaw 函数，将调用 platform_legacy_resume 函数尝试恢复设备状态。
 *
 * 返回值：如果设备成功恢复或没有执行任何操作，则返回 0，否则返回错误代码。
 */
static int platform_pm_thaw(struct device *dev)
{
	struct device_driver *drv = dev->driver; // 获取设备关联的驱动程序
	int ret = 0; // 初始化返回值为0，表示默认操作成功

	if (!drv) // 如果没有驱动程序
		return 0; // 直接返回成功，因为没有特定的恢复操作要执行

	if (drv->pm) { // 如果驱动程序包含电源管理结构
		if (drv->pm->thaw) // 并且定义了解冻函数
			ret = drv->pm->thaw(dev); // 调用该函数，并保存返回值
	} else {
		ret = platform_legacy_resume(dev); // 否则，尝试使用传统的恢复方法
	}

	return ret; // 返回操作结果
}

/**
 * platform_pm_thaw_noirq - 在没有中断的情况下，使设备从冻结状态恢复正常工作
 * @dev: 需要恢复的设备
 *
 * 此函数在设备从休眠中唤醒后调用，在不涉及中断的情况下恢复设备的工作状态。
 * 此函数将检查设备的驱动程序是否提供了对应的 thaw_noirq 函数，如果提供了，则调用该函数恢复设备状态。
 * 如果没有提供 thaw_noirq 函数，将简单地返回0，表示没有执行任何操作。
 *
 * 返回值：如果设备成功恢复或没有执行任何操作，则返回 0，否则返回错误代码。
 */
static int platform_pm_thaw_noirq(struct device *dev)
{
	struct device_driver *drv = dev->driver; // 获取设备关联的驱动程序
	int ret = 0; // 初始化返回值为0，表示默认操作成功

	if (!drv) // 如果没有驱动程序
		return 0; // 直接返回成功，因为没有特定的恢复操作要执行

	if (drv->pm) { // 如果驱动程序包含电源管理结构
		if (drv->pm->thaw_noirq) // 并且定义了不涉及中断的解冻函数
			ret = drv->pm->thaw_noirq(dev); // 调用该函数，并保存返回值
	}

	return ret; // 返回操作结果
}

/**
 * platform_pm_poweroff - 执行设备的关机（断电）操作
 * @dev: 需要进行断电操作的设备
 *
 * 此函数用于执行设备的关机（断电）操作，主要在系统需要彻底断电前调用。
 * 它会检查设备的驱动程序中是否实现了 poweroff 方法，如果实现了，则调用该方法。
 * 如果没有提供 poweroff 方法，将使用传统的挂起方法来模拟断电处理。
 *
 * 返回值：如果操作成功或没有执行任何操作，则返回 0，否则返回错误代码。
 */
static int platform_pm_poweroff(struct device *dev)
{
	struct device_driver *drv = dev->driver;  // 获取设备关联的驱动程序
	int ret = 0;  // 初始化返回值为0，表示默认操作成功

	if (!drv)  // 如果没有驱动程序
		return 0;  // 直接返回成功，因为没有特定的关机操作要执行

	if (drv->pm) {  // 如果驱动程序包含电源管理结构
		if (drv->pm->poweroff)  // 并且定义了 poweroff 函数
			ret = drv->pm->poweroff(dev);  // 调用该函数，并保存返回值
	} else {  // 如果没有提供 poweroff 函数
		ret = platform_legacy_suspend(dev, PMSG_HIBERNATE);  // 使用传统的挂起函数来模拟断电
	}

	return ret;  // 返回操作结果
}

/**
 * platform_pm_poweroff_noirq - 执行设备的关机操作，不处理中断请求
 * @dev: 需要进行断电操作的设备
 *
 * 此函数用于在不处理任何中断请求的情况下执行设备的关机（断电）操作。
 * 它检查设备的驱动程序是否实现了 poweroff_noirq 方法，
 * 如果实现了，则调用该方法执行断电操作，此操作在中断被禁用的情况下执行，
 * 确保在关键时刻不会被中断干扰。
 *
 * 返回值：如果操作成功或没有执行任何操作，则返回 0，否则返回错误代码。
 */
static int platform_pm_poweroff_noirq(struct device *dev)
{
	struct device_driver *drv = dev->driver;  // 获取设备关联的驱动程序
	int ret = 0;  // 初始化返回值为0，表示默认操作成功

	if (!drv)  // 如果没有驱动程序
		return 0;  // 直接返回成功，因为没有特定的关机操作要执行

	if (drv->pm) {  // 如果驱动程序包含电源管理结构
		if (drv->pm->poweroff_noirq)  // 并且定义了 poweroff_noirq 函数
			ret = drv->pm->poweroff_noirq(dev);  // 调用该函数，并保存返回值
	}

	return ret;  // 返回操作结果
}

/**
 * platform_pm_restore - 恢复设备到之前的状态
 * @dev: 要恢复的设备
 *
 * 此函数负责将设备从低功耗状态恢复到正常工作状态。如果设备驱动程序提供了 restore 方法，
 * 则调用该方法。如果没有提供，将尝试调用传统的 resume 方法作为回退。
 *
 * 返回值：如果恢复成功或设备驱动程序未定义恢复方法，则返回 0；否则返回错误代码。
 */
static int platform_pm_restore(struct device *dev)
{
	struct device_driver *drv = dev->driver;  // 获取设备关联的驱动程序
	int ret = 0;  // 初始化返回值为0，表示默认操作成功

	if (!drv)  // 如果没有驱动程序
		return 0;  // 直接返回成功，因为没有特定的恢复操作要执行

	if (drv->pm) {  // 如果驱动程序包含电源管理结构
		if (drv->pm->restore)  // 并且定义了 restore 函数
			ret = drv->pm->restore(dev);  // 调用该函数，并保存返回值
	} else {
		ret = platform_legacy_resume(dev);  // 使用传统的 resume 方法作为回退
	}

	return ret;  // 返回操作结果
}

/**
 * platform_pm_restore_noirq - 恢复设备到之前的状态，不处理中断
 * @dev: 要恢复的设备
 *
 * 此函数负责在不处理任何中断的情况下恢复设备到之前的状态。如果设备驱动程序定义了恢复操作
 * 且不涉及中断处理，则此函数将被调用。
 *
 * 返回值：如果恢复操作成功执行，则返回0；否则返回错误代码。
 */
static int platform_pm_restore_noirq(struct device *dev)
{
	struct device_driver *drv = dev->driver;  // 获取设备的驱动程序
	int ret = 0;  // 初始化返回值为0，表示默认操作成功

	if (!drv)  // 如果没有驱动程序
		return 0;  // 直接返回成功，因为没有特定的恢复操作要执行

	if (drv->pm) {  // 如果驱动程序包含电源管理结构
		if (drv->pm->restore_noirq)  // 如果定义了不处理中断的恢复函数
			ret = drv->pm->restore_noirq(dev);  // 调用该函数，并保存返回值
	}

	return ret;  // 返回操作结果
}

#else /* !CONFIG_HIBERNATION */

// 在不支持休眠的配置下，将所有电源管理函数指针设为 NULL
#define platform_pm_freeze		NULL
#define platform_pm_thaw		NULL
#define platform_pm_poweroff		NULL
#define platform_pm_restore		NULL
#define platform_pm_freeze_noirq	NULL
#define platform_pm_thaw_noirq		NULL
#define platform_pm_poweroff_noirq	NULL
#define platform_pm_restore_noirq	NULL

#endif /* !CONFIG_HIBERNATION */

#ifdef CONFIG_PM_RUNTIME

/**
 * platform_pm_runtime_suspend - 挂起设备的运行时电源管理处理函数
 * @dev: 需要被挂起的设备
 *
 * 如果运行时电源管理已配置（CONFIG_PM_RUNTIME），此函数用于处理设备的运行时挂起。
 * 如果未提供具体实现，此函数返回 -ENOSYS 表示该操作不被系统支持。
 *
 * 返回值: 如果挂起成功，通常返回 0 或正常完成代码；如果未实现，返回 -ENOSYS。
 */
int __weak platform_pm_runtime_suspend(struct device *dev)
{
	return -ENOSYS;
};
/**
 * platform_pm_runtime_resume - 恢复设备的运行时电源管理处理函数
 * @dev: 需要被恢复的设备
 *
 * 处理设备从运行时挂起状态恢复的功能。如果未提供具体实现，此函数返回 -ENOSYS 表示该操作不被系统支持。
 *
 * 返回值: 如果恢复成功，通常返回 0 或正常完成代码；如果未实现，返回 -ENOSYS。
 */

int __weak platform_pm_runtime_resume(struct device *dev)
{
	return -ENOSYS;
};

/**
 * platform_pm_runtime_idle - 设备进入空闲状态的运行时电源管理处理函数
 * @dev: 需要进入空闲状态的设备
 *
 * 处理设备进入空闲状态的运行时电源管理。如果未提供具体实现，此函数返回 -ENOSYS 表示该操作不被系统支持。
 *
 * 返回值: 如果操作成功，通常返回 0 或正常完成代码；如果未实现，返回 -ENOSYS。
 */
int __weak platform_pm_runtime_idle(struct device *dev)
{
	return -ENOSYS;
};

#else /* !CONFIG_PM_RUNTIME */

// 如果没有启用运行时电源管理 (CONFIG_PM_RUNTIME)，将相关函数指针设置为 NULL
#define platform_pm_runtime_suspend NULL
#define platform_pm_runtime_resume NULL
#define platform_pm_runtime_idle NULL

#endif /* !CONFIG_PM_RUNTIME */

/**
 * platform_dev_pm_ops - 设备电源管理操作的结构体
 * 定义了各种电源管理状态下的回调函数，用于管理设备的电源状态。
 */
static const struct dev_pm_ops platform_dev_pm_ops = {
	.prepare = platform_pm_prepare, // 设备进入电源管理状态前的准备工作
	.complete = platform_pm_complete, // 完成电源状态转换后的收尾工作
	.suspend = platform_pm_suspend, // 设备挂起操作
	.resume = platform_pm_resume, // 设备恢复操作
	.freeze = platform_pm_freeze, // 设备冻结操作
	.thaw = platform_pm_thaw, // 设备解冻操作
	.poweroff = platform_pm_poweroff, // 设备断电操作
	.restore = platform_pm_restore, // 设备恢复供电后的操作
	.suspend_noirq = platform_pm_suspend_noirq, // 无中断挂起操作
	.resume_noirq = platform_pm_resume_noirq, // 无中断恢复操作
	.freeze_noirq = platform_pm_freeze_noirq, // 无中断冻结操作
	.thaw_noirq = platform_pm_thaw_noirq, // 无中断解冻操作
	.poweroff_noirq = platform_pm_poweroff_noirq, // 无中断断电操作
	.restore_noirq = platform_pm_restore_noirq, // 无中断恢复供电操作
	.runtime_suspend = platform_pm_runtime_suspend, // 运行时挂起操作
	.runtime_resume = platform_pm_runtime_resume, // 运行时恢复操作
	.runtime_idle = platform_pm_runtime_idle, // 运行时空闲操作
};

/**
 * platform_bus_type - 平台总线类型
 * 定义了平台总线的基本属性和操作。
 */
struct bus_type platform_bus_type = {
	.name		= "platform", // 总线名称
	.dev_attrs	= platform_dev_attrs, // 总线设备属性数组
	.match		= platform_match, // 设备与驱动匹配函数
	.uevent		= platform_uevent, // 生成uevent事件的函数
	.pm		= &platform_dev_pm_ops, // 电源管理操作
};
EXPORT_SYMBOL_GPL(platform_bus_type);

/**
 * platform_bus_init - 初始化平台总线
 *
 * 初始化平台总线并注册设备和总线。
 * 这个函数会在系统引导早期被调用，用于设置平台总线。
 *
 * 返回值: 成功返回0，失败返回错误码。
 */
int __init platform_bus_init(void)
{
	int error;  // 用于存储错误码

	early_platform_cleanup();  // 清理早期平台设备注册过程中的资源

	error = device_register(&platform_bus);  // 注册平台总线设备
	if (error)  // 如果注册失败
		return error;  // 返回错误码

	error = bus_register(&platform_bus_type);  // 注册平台总线类型
	if (error)  // 如果总线注册失败
		device_unregister(&platform_bus);  // 取消已经注册的平台总线设备

	return error;  // 返回操作结果，成功或错误码

}

#ifndef ARCH_HAS_DMA_GET_REQUIRED_MASK
/**
 * dma_get_required_mask - 计算设备所需的DMA掩码
 * @dev: 设备结构体指针
 *
 * 根据系统总RAM计算设备支持的最大DMA地址。
 * 这个函数计算系统所需的DMA掩码，基于系统的总RAM大小。
 *
 * 返回值: 返回计算出的DMA掩码。
 */
u64 dma_get_required_mask(struct device *dev)
{
	u32 low_totalram = ((max_pfn - 1) << PAGE_SHIFT);  // 计算物理页帧号上限对应的地址的低32位
	u32 high_totalram = ((max_pfn - 1) >> (32 - PAGE_SHIFT));  // 计算物理页帧号上限对应的地址的高32位
	u64 mask;  // 最终的DMA掩码

	if (!high_totalram) {
		/* convert o mask just covering totalram */
		// 如果高32位为0，说明总RAM小于4GB
		low_totalram = (1 << (fls(low_totalram) - 1));  // 找到最高位的1，计算2的幂次方
		low_totalram += low_totalram - 1;  // 创建一个掩码，包含所有低位
		mask = low_totalram;  // 设置掩码
	} else {
		// 如果高32位不为0，说明总RAM超过4GB
		high_totalram = (1 << (fls(high_totalram) - 1));  // 计算高位部分的掩码
		high_totalram += high_totalram - 1;  // 修正高位掩码
		mask = (((u64)high_totalram) << 32) + 0xffffffff;  // 创建完整的64位掩码
	}
	return mask;  // 返回计算得到的DMA掩码
}
EXPORT_SYMBOL_GPL(dma_get_required_mask);
#endif

/* 静态初始化早期平台驱动程序列表 */
static __initdata LIST_HEAD(early_platform_driver_list);
/* 静态初始化早期平台设备列表 */
static __initdata LIST_HEAD(early_platform_device_list);

/**
 * early_platform_driver_register - register early platform driver
 * @epdrv: early_platform driver structure
 * @buf: string passed from early_param()
 *
 * Helper function for early_platform_init() / early_platform_init_buffer()
 */
/**
 * early_platform_driver_register - 在系统启动早期注册平台驱动程序
 * @epdrv: 早期平台驱动结构体
 * @buf: 通过 early_param() 传递过来的字符串
 *
 * 这个函数用于 early_platform_init() 或 early_platform_init_buffer() 的辅助功能，
 * 它可以在系统完全启动前注册需要提前加载的驱动。
 */
/*
 * 用于在系统启动早期注册平台驱动。这通常是在内核的初始化阶段完成的，用于设置最基本的硬件设备驱动。
 * 该函数处理从 early_param() 传递过来的字符串参数，将驱动程序添加到一个全局列表，并根据命令行
 * 参数对这些驱动程序进行优先级排序。此外，如果提供了足够的缓冲区，还会将一些额外的参数复制到驱动程序的缓冲区中。
 * 
 * 在系统启动早期注册指定的平台驱动程序，使其可以优先加载和初始化。通过对命令行传递的参数进行解析，
 * 可以设定特定的设备驱动加载顺序，以及传递必要的初始化参数给驱动程序。通过这种方式，系统能够在
 * 启动过程中更有效地管理设备的初始化和资源分配。
 */
int __init early_platform_driver_register(struct early_platform_driver *epdrv,
					  char *buf)
{
	char *tmp; // 用于临时存储字符串解析过程中的位置指针
	int n; // 用于存储字符串长度或位置索引

	/* Simply add the driver to the end of the global list.
	 * Drivers will by default be put on the list in compiled-in order.
	 */
	/* 简单地将驱动程序添加到全局列表的末尾。
	 * 驱动程序将按照编译顺序默认添加到列表中。
	 */
	// 如果此驱动程序尚未添加到全局列表，先初始化它的列表头部，然后添加到早期平台驱动列表尾部
	if (!epdrv->list.next) {
		INIT_LIST_HEAD(&epdrv->list);
		list_add_tail(&epdrv->list, &early_platform_driver_list);
	}

	/* If the user has specified device then make sure the driver
	 * gets prioritized. The driver of the last device specified on
	 * command line will be put first on the list.
	 */
	/* 如果用户已经指定了设备，则确保驱动程序得到优先处理。
	 * 命令行上最后指定的设备的驱动程序将被放到列表的前面。
	 */
	n = strlen(epdrv->pdrv->driver.name);
	// 检查传入的字符串与驱动程序名称是否匹配
	if (buf && !strncmp(buf, epdrv->pdrv->driver.name, n)) {
		// 将此驱动移动到早期平台驱动列表的开始位置，以优先处理
		list_move(&epdrv->list, &early_platform_driver_list);

		/* Allow passing parameters after device name */
		/* 允许在设备名称后传递参数 */
		// 解析可能在设备名称之后传递的参数
		// 如果 buf[n] 是空字符或逗号，设置 requested_id 为 -1。
		if (buf[n] == '\0' || buf[n] == ',')
			epdrv->requested_id = -1;	// 如果没有额外参数，将 requested_id 设置为 -1
		else {
			// 解析从设备名称后开始的第一个参数，将其转换为数字
			epdrv->requested_id = simple_strtoul(&buf[n + 1],
							     &tmp, 10);

			// 检查格式是否正确，如果不正确设置 requested_id 为错误码。
			// 检查参数格式是否正确（期望格式为 ".<number>"）
			if (buf[n] != '.' || (tmp == &buf[n + 1])) {
				epdrv->requested_id = EARLY_PLATFORM_ID_ERROR;	// 参数格式错误
				n = 0;	// 重置 n 为 0，表示没有解析到有效参数
			} else
				// 更新 n 为逗号后的位置，准备解析下一个参数
				n += strcspn(&buf[n + 1], ",") + 1;	// 移动 n 到下一个参数的开始位置。
		}

		// 如果 buf[n] 是逗号，向前移动 n。
		if (buf[n] == ',')	// 如果字符串中还有更多参数，将逗号后的内容复制到驱动程序的缓冲区
			n++;

		// 如果提供的缓冲区大小足够，将参数复制到驱动程序的缓冲区。
		if (epdrv->bufsize) {	// 如果驱动程序提供了缓冲区空间，将命令行中的参数复制到驱动程序的缓冲区
			memcpy(epdrv->buffer, &buf[n],
			       min_t(int, epdrv->bufsize, strlen(&buf[n]) + 1));
			// 确保字符串以空字符结束
			epdrv->buffer[epdrv->bufsize - 1] = '\0';	// 确保字符串正确终止。
		}
	}

	return 0;	// 返回成功。
}

/**
 * early_platform_add_devices - adds a number of early platform devices
 * @devs: array of early platform devices to add
 * @num: number of early platform devices in array
 *
 * Used by early architecture code to register early platform devices and
 * their platform data.
 */
/**
 * early_platform_add_devices - 添加一批早期平台设备
 * @devs: 要添加的早期平台设备数组
 * @num: 数组中早期平台设备的数量
 *
 * 由早期架构代码使用，用于注册早期平台设备及其平台数据。
 */
/*
 * 这段代码的功能是在系统早期注册一系列的平台设备。这通常发生在系统初始化的非常早期阶段，此时大多数服务和驱动还未启动。
 * 早期平台设备通常包括那些对系统启动至关重要的设备，比如时钟管理器或早期的控制设备。通过在早期将这些设备添加到设备列表中，
 * 可以确保它们能被相应的驱动程序在系统启动过程中正确地识别和配置。
 */
void __init early_platform_add_devices(struct platform_device **devs, int num)
{
	struct device *dev;	// 定义设备指针，用于引用当前正在处理的设备
	int i;		// 用于循环的索引变量

	/* simply add the devices to list */
	/* 简单地将设备添加到列表 */
	for (i = 0; i < num; i++) {	// 遍历传入的设备数组
		dev = &devs[i]->dev;			// 获取当前平台设备的设备结构指针

		// 如果设备资源头部的next指针未初始化，初始化列表头并添加到早期平台设备列表
		if (!dev->devres_head.next) {
			INIT_LIST_HEAD(&dev->devres_head);	// 初始化设备资源列表
			list_add_tail(&dev->devres_head,
				      &early_platform_device_list);	 // 将设备资源头部添加到早期平台设备列表
		}
	}
}

/**
 * early_platform_driver_register_all - register early platform drivers
 * @class_str: string to identify early platform driver class
 *
 * Used by architecture code to register all early platform drivers
 * for a certain class. If omitted then only early platform drivers
 * with matching kernel command line class parameters will be registered.
 */
/**
 * early_platform_driver_register_all - 注册所有早期平台驱动程序
 * @class_str: 用于识别早期平台驱动程序类的字符串
 *
 * 该函数被架构代码使用，用于为某一类注册所有早期平台驱动程序。如果省略此参数，
 * 则只会注册与内核命令行类参数匹配的早期平台驱动程序。
 */
void __init early_platform_driver_register_all(char *class_str)
{
	/* The "class_str" parameter may or may not be present on the kernel
	 * command line. If it is present then there may be more than one
	 * matching parameter.
	 *
	 * Since we register our early platform drivers using early_param()
	 * we need to make sure that they also get registered in the case
	 * when the parameter is missing from the kernel command line.
	 *
	 * We use parse_early_options() to make sure the early_param() gets
	 * called at least once. The early_param() may be called more than
	 * once since the name of the preferred device may be specified on
	 * the kernel command line. early_platform_driver_register() handles
	 * this case for us.
	 */
	/* “class_str”参数可能在内核命令行上出现也可能不出现。如果出现，则可能有多个匹配的参数。
	 *
	 * 由于我们使用 early_param() 注册我们的早期平台驱动程序，我们需要确保在内核命令行上
	 * 缺少参数时它们也得到注册。
	 *
	 * 我们使用 parse_early_options() 来确保至少调用一次 early_param()。early_param() 
	 * 可能会被调用多次，因为内核命令行上可能指定了首选设备的名称。early_platform_driver_register() 
	 * 为我们处理了这种情况。
	 */
	parse_early_options(class_str);
}

/**
 * early_platform_match - find early platform device matching driver
 * @epdrv: early platform driver structure
 * @id: id to match against
 */
/**
 * early_platform_match - 查找与驱动程序匹配的早期平台设备
 * @epdrv: 早期平台驱动程序结构
 * @id: 用于匹配的id
 *
 * 此函数用于在早期平台设备列表中查找与指定驱动程序匹配的设备。
 * 如果找到匹配的设备，且设备ID与提供的ID相符，就返回该设备的指针。
 * 如果没有找到匹配的设备，返回NULL。
 */
static  __init struct platform_device *
early_platform_match(struct early_platform_driver *epdrv, int id)
{
	struct platform_device *pd;

	// 遍历早期平台设备列表
	list_for_each_entry(pd, &early_platform_device_list, dev.devres_head)
		// 使用 platform_match 函数检查设备和驱动程序是否匹配
		if (platform_match(&pd->dev, &epdrv->pdrv->driver))
			// 如果设备ID与提供的ID相匹配，则返回该设备
			if (pd->id == id)
				return pd;

	// 如果没有找到匹配的设备，返回NULL
	return NULL;
}

/**
 * early_platform_left - check if early platform driver has matching devices
 * @epdrv: early platform driver structure
 * @id: return true if id or above exists
 */
/**
 * early_platform_left - 检查早期平台驱动程序是否有匹配的设备
 * @epdrv: 早期平台驱动程序结构
 * @id: 如果存在id或更高的id则返回真
 *
 * 此函数用于确定是否有匹配给定驱动程序的早期平台设备，其设备ID等于或高于指定的ID。
 * 如果找到至少一个符合条件的设备，函数返回1，表示还有设备未处理。
 * 如果没有符合条件的设备，函数返回0。
 */
static  __init int early_platform_left(struct early_platform_driver *epdrv,
				       int id)
{
	struct platform_device *pd;

	// 遍历早期平台设备列表
	list_for_each_entry(pd, &early_platform_device_list, dev.devres_head)
		// 使用 platform_match 函数检查设备和驱动程序是否匹配
		if (platform_match(&pd->dev, &epdrv->pdrv->driver))
			// 检查设备ID是否大于或等于给定的ID
			if (pd->id >= id)
				return 1;  // 如果找到符合条件的设备，返回1

	// 如果没有找到任何符合条件的设备，返回0
	return 0;
}

/**
 * early_platform_driver_probe_id - probe drivers matching class_str and id
 * @class_str: string to identify early platform driver class
 * @id: id to match against
 * @nr_probe: number of platform devices to successfully probe before exiting
 */
/**
 * early_platform_driver_probe_id - 根据类别字符串和ID探测驱动
 * @class_str: 用于识别早期平台驱动类别的字符串
 * @id: 用于匹配的ID
 * @nr_probe: 在退出前成功探测的平台设备数量
 *
 * 此函数旨在根据指定的类别字符串和ID，探测并激活早期平台设备。
 * 它尝试激活指定数量的设备，一旦达到这个数量，就会停止探测。
 */
/*
 * 遍历所有早期平台驱动程序，检查是否有与给定条件匹配的设备，如果有，则尝试使用驱动程序的
 * probe函数来激活这些设备。如果指定数量的设备已被成功激活，或者没有更多匹配的设备可供激活，则函数将停止并返回。
 */
static int __init early_platform_driver_probe_id(char *class_str,
						 int id,
						 int nr_probe)
{
	struct early_platform_driver *epdrv;  // 早期平台驱动程序的结构体指针
	struct platform_device *match;        // 匹配的平台设备
	int match_id;                        // 用于匹配的ID
	int n = 0;                           // 已成功探测的设备计数
	int left = 0;                        // 剩余未处理的设备计数

	// 遍历所有早期平台驱动程序
	list_for_each_entry(epdrv, &early_platform_driver_list, list) {
		/* only use drivers matching our class_str */
		// 只处理与指定类别字符串匹配的驱动程序
		if (strcmp(class_str, epdrv->class_str))
			continue;

		// 如果没有指定id (-2), 使用驱动程序的requested_id
		if (id == -2) {	// 如果ID为-2，则使用请求的ID
			match_id = epdrv->requested_id;
			left = 1;

		} else {
			match_id = id;
			// 检查是否还有匹配的设备未处理
			left += early_platform_left(epdrv, id);

			/* skip requested id */
			// 跳过已请求的ID
			// 如果请求的id与当前id相同，设置match_id为未设置
			switch (epdrv->requested_id) {
			case EARLY_PLATFORM_ID_ERROR:
			case EARLY_PLATFORM_ID_UNSET:
				break;
			default:
				if (epdrv->requested_id == id)
					match_id = EARLY_PLATFORM_ID_UNSET;
			}
		}

		// 根据match_id进行匹配
		switch (match_id) {
		case EARLY_PLATFORM_ID_ERROR:
			// 打印警告信息，参数解析失败
			pr_warning("%s: unable to parse %s parameter\n",
				   class_str, epdrv->pdrv->driver.name);
			/* fall-through */
			// 继续下一步操作
		case EARLY_PLATFORM_ID_UNSET:
			match = NULL;	// 未设置匹配设备
			break;
		default:
			// 尝试匹配设备
			match = early_platform_match(epdrv, match_id);
		}

		// 如果找到匹配的设备并成功探测
		if (match) {
			if (epdrv->pdrv->probe(match))
				// 如果探测失败，打印警告信息
				pr_warning("%s: unable to probe %s early.\n",
					   class_str, match->name);
			else
				n++;	// 探测成功，增加计数
		}

		// 如果已探测到足够数量的设备，则停止
		if (n >= nr_probe)
			break;
	}

	// 如果还有未处理的设备，返回已探测的数量；否则返回错误
	if (left)
		return n;
	else
		return -ENODEV;
}

/**
 * early_platform_driver_probe - probe a class of registered drivers
 * @class_str: string to identify early platform driver class
 * @nr_probe: number of platform devices to successfully probe before exiting
 * @user_only: only probe user specified early platform devices
 *
 * Used by architecture code to probe registered early platform drivers
 * within a certain class. For probe to happen a registered early platform
 * device matching a registered early platform driver is needed.
 */
/**
 * early_platform_driver_probe - 探测已注册驱动的一类
 * @class_str: 用来识别早期平台驱动类别的字符串
 * @nr_probe: 在退出前成功探测的平台设备数量
 * @user_only: 仅探测用户指定的早期平台设备
 *
 * 由架构代码使用，用于探测特定类别内的已注册早期平台驱动。
 * 要进行探测，需要一个已注册的早期平台设备与一个已注册的早期平台驱动相匹配。
 */
int __init early_platform_driver_probe(char *class_str,
				       int nr_probe,
				       int user_only)
{
	int k, n, i; // 定义整型变量用于计数和循环

	n = 0;  // 初始化已成功探测的设备计数
	for (i = -2; n < nr_probe; i++) {  // 从i = -2开始循环，直到探测到足够的设备
		k = early_platform_driver_probe_id(class_str, i, nr_probe - n);  // 探测指定类别和ID的设备

		if (k < 0)  // 如果探测返回错误，中断循环
			break;

		n += k;  // 增加已成功探测的设备计数

		if (user_only)  // 如果仅探测用户指定的设备，结束循环
			break;
	}

	return n;  // 返回已成功探测的设备数量
}

/**
 * early_platform_cleanup - clean up early platform code
 */
/**
 * early_platform_cleanup - 清理早期平台代码
 *
 * 该函数用于清理在系统启动早期阶段注册的平台设备列表。
 */
void __init early_platform_cleanup(void)
{
	struct platform_device *pd, *pd2;	// 定义两个指向平台设备结构体的指针，用于迭代和安全删除

	/* clean up the devres list used to chain devices */
	/* 清理用于链接设备的 devres 列表 */
	list_for_each_entry_safe(pd, pd2, &early_platform_device_list,
				 dev.devres_head) {	// 安全地遍历早期平台设备列表
		list_del(&pd->dev.devres_head);  // 从列表中删除当前设备
		memset(&pd->dev.devres_head, 0, sizeof(pd->dev.devres_head));  // 清零设备的资源头部，防止悬挂指针
	}
}

