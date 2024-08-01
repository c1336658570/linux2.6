/*
 * platform_device.h - generic, centralized driver model
 *
 * Copyright (c) 2001-2003 Patrick Mochel <mochel@osdl.org>
 *
 * This file is released under the GPLv2
 *
 * See Documentation/driver-model/ for more information.
 */

#ifndef _PLATFORM_DEVICE_H_
#define _PLATFORM_DEVICE_H_

#include <linux/device.h>
#include <linux/mod_devicetable.h>

struct platform_device {
	const char	* name;		// 设备名称
	int		id;							// 设备标识符
	struct device	dev;		// 设备模型核心结构

	u32		num_resources;	// 设备使用的资源数量
	struct resource	* resource;	// 指向设备资源数组的指针

	const struct platform_device_id	*id_entry;	// 指向设备ID条目，用于设备与驱动的匹配

	/* arch specific additions */
	/* 架构特定的额外信息 */
	struct pdev_archdata	archdata;   // 包含平台特定数据
};

// 定义一个宏，用于获取平台设备的ID条目。这个ID条目通常用于识别设备并与相应的驱动进行匹配。
#define platform_get_device_id(pdev)	((pdev)->id_entry)

// 定义一个宏，将给定的device结构体指针转换为包含它的platform_device结构体指针。
// 这是一个常用的技巧，用于从结构体的一个成员的地址获取整个结构体的地址。
#define to_platform_device(x) container_of((x), struct platform_device, dev)

// 声明一个外部函数，用于注册一个平台设备。这个函数通常在驱动的初始化代码中被调用，以通知内核有一个新的设备需要管理。
extern int platform_device_register(struct platform_device *);
// 声明一个外部函数，用于注销一个已注册的平台设备。通常在设备不再需要或驱动被卸载时调用。
extern void platform_device_unregister(struct platform_device *);

// 声明一个外部变量，表示平台总线类型。这是一个结构体，包含了与平台总线相关的各种操作和属性。
extern struct bus_type platform_bus_type;
// 声明一个外部变量，表示平台总线设备。这个设备是平台总线的一部分，作为平台设备挂载的根。
extern struct device platform_bus;

// 声明一个函数用于从指定的平台设备获取资源。这个函数允许访问设备的物理资源（如内存和I/O端口）。
extern struct resource *platform_get_resource(struct platform_device *, unsigned int, unsigned int);
// 声明一个函数用于从平台设备获取中断号。该函数基于中断资源的索引返回对应的中断号。
extern int platform_get_irq(struct platform_device *, unsigned int);
// 声明一个函数，用于通过资源名称从平台设备中获取资源。这允许开发者通过资源的名称而非索引来访问资源。
extern struct resource *platform_get_resource_byname(struct platform_device *, unsigned int, const char *);
// 声明一个函数，用于通过中断名称从平台设备获取中断号。这使得可以通过中断资源的名称来检索中断号。
extern int platform_get_irq_byname(struct platform_device *, const char *);
// 声明一个函数，用于批量注册平台设备。这个函数接受一个平台设备指针数组和设备数量，用于一次性注册多个设备。
extern int platform_add_devices(struct platform_device **, int);

// 声明一个函数，用于注册一个简单的平台设备。这个函数简化了平台设备的注册过程，只需要指定设备名、ID和资源列表。
extern struct platform_device *platform_device_register_simple(const char *, int id,
					struct resource *, unsigned int);
// 声明一个函数，用于携带附加数据注册平台设备。这允许在注册设备时提供额外的数据，例如设备配置信息或平台特定数据。
extern struct platform_device *platform_device_register_data(struct device *,
		const char *, int, const void *, size_t);

// 声明一个函数，用于分配一个新的平台设备。这个函数仅初始化设备结构体并设置设备的名称和ID，但不注册设备。
extern struct platform_device *platform_device_alloc(const char *name, int id);
// 声明一个函数，用于给已分配的平台设备添加资源。这个函数接收资源数组和数量，将它们关联到指定的平台设备上。
extern int platform_device_add_resources(struct platform_device *pdev, struct resource *res, unsigned int num);
// 声明一个函数，用于给平台设备添加附加数据。这允许为设备提供初始化数据或配置参数。
extern int platform_device_add_data(struct platform_device *pdev, const void *data, size_t size);
// 声明一个函数，用于注册一个已经分配并配置了资源和数据的平台设备到系统中。
extern int platform_device_add(struct platform_device *pdev);
// 声明一个函数，用于从系统中删除一个平台设备。这包括注销设备和释放与设备关联的所有资源。
extern void platform_device_del(struct platform_device *pdev);
// 声明一个函数，用于减少平台设备的引用计数。当引用计数达到零时，会自动释放设备及其资源。
extern void platform_device_put(struct platform_device *pdev);

struct platform_driver {
	int (*probe)(struct platform_device *);		// 定义一个回调函数，当平台设备与驱动匹配并尝试进行绑定时调用。
	int (*remove)(struct platform_device *);	// 定义一个回调函数，当平台设备从驱动注销时调用。
	void (*shutdown)(struct platform_device *);	// 定义一个回调函数，用于处理设备关机时的操作。
	int (*suspend)(struct platform_device *, pm_message_t state);	// 定义一个回调函数，用于处理设备的挂起操作。
	int (*resume)(struct platform_device *);	// 定义一个回调函数，用于处理设备的恢复操作。
	struct device_driver driver;	// 嵌入的设备驱动结构体，包含驱动的核心功能和属性。
	const struct platform_device_id *id_table;	// 指向支持的设备ID表的指针，用于设备与驱动的匹配。
};

// 声明一个函数用于注册一个平台驱动。这使得驱动可以接收关于新设备的通知。
extern int platform_driver_register(struct platform_driver *);
// 声明一个函数用于注销一个平台驱动。这通常在驱动不再需要时调用，以清理所有关联的资源。
extern void platform_driver_unregister(struct platform_driver *);

/* non-hotpluggable platform devices may use this so that probe() and
 * its support may live in __init sections, conserving runtime memory.
 */
/* 非热插拔平台设备可能使用此方法，以便probe()及其支持代码可以位于__init节中，从而节省运行时内存。 */
// 声明一个函数，允许直接注册驱动并提供一个自定义的probe函数。这样可以避免驱动自动连接到设备，使得初始化代码可以在不需要时被丢弃。
extern int platform_driver_probe(struct platform_driver *driver,
		int (*probe)(struct platform_device *));

// 宏定义，用于获取存储在平台设备中的驱动数据。
#define platform_get_drvdata(_dev)	dev_get_drvdata(&(_dev)->dev)
// 宏定义，用于设置存储在平台设备中的驱动数据。
#define platform_set_drvdata(_dev,data)	dev_set_drvdata(&(_dev)->dev, (data))

// 声明一个函数，用于创建一个包含多个资源的平台设备，并可选地关联一个驱动和初始化数据。这个函数可以简化具有多个资源（如内存区域、中断等）的设备的初始化过程。
extern struct platform_device *platform_create_bundle(struct platform_driver *driver,
					int (*probe)(struct platform_device *),
					struct resource *res, unsigned int n_res,
					const void *data, size_t size);

/* early platform driver interface */
/* 早期平台驱动程序接口 */
struct early_platform_driver {
	const char *class_str; // 驱动类的字符串表示，用于匹配特定类的设备
	struct platform_driver *pdrv; // 指向相关平台驱动的指针
	struct list_head list; // 链表头，用于将多个早期驱动组织在一起
	int requested_id; // 请求的设备ID，用于驱动匹配
	char *buffer; // 指向缓冲区的指针，可能用于存储临时数据
	int bufsize; // 缓冲区的大小，以字节为单位
};

/* 定义两个常量，表示未设置的早期平台设备 ID 和错误的设备 ID */
#define EARLY_PLATFORM_ID_UNSET -2 // 未设置的设备 ID
#define EARLY_PLATFORM_ID_ERROR -3 // 错误的设备 ID

/* 注册一个早期平台驱动程序 */
extern int early_platform_driver_register(struct early_platform_driver *epdrv,
					  char *buf);
/* 为一组早期平台设备添加设备 */
extern void early_platform_add_devices(struct platform_device **devs, int num);

/* 内联函数，用于检查一个平台设备是否是早期平台设备 */
static inline int is_early_platform_device(struct platform_device *pdev)
{
	return !pdev->dev.driver; // 如果设备没有关联的驱动程序，则视为早期平台设备
}

/* 注册给定类字符串的所有早期平台驱动程序 */
extern void early_platform_driver_register_all(char *class_str);
/* 尝试探测给定类字符串的早期平台设备，探测指定数量，如果 user_only 为真，则仅探测用户指定的设备 */
extern int early_platform_driver_probe(char *class_str,
				       int nr_probe, int user_only);
/* 清理所有早期平台驱动程序 */
extern void early_platform_cleanup(void);

// 这个宏提供了一个简便的方法来初始化一个早期平台驱动，不需要额外的缓冲区。
#define early_platform_init(class_string, platdrv)		\
	early_platform_init_buffer(class_string, platdrv, NULL, 0)

// 如果不是模块编译，定义一个更复杂的宏来初始化带缓冲区的早期平台驱动。
#ifndef MODULE
#define early_platform_init_buffer(class_string, platdrv, buf, bufsiz)	\
static __initdata struct early_platform_driver early_driver = {		\
	.class_str = class_string,					\
	.buffer = buf,							\
	.bufsize = bufsiz,						\
	.pdrv = platdrv,						\
	.requested_id = EARLY_PLATFORM_ID_UNSET,			\
};									\
static int __init early_platform_driver_setup_func(char *buffer)	\
{									\
	return early_platform_driver_register(&early_driver, buffer);	\
}									\
early_param(class_string, early_platform_driver_setup_func)
// 以上代码块定义了一个早期平台驱动结构体，并注册了一个初始化函数，该函数会在早期参数解析时调用，以注册早期平台驱动。
#else /* MODULE */
// 对于模块编译，定义一个简单的宏来处理带缓冲区的早期平台设备驱动的初始化。
// 这一部分为模块提供了一个处理早期平台设备驱动初始化的简化版本。
#define early_platform_init_buffer(class_string, platdrv, buf, bufsiz)	\
static inline char *early_platform_driver_setup_func(void)		\
{									\
	return bufsiz ? buf : NULL;					\
}
#endif /* MODULE */

#endif /* _PLATFORM_DEVICE_H_ */
