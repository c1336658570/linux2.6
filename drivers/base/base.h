
/**
 * struct bus_type_private - structure to hold the private to the driver core portions of the bus_type structure.
 *
 * @subsys - the struct kset that defines this bus.  This is the main kobject
 * @drivers_kset - the list of drivers associated with this bus
 * @devices_kset - the list of devices associated with this bus
 * @klist_devices - the klist to iterate over the @devices_kset
 * @klist_drivers - the klist to iterate over the @drivers_kset
 * @bus_notifier - the bus notifier list for anything that cares about things
 * on this bus.
 * @bus - pointer back to the struct bus_type that this structure is associated
 * with.
 *
 * This structure is the one that is the actual kobject allowing struct
 * bus_type to be statically allocated safely.  Nothing outside of the driver
 * core should ever touch these fields.
 */
/**
 * struct bus_type_private - 用于存放与驱动核心相关的总线类型结构的私有部分。
 *
 * @subsys - 定义此总线的 struct kset。这是主要的 kobject。
 * @drivers_kset - 与此总线相关联的驱动程序列表。
 * @devices_kset - 与此总线相关联的设备列表。
 * @klist_devices - 用于遍历 @devices_kset 的 klist。
 * @klist_drivers - 用于遍历 @drivers_kset 的 klist。
 * @bus_notifier - 用于通知关心此总线上发生事情的任何方的通知器列表。
 * @bus - 指向与此结构相关联的 struct bus_type 的指针。
 *
 * 这个结构是实际的 kobject，允许 struct bus_type 能够安全地被静态分配。
 * 驱动核心之外的任何部分都不应触及这些字段。
 */
struct bus_type_private {
	struct kset subsys;                  // 此总线的主 kset
	struct kset *drivers_kset;           // 与此总线关联的驱动程序的 kset
	struct kset *devices_kset;           // 与此总线关联的设备的 kset
	struct klist klist_devices;          // 遍历设备的 klist
	struct klist klist_drivers;          // 遍历驱动程序的 klist
	struct blocking_notifier_head bus_notifier;  // 总线事件的通知器头
	unsigned int drivers_autoprobe:1;    // 自动探测驱动程序标志位
	struct bus_type *bus;                // 指向关联的总线类型
};

/**
 * struct driver_private - 用于存放与设备驱动程序相关的私有数据。
 *
 * @kobj - 设备驱动程序的 kobject。
 * @klist_devices - 与此驱动关联的设备的 klist。
 * @knode_bus - 驱动在总线上的节点。
 * @mkobj - 模块的 kobject。
 * @driver - 指向设备驱动结构的指针。
 */
struct driver_private {
	struct kobject kobj;                // 驱动程序的 kobject
	struct klist klist_devices;         // 遍历与驱动关联的设备的 klist
	struct klist_node knode_bus;        // 驱动在总线上的节点
	struct module_kobject *mkobj;       // 驱动所属模块的 kobject
	struct device_driver *driver;       // 指向关联的设备驱动结构
};

// 宏定义：从 kobject 获取 struct driver_private
#define to_driver(obj) container_of(obj, struct driver_private, kobj)

/**
 * struct class_private - structure to hold the private to the driver core portions of the class structure.
 *
 * @class_subsys - the struct kset that defines this class.  This is the main kobject
 * @class_devices - list of devices associated with this class
 * @class_interfaces - list of class_interfaces associated with this class
 * @class_dirs - "glue" directory for virtual devices associated with this class
 * @class_mutex - mutex to protect the children, devices, and interfaces lists.
 * @class - pointer back to the struct class that this structure is associated
 * with.
 *
 * This structure is the one that is the actual kobject allowing struct
 * class to be statically allocated safely.  Nothing outside of the driver
 * core should ever touch these fields.
 */
/**
 * struct class_private - 用于存放设备类结构中与驱动核心相关的私有部分的结构体。
 *
 * @class_subsys - 定义此类的 struct kset。这是主要的 kobject。
 * @class_devices - 与此类相关联的设备列表。
 * @class_interfaces - 与此类相关联的类接口列表。
 * @class_dirs - 用于此类相关的虚拟设备的“胶合”目录。
 * @class_mutex - 用于保护子设备、设备和接口列表的互斥锁。
 * @class - 指向与此结构体相关联的 struct class 的指针。
 *
 * 这个结构体是实际的 kobject，允许 struct class 能够安全地被静态分配。
 * 驱动核心之外的任何部分都不应该触及这些字段。
 */
struct class_private {
	struct kset class_subsys;          // 定义这个类的 kset，作为主 kobject
	struct klist class_devices;        // 与这个类相关联的设备的 klist
	struct list_head class_interfaces; // 与这个类相关联的接口的列表
	struct kset class_dirs;            // 与这个类相关的虚拟设备的目录 kset
	struct mutex class_mutex;          // 用于保护设备和接口列表的互斥锁
	struct class *class;               // 指回关联的 struct class
};

// 从 kobject 获取 struct class_private 的宏定义
#define to_class(obj)	\
	container_of(obj, struct class_private, class_subsys.kobj)

/**
 * struct device_private - structure to hold the private to the driver core portions of the device structure.
 *
 * @klist_children - klist containing all children of this device
 * @knode_parent - node in sibling list
 * @knode_driver - node in driver list
 * @knode_bus - node in bus list
 * @driver_data - private pointer for driver specific info.  Will turn into a
 * list soon.
 * @device - pointer back to the struct class that this structure is
 * associated with.
 *
 * Nothing outside of the driver core should ever touch these fields.
 */
/**
 * struct device_private - 用于存放设备结构中与驱动核心相关的私有部分的结构体。
 *
 * @klist_children - 包含此设备所有子设备的 klist。
 * @knode_parent - 兄弟列表中的节点。
 * @knode_driver - 驱动程序列表中的节点。
 * @knode_bus - 总线列表中的节点。
 * @driver_data - 为驱动程序特定信息保留的私有指针。很快会变成一个列表。
 * @device - 指回与此结构体相关联的 struct device 的指针。
 *
 * 驱动核心以外的任何部分都不应该触及这些字段。
 */
struct device_private {
	struct klist klist_children;      // 存放所有子设备的 klist
	struct klist_node knode_parent;   // 位于父设备兄弟列表中的节点
	struct klist_node knode_driver;   // 位于驱动列表中的节点
	struct klist_node knode_bus;      // 位于总线列表中的节点
	// 比如指向 struct backing_dev_info
	void *driver_data;                // 驱动特定数据，预留作为私有指针
	struct device *device;            // 指向关联的 struct device
};
// 宏定义，从父节点 klist_node 获取 struct device_private
#define to_device_private_parent(obj)	\
	container_of(obj, struct device_private, knode_parent)
// 宏定义，从驱动节点 klist_node 获取 struct device_private
#define to_device_private_driver(obj)	\
	container_of(obj, struct device_private, knode_driver)
// 宏定义，从总线节点 klist_node 获取 struct device_private
#define to_device_private_bus(obj)	\
	container_of(obj, struct device_private, knode_bus)

// 初始化给定设备的 device_private 结构
extern int device_private_init(struct device *dev);

/* initialisation functions */
/* 初始化函数 */
extern int devices_init(void); // 初始化设备子系统
extern int buses_init(void); // 初始化总线子系统
extern int classes_init(void); // 初始化类子系统
extern int firmware_init(void); // 初始化固件子系统
#ifdef CONFIG_SYS_HYPERVISOR
// 初始化虚拟机监控器子系统
extern int hypervisor_init(void);
#else
// 如果没有启用虚拟机监控器支持，则返回0
static inline int hypervisor_init(void) { return 0; }
#endif
extern int platform_bus_init(void); // 初始化平台总线
extern int system_bus_init(void); // 初始化系统总线
extern int cpu_dev_init(void); // 初始化CPU设备

extern int bus_add_device(struct device *dev); // 向总线添加设备
extern void bus_probe_device(struct device *dev); // 探测总线上的设备
extern void bus_remove_device(struct device *dev); // 从总线移除设备

extern int bus_add_driver(struct device_driver *drv); // 向总线添加驱动
extern void bus_remove_driver(struct device_driver *drv); // 从总线移除驱动

extern void driver_detach(struct device_driver *drv); // 分离设备和驱动
extern int driver_probe_device(struct device_driver *drv, struct device *dev); // 探测设备是否匹配驱动

static inline int driver_match_device(struct device_driver *drv,
				      struct device *dev)
{
	// 如果总线提供了匹配函数则调用，否则默认匹配成功
	return drv->bus->match ? drv->bus->match(dev, drv) : 1;
}

// 关闭系统设备
extern void sysdev_shutdown(void);

// 根据提供的名称和kobject生成类名
extern char *make_class_name(const char *name, struct kobject *kobj);

// 释放设备的所有资源
extern int devres_release_all(struct device *dev);

// 设备kset全局变量
extern struct kset *devices_kset;

#if defined(CONFIG_MODULES) && defined(CONFIG_SYSFS)
// 将模块和驱动关联
extern void module_add_driver(struct module *mod, struct device_driver *drv);
// 移除模块和驱动的关联
extern void module_remove_driver(struct device_driver *drv);
#else
static inline void module_add_driver(struct module *mod,
				     struct device_driver *drv) { }
static inline void module_remove_driver(struct device_driver *drv) { }
#endif

#ifdef CONFIG_DEVTMPFS
// 初始化devtmpfs
extern int devtmpfs_init(void);
#else
// 如果没有启用devtmpfs，则返回0
static inline int devtmpfs_init(void) { return 0; }
#endif
