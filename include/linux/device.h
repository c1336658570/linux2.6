/*
 * device.h - generic, centralized driver model
 *
 * Copyright (c) 2001-2003 Patrick Mochel <mochel@osdl.org>
 * Copyright (c) 2004-2009 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2008-2009 Novell Inc.
 *
 * This file is released under the GPLv2
 *
 * See Documentation/driver-model/ for more information.
 */

#ifndef _DEVICE_H_
#define _DEVICE_H_

#include <linux/ioport.h>
#include <linux/kobject.h>
#include <linux/klist.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/semaphore.h>
#include <asm/atomic.h>
#include <asm/device.h>

// 前置声明，为了后续的类型引用
struct device;
struct device_private;
struct device_driver;
struct driver_private;
struct class;
struct class_private;
struct bus_type;
struct bus_type_private;

// 表示总线属性的结构体
struct bus_attribute {
	struct attribute	attr;	// 嵌入的基本属性
	// 显示属性值的函数
	ssize_t (*show)(struct bus_type *bus, char *buf);
	// 存储属性值的函数
	ssize_t (*store)(struct bus_type *bus, const char *buf, size_t count);
};

// 定义和初始化总线属性
#define BUS_ATTR(_name, _mode, _show, _store)	\
struct bus_attribute bus_attr_##_name = __ATTR(_name, _mode, _show, _store)

// 创建和移除总线属性文件的函数
extern int __must_check bus_create_file(struct bus_type *,
					struct bus_attribute *);
extern void bus_remove_file(struct bus_type *, struct bus_attribute *);

// 总线类型结构体
struct bus_type {
	const char		*name;    // 总线的名称
	struct bus_attribute	*bus_attrs;    // 总线级别的属性
	struct device_attribute	*dev_attrs;    // 设备级别的属性
	struct driver_attribute	*drv_attrs;    // 驱动级别的属性

	// 总线提供的回调函数，用于处理设备和驱动程序之间的匹配、事件和状态变化
	int (*match)(struct device *dev, struct device_driver *drv);  // 匹配设备与驱动
	int (*uevent)(struct device *dev, struct kobj_uevent_env *env); // 处理设备的uevent
	int (*probe)(struct device *dev);   // 探测设备
	int (*remove)(struct device *dev);  // 移除设备
	void (*shutdown)(struct device *dev);  // 关闭设备

	// 设备的电源管理操作
	int (*suspend)(struct device *dev, pm_message_t state);  // 挂起设备
	int (*resume)(struct device *dev);   // 恢复设备

	const struct dev_pm_ops *pm;  // 指向电源管理操作结构体的指针

	struct bus_type_private *p;  // 指向私有数据结构体的指针
};

// 总线注册与注销
extern int __must_check bus_register(struct bus_type *bus);
extern void bus_unregister(struct bus_type *bus);

// 重新扫描总线上的设备
extern int __must_check bus_rescan_devices(struct bus_type *bus);

/* iterator helpers for buses */
/* 总线上设备的迭代助手 */

// 遍历总线上的所有设备
int bus_for_each_dev(struct bus_type *bus, struct device *start, void *data,
		     int (*fn)(struct device *dev, void *data));
// 在总线上查找设备，根据匹配函数
struct device *bus_find_device(struct bus_type *bus, struct device *start,
			       void *data,
			       int (*match)(struct device *dev, void *data));
// 通过名称在总线上查找设备
struct device *bus_find_device_by_name(struct bus_type *bus,
				       struct device *start,
				       const char *name);

// 遍历总线上的所有驱动程序
int __must_check bus_for_each_drv(struct bus_type *bus,
				  struct device_driver *start, void *data,
				  int (*fn)(struct device_driver *, void *));

// 根据指定的比较函数，对总线上的设备进行排序
void bus_sort_breadthfirst(struct bus_type *bus,
			   int (*compare)(const struct device *a,
					  const struct device *b));
/*
 * Bus notifiers: Get notified of addition/removal of devices
 * and binding/unbinding of drivers to devices.
 * In the long run, it should be a replacement for the platform
 * notify hooks.
 */
/*
 * 总线通知器：在设备添加/移除和驱动程序与设备的绑定/解绑时获取通知。
 * 长远来看，这应该取代平台的通知钩子。
 */
struct notifier_block;

// 注册和注销总线通知器
// 允许模块注册自己的通知器，以便在设备或驱动程序的状态发生变化时接收通知。
extern int bus_register_notifier(struct bus_type *bus,
				 struct notifier_block *nb);
extern int bus_unregister_notifier(struct bus_type *bus,
				   struct notifier_block *nb);

/* All 4 notifers below get called with the target struct device *
 * as an argument. Note that those functions are likely to be called
 * with the device lock held in the core, so be careful.
 */
/* 所有以下四种通知类型的回调函数均以目标设备结构体作为参数。
 * 请注意，这些函数可能会在核心持有设备锁的情况下被调用，因此请谨慎处理。
 */
/* 设备被添加 */
#define BUS_NOTIFY_ADD_DEVICE		0x00000001 /* device added */
/* 设备被移除 */
#define BUS_NOTIFY_DEL_DEVICE		0x00000002 /* device removed */
/* 驱动程序绑定到设备 */
#define BUS_NOTIFY_BOUND_DRIVER		0x00000003 /* driver bound to device */
/* 驱动程序即将解绑 */
#define BUS_NOTIFY_UNBIND_DRIVER	0x00000004 /* driver about to be
						      unbound */
/* 驱动程序已从设备解绑 */
#define BUS_NOTIFY_UNBOUND_DRIVER	0x00000005 /* driver is unbound
						      from the device */

// 获取总线的 kset 结构体
extern struct kset *bus_get_kset(struct bus_type *bus);
// 获取总线的设备列表
extern struct klist *bus_get_device_klist(struct bus_type *bus);

// 设备驱动程序结构体
struct device_driver {
	const char		*name;	// 驱动程序的名称
	struct bus_type		*bus;	// 驱动程序所属的总线类型

	struct module		*owner;	// 拥有这个驱动程序的模块
	// 内置模块使用的模块名称
	const char		*mod_name;	/* used for built-in modules */

	// 是否通过 sysfs 禁用绑定/解绑
	bool suppress_bind_attrs;	/* disables bind/unbind via sysfs */

	int (*probe)(struct device *dev);   // 探测设备
	int (*remove)(struct device *dev);  // 移除设备
	void (*shutdown)(struct device *dev); // 关闭设备
	int (*suspend)(struct device *dev, pm_message_t state); // 设备挂起
	int (*resume)(struct device *dev);  // 设备恢复

	const struct attribute_group **groups; // 属性组

	const struct dev_pm_ops *pm; // 设备电源管理操作

	struct driver_private *p; // 私有数据
};

// 注册和注销设备驱动程序
extern int __must_check driver_register(struct device_driver *drv);
extern void driver_unregister(struct device_driver *drv);

// 获取和释放设备驱动程序的引用
extern struct device_driver *get_driver(struct device_driver *drv);
extern void put_driver(struct device_driver *drv);
// 根据名称和总线类型查找设备驱动程序
extern struct device_driver *driver_find(const char *name,
					 struct bus_type *bus);
// 检查驱动程序探测是否完成
extern int driver_probe_done(void);
// 等待设备探测完成
extern void wait_for_device_probe(void);


/* sysfs interface for exporting driver attributes */
/* sysfs 接口用于导出驱动属性 */

// 表示驱动属性的结构体
struct driver_attribute {
	struct attribute attr;	// 基本属性结构
	// 显示属性的函数
	ssize_t (*show)(struct device_driver *driver, char *buf);
	// 存储属性的函数
	ssize_t (*store)(struct device_driver *driver, const char *buf,
			 size_t count);
};

// 定义和初始化一个驱动属性
#define DRIVER_ATTR(_name, _mode, _show, _store)	\
struct driver_attribute driver_attr_##_name =		\
	__ATTR(_name, _mode, _show, _store)

// 创建和移除与驱动关联的 sysfs 文件
extern int __must_check driver_create_file(struct device_driver *driver,
					const struct driver_attribute *attr);
extern void driver_remove_file(struct device_driver *driver,
			       const struct driver_attribute *attr);

// 向驱动程序添加自定义 kobject
extern int __must_check driver_add_kobj(struct device_driver *drv,
					struct kobject *kobj,
					const char *fmt, ...);

// 遍历与驱动关联的所有设备
extern int __must_check driver_for_each_device(struct device_driver *drv,
					       struct device *start,
					       void *data,
					       int (*fn)(struct device *dev,
							 void *));
// 根据指定条件查找与驱动关联的设备
struct device *driver_find_device(struct device_driver *drv,
				  struct device *start, void *data,
				  int (*match)(struct device *dev, void *data));

/*
 * device classes
 */
/*
 * 设备类
 */
struct class {
	const char		*name;          // 类的名称
	struct module		*butner;         // 拥有此类的模块

	struct class_attribute		*class_attrs;   // 类属性
	struct device_attribute		*dev_attrs;     // 设备属性
	/**
	 * dev_kobj是一个kobject指针。在device注册时，会在/sys/dev下创建名为自己设备号的软链接。
	 * 但设备不知道自己属于块设备还是字符设备，所以会请示自己所属的class，
	 * class就是用dev_kobj记录本类设备应属于的哪种设备。表示该class下的设备在/sys/dev/下的目录，
	 * 现在一般有char和block两个，如果dev_kobj为NULL，则默认选择char。
	 */
	struct kobject			*dev_kobj;      // 设备的 kobject

	int (*dev_uevent)(struct device *dev, struct kobj_uevent_env *env);  // 设备的 uevent 处理函数
	char *(*devnode)(struct device *dev, mode_t *mode);  // 设备节点名称生成函数

	void (*class_release)(struct class *class);  // 类的释放函数
	void (*dev_release)(struct device *dev);  // 设备的释放函数

	int (*suspend)(struct device *dev, pm_message_t state);  // 设备的挂起函数
	int (*resume)(struct device *dev);  // 设备的恢复函数

	const struct dev_pm_ops *pm;  // 指向电源管理操作的指针

	struct class_private *p;  // 私有数据
};

// class为了遍历设备链表，特意定义了专门的结构和遍历函数（在class.h和class.c中），实现如下。
// class为了遍历设备链表，特意定义了专门的结构和遍历函数，实现如下。
// 之所以要如此费一番周折，在klist_iter外面加上这一层封装，完全是为了对链表进行选择性遍历。
// 选择的条件就是device_type。device_type是在device结构中使用的类型，
// 其中定义了相似设备使用的一些处理操作，可以说比class的划分还要小一层。
struct class_dev_iter {
	struct klist_iter		ki;  // klist 迭代器
	const struct device_type	*type;  // 设备类型
};

// 用于 sysfs 的设备类 kobjects
extern struct kobject *sysfs_dev_block_kobj;
extern struct kobject *sysfs_dev_char_kobj;
// 注册和注销设备类
extern int __must_check __class_register(struct class *class,
					 struct lock_class_key *key);
extern void class_unregister(struct class *class);

/* This is a #define to keep the compiler from merging different
 * instances of the __key variable */
// 定义用于注册类的宏，以防止编译器合并不同实例的 __key 变量
/**
 * class_register()将class注册到系统中。之所以把class_register()写成宏定义的形式，
 * 似乎是为了__key的不同实例合并，在__class_register()中确实使用了__key，
 * 但是是为了调试class中使用的mutex用的。__key的类型lock_class_key是只有使用LOCKDEP定义时才会有内容，
 * 写成这样也许是为了在lock_class_key定义为空时减少一些不必要的空间消耗。
 */
#define class_register(class)			\
({						\
	static struct lock_class_key __key;	\
	__class_register(class, &__key);	\
})

// 兼容性类结构和函数
struct class_compat;
struct class_compat *class_compat_register(const char *name);
void class_compat_unregister(struct class_compat *cls);
int class_compat_create_link(struct class_compat *cls, struct device *dev,
			     struct device *device_link);
void class_compat_remove_link(struct class_compat *cls, struct device *dev,
			      struct device *device_link);

// 设备类迭代器初始化和管理
extern void class_dev_iter_init(struct class_dev_iter *iter,
				struct class *class,
				struct device *start,
				const struct device_type *type);
extern struct device *class_dev_iter_next(struct class_dev_iter *iter);
extern void class_dev_iter_exit(struct class_dev_iter *iter);

// 遍历类中的设备
extern int class_for_each_device(struct class *class, struct device *start,
				 void *data,
				 int (*fn)(struct device *dev, void *data));
extern struct device *class_find_device(struct class *class,
					struct device *start, void *data,
					int (*match)(struct device *, void *));

// 设备类属性结构体
struct class_attribute {
	struct attribute attr; // 基本属性
	ssize_t (*show)(struct class *class, struct class_attribute *attr, char *buf); // 显示属性的函数
	ssize_t (*store)(struct class *class, struct class_attribute *attr, const char *buf, size would count); // 存储属性的函数
};

// 定义并初始化设备类属性
#define CLASS_ATTR(_name, _mode, _show, _store)			\
struct class_attribute class_attr_##_name = __ATTR(_name, _mode, _show, _store)

// 创建和移除类属性文件的函数
extern int __must_check class_create_file(struct class *class,
					  const struct class_attribute *attr);
extern void class_remove_file(struct class *class,
			      const struct class_attribute *attr);

/* Simple class attribute that is just a static string */
/* 简单的类属性，仅为静态字符串 */

struct class_attribute_string {
	struct class_attribute attr;	// 类属性
	char *str;										// 字符串值
};

/* Currently read-only only */
/* 当前仅支持只读 */
#define _CLASS_ATTR_STRING(_name, _mode, _str) \
	{ __ATTR(_name, _mode, show_class_attr_string, NULL), _str }
#define CLASS_ATTR_STRING(_name, _mode, _str) \
	struct class_attribute_string class_attr_##_name = \
		_CLASS_ATTR_STRING(_name, _mode, _str)

// 显示静态字符串类属性的函数
extern ssize_t show_class_attr_string(struct class *class, struct class_attribute *attr,
                        char *buf);

// 设备类接口结构体
struct class_interface {
	struct list_head	node;		// 节点，被插入到class_private中的class_interfaces中。
	struct class		*class;		// 设备类

	// 添加设备时的回调函数
	int (*add_dev)		(struct device *, struct class_interface *);
	// 移除设备时的回调函数
	void (*remove_dev)	(struct device *, struct class_interface *);
};

// 注册和注销设备类接口的函数
extern int __must_check class_interface_register(struct class_interface *);
extern void class_interface_unregister(struct class_interface *);

// 创建和销毁设备类的函数
extern struct class * __must_check __class_create(struct module *owner,
						  const char *name,
						  struct lock_class_key *key);
extern void class_destroy(struct class *cls);

/* This is a #define to keep the compiler from merging different
 * instances of the __key variable */
// 定义用于创建设备类的宏，以防止编译器合并不同实例的 __key 变量
#define class_create(owner, name)		\
({						\
	static struct lock_class_key __key;	\
	__class_create(owner, name, &__key);	\
})

/*
 * The type of device, "struct device" is embedded in. A class
 * or bus can contain devices of different types
 * like "partitions" and "disks", "mouse" and "event".
 * This identifies the device type and carries type-specific
 * information, equivalent to the kobj_type of a kobject.
 * If "name" is specified, the uevent will contain it in
 * the DEVTYPE variable.
 */
/*
 * 设备类型，"struct device" 嵌入在内的类型。一个类或总线可以包含不同类型的设备，
 * 如 "分区" 和 "硬盘"，"鼠标" 和 "事件"。这用于标识设备类型并携带类型特定信息，
 * 等同于 kobject 的 kobj_type。如果指定了 "name"，则 uevent 将包含此信息在
 * DEVTYPE 变量中。
 */
struct device_type {
	const char *name;  // 设备类型名称
	const struct attribute_group **groups;  // 属性组
	int (*uevent)(struct device *dev, struct kobj_uevent_env *env);  // uevent 事件处理函数
	char *(*devnode)(struct device *dev, mode_t *mode);  // 设备节点名称生成函数
	void (*release)(struct device *dev);  // 设备释放函数

	const struct dev_pm_ops *pm;  // 电源管理操作
};

/* interface for exporting device attributes */
/* 用于导出设备属性的接口 */
struct device_attribute {
	struct attribute	attr;  // 基本属性
	// 显示属性值的函数
	ssize_t (*show)(struct device *dev, struct device_attribute *attr, char *buf);
	// 存储属性值的函数
	ssize_t (*store)(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
};

// 定义和初始化设备属性
#define DEVICE_ATTR(_name, _mode, _show, _store) \
struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)

// 创建和移除设备属性文件的函数
extern int __must_check device_create_file(struct device *device,
					const struct device_attribute *entry);
extern void device_remove_file(struct device *dev,
			       const struct device_attribute *attr);
// 创建一个二进制设备属性文件
extern int __must_check device_create_bin_file(struct device *dev,
					const struct bin_attribute *attr);
// 移除一个二进制设备属性文件
extern void device_remove_bin_file(struct device *dev,
				   const struct bin_attribute *attr);
// 调度设备回调函数，可指定模块拥有者
extern int device_schedule_callback_owner(struct device *dev,
		void (*func)(struct device *dev), struct module *owner);

/* This is a macro to avoid include problems with THIS_MODULE */
// 用宏定义简化设备回调函数的调度
#define device_schedule_callback(dev, func)			\
	device_schedule_callback_owner(dev, func, THIS_MODULE)	// 定义宏以避免引用 THIS_MODULE 时的编译问题

/* device resource management */
/* 设备资源管理 */
// 定义资源释放函数类型
typedef void (*dr_release_t)(struct device *dev, void *res);
// 定义资源匹配函数类型
typedef int (*dr_match_t)(struct device *dev, void *res, void *match_data);

#ifdef CONFIG_DEBUG_DEVRES
// 在调试模式下分配设备资源
extern void *__devres_alloc(dr_release_t release, size_t size, gfp_t gfp,
			     const char *name);
// 定义宏以简化资源分配函数调用
#define devres_alloc(release, size, gfp) \
	__devres_alloc(release, size, gfp, #release)
#else
// 分配设备资源
extern void *devres_alloc(dr_release_t release, size_t size, gfp_t gfp);
#endif
// 释放设备资源
extern void devres_free(void *res);
// 将资源添加到设备
extern void devres_add(struct device *dev, void *res);
// 查找设备资源
extern void *devres_find(struct device *dev, dr_release_t release,
			 dr_match_t match, void *match_data);
// 获取设备资源
extern void *devres_get(struct device *dev, void *new_res,
			dr_match_t match, void *match_data);
// 移除设备资源
extern void *devres_remove(struct device *dev, dr_release_t release,
			   dr_match_t match, void *match_data);
// 销毁设备资源
extern int devres_destroy(struct device *dev, dr_release_t release,
			  dr_match_t match, void *match_data);

/* devres group */
/* 设备资源组 */
// 打开一个设备资源组
extern void * __must_check devres_open_group(struct device *dev, void *id,
					     gfp_t gfp);
// 关闭一个设备资源组
extern void devres_close_group(struct device *dev, void *id);
// 移除一个设备资源组
extern void devres_remove_group(struct device *dev, void *id);
// 释放一个设备资源组
extern int devres_release_group(struct device *dev, void *id);

/* managed kzalloc/kfree for device drivers, no kmalloc, always use kzalloc */
/* 为设备驱动提供的托管 kzalloc/kfree 功能，无需使用 kmalloc，始终使用 kzalloc */
// 为设备分配内存
extern void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp);
// 释放设备分配的内存
extern void devm_kfree(struct device *dev, void *p);

struct device_dma_parameters {
	/*
	 * a low level driver may set these to teach IOMMU code about
	 * sg limitations.
	 */
	/*
	 * 低级驱动程序可以设置这些参数，以便教导IOMMU（输入输出内存管理单元）代码关于
	 * 分散/聚集限制的信息。
	 */
	unsigned int max_segment_size;         // 单个分散/聚集段的最大尺寸
	unsigned long segment_boundary_mask;   // 分散/聚集段的边界掩码
};

struct device {
	// 父设备
	struct device		*parent;

	// 指向设备私有数据的指针
	struct device_private	*p;

	// 设备的内核对象，用于sysfs展示
	struct kobject kobj;
	/* 设备初始化时的名字 */
	const char		*init_name; /* initial name of the device */
	// 设备类型
	struct device_type	*type;

	/* 用于同步对驱动程序的调用的信号量 */
	struct semaphore	sem;	/* semaphore to synchronize calls to
					 * its driver.
					 */

	/* 设备所在的总线类型 */
	struct bus_type	*bus;		/* type of bus device is on */
	/* 分配了该设备的驱动程序 */
	struct device_driver *driver;	/* which driver has allocated this
					   device */
	/* 平台特定数据，设备核心不会修改它 */
	void		*platform_data;	/* Platform specific data, device
					   core doesn't touch it */
	// 设备的电源管理信息
	struct dev_pm_info	power;

#ifdef CONFIG_NUMA
	/* 该设备靠近的NUMA节点 */
	int		numa_node;	/* NUMA node this device is close to */
#endif
	/* 设备的DMA掩码（如果是可DMA设备） */
	u64		*dma_mask;	/* dma mask (if dma'able device) */
	/* 
	 * 和dma_mask类似，但用于一致性内存分配，
	 * 因为并非所有硬件都支持64位地址的一致性分配
	 */
	u64		coherent_dma_mask;/* Like dma_mask, but for
					     alloc_coherent mappings as
					     not all hardware supports
					     64 bit addresses for consistent
					     allocations such descriptors. */

	// 设备的DMA参数
	struct device_dma_parameters *dma_parms;

	/* 如果是可DMA设备的DMA池 */
	struct list_head	dma_pools;	/* dma pools (if dma'ble) */

	/* 内部用于一致性内存重写 */
	struct dma_coherent_mem	*dma_mem; /* internal for coherent mem
					     override */
	/* arch specific additions */
	/* 架构特定的添加 */
	struct dev_archdata	archdata;

	/* 设备的dev_t，创建sysfs中的"dev" */
	dev_t			devt;	/* dev_t, creates the sysfs "dev" */

	// 设备资源锁
	spinlock_t		devres_lock;
	// 设备资源列表
	struct list_head	devres_head;

	// 设备所属的类的节点
	struct klist_node	knode_class;
	// 设备所属的类
	struct class		*class;
	/* 可选的属性组 */
	const struct attribute_group **groups;	/* optional groups */

	// 释放设备时调用的函数
	void	(*release)(struct device *dev);
};

/* Get the wakeup routines, which depend on struct device */
#include <linux/pm_wakeup.h>

static inline const char *dev_name(const struct device *dev)
{
	return kobject_name(&dev->kobj);
}

extern int dev_set_name(struct device *dev, const char *name, ...)
			__attribute__((format(printf, 2, 3)));

#ifdef CONFIG_NUMA
static inline int dev_to_node(struct device *dev)
{
	return dev->numa_node;
}
static inline void set_dev_node(struct device *dev, int node)
{
	dev->numa_node = node;
}
#else
static inline int dev_to_node(struct device *dev)
{
	return -1;
}
static inline void set_dev_node(struct device *dev, int node)
{
}
#endif

static inline unsigned int dev_get_uevent_suppress(const struct device *dev)
{
	return dev->kobj.uevent_suppress;
}

static inline void dev_set_uevent_suppress(struct device *dev, int val)
{
	dev->kobj.uevent_suppress = val;
}

static inline int device_is_registered(struct device *dev)
{
	return dev->kobj.state_in_sysfs;
}

static inline void device_enable_async_suspend(struct device *dev)
{
	if (dev->power.status == DPM_ON)
		dev->power.async_suspend = true;
}

static inline void device_disable_async_suspend(struct device *dev)
{
	if (dev->power.status == DPM_ON)
		dev->power.async_suspend = false;
}

static inline bool device_async_suspend_enabled(struct device *dev)
{
	return !!dev->power.async_suspend;
}

static inline void device_lock(struct device *dev)
{
	down(&dev->sem);
}

static inline int device_trylock(struct device *dev)
{
	return down_trylock(&dev->sem);
}

static inline void device_unlock(struct device *dev)
{
	up(&dev->sem);
}

void driver_init(void);

/*
 * High level routines for use by the bus drivers
 */
extern int __must_check device_register(struct device *dev);
extern void device_unregister(struct device *dev);
extern void device_initialize(struct device *dev);
extern int __must_check device_add(struct device *dev);
extern void device_del(struct device *dev);
extern int device_for_each_child(struct device *dev, void *data,
		     int (*fn)(struct device *dev, void *data));
extern struct device *device_find_child(struct device *dev, void *data,
				int (*match)(struct device *dev, void *data));
extern int device_rename(struct device *dev, char *new_name);
extern int device_move(struct device *dev, struct device *new_parent,
		       enum dpm_order dpm_order);
extern const char *device_get_devnode(struct device *dev,
				      mode_t *mode, const char **tmp);
extern void *dev_get_drvdata(const struct device *dev);
extern void dev_set_drvdata(struct device *dev, void *data);

/*
 * Root device objects for grouping under /sys/devices
 */
extern struct device *__root_device_register(const char *name,
					     struct module *owner);
static inline struct device *root_device_register(const char *name)
{
	return __root_device_register(name, THIS_MODULE);
}
extern void root_device_unregister(struct device *root);

static inline void *dev_get_platdata(const struct device *dev)
{
	return dev->platform_data;
}

/*
 * Manual binding of a device to driver. See drivers/base/bus.c
 * for information on use.
 */
extern int __must_check device_bind_driver(struct device *dev);
extern void device_release_driver(struct device *dev);
extern int  __must_check device_attach(struct device *dev);
extern int __must_check driver_attach(struct device_driver *drv);
extern int __must_check device_reprobe(struct device *dev);

/*
 * Easy functions for dynamically creating devices on the fly
 */
extern struct device *device_create_vargs(struct class *cls,
					  struct device *parent,
					  dev_t devt,
					  void *drvdata,
					  const char *fmt,
					  va_list vargs);
extern struct device *device_create(struct class *cls, struct device *parent,
				    dev_t devt, void *drvdata,
				    const char *fmt, ...)
				    __attribute__((format(printf, 5, 6)));
extern void device_destroy(struct class *cls, dev_t devt);

/*
 * Platform "fixup" functions - allow the platform to have their say
 * about devices and actions that the general device layer doesn't
 * know about.
 */
/* Notify platform of device discovery */
extern int (*platform_notify)(struct device *dev);

extern int (*platform_notify_remove)(struct device *dev);


/**
 * get_device - atomically increment the reference count for the device.
 *
 */
extern struct device *get_device(struct device *dev);
extern void put_device(struct device *dev);

extern void wait_for_device_probe(void);

#ifdef CONFIG_DEVTMPFS
extern int devtmpfs_create_node(struct device *dev);
extern int devtmpfs_delete_node(struct device *dev);
extern int devtmpfs_mount(const char *mntdir);
#else
static inline int devtmpfs_create_node(struct device *dev) { return 0; }
static inline int devtmpfs_delete_node(struct device *dev) { return 0; }
static inline int devtmpfs_mount(const char *mountpoint) { return 0; }
#endif

/* drivers/base/power/shutdown.c */
extern void device_shutdown(void);

/* drivers/base/sys.c */
extern void sysdev_shutdown(void);

/* debugging and troubleshooting/diagnostic helpers. */
extern const char *dev_driver_string(const struct device *dev);
#define dev_printk(level, dev, format, arg...)	\
	printk(level "%s %s: " format , dev_driver_string(dev) , \
	       dev_name(dev) , ## arg)

#define dev_emerg(dev, format, arg...)		\
	dev_printk(KERN_EMERG , dev , format , ## arg)
#define dev_alert(dev, format, arg...)		\
	dev_printk(KERN_ALERT , dev , format , ## arg)
#define dev_crit(dev, format, arg...)		\
	dev_printk(KERN_CRIT , dev , format , ## arg)
#define dev_err(dev, format, arg...)		\
	dev_printk(KERN_ERR , dev , format , ## arg)
#define dev_warn(dev, format, arg...)		\
	dev_printk(KERN_WARNING , dev , format , ## arg)
#define dev_notice(dev, format, arg...)		\
	dev_printk(KERN_NOTICE , dev , format , ## arg)
#define dev_info(dev, format, arg...)		\
	dev_printk(KERN_INFO , dev , format , ## arg)

#if defined(DEBUG)
#define dev_dbg(dev, format, arg...)		\
	dev_printk(KERN_DEBUG , dev , format , ## arg)
#elif defined(CONFIG_DYNAMIC_DEBUG)
#define dev_dbg(dev, format, ...) do { \
	dynamic_dev_dbg(dev, format, ##__VA_ARGS__); \
	} while (0)
#else
#define dev_dbg(dev, format, arg...)		\
	({ if (0) dev_printk(KERN_DEBUG, dev, format, ##arg); 0; })
#endif

#ifdef VERBOSE_DEBUG
#define dev_vdbg	dev_dbg
#else

#define dev_vdbg(dev, format, arg...)		\
	({ if (0) dev_printk(KERN_DEBUG, dev, format, ##arg); 0; })
#endif

/*
 * dev_WARN() acts like dev_printk(), but with the key difference
 * of using a WARN/WARN_ON to get the message out, including the
 * file/line information and a backtrace.
 */
#define dev_WARN(dev, format, arg...) \
	WARN(1, "Device: %s\n" format, dev_driver_string(dev), ## arg);

/* Create alias, so I can be autoloaded. */
#define MODULE_ALIAS_CHARDEV(major,minor) \
	MODULE_ALIAS("char-major-" __stringify(major) "-" __stringify(minor))
#define MODULE_ALIAS_CHARDEV_MAJOR(major) \
	MODULE_ALIAS("char-major-" __stringify(major) "-*")
#endif /* _DEVICE_H_ */
