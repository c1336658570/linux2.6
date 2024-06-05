/*
 * kobject.h - generic kernel object infrastructure.
 *
 * Copyright (c) 2002-2003 Patrick Mochel
 * Copyright (c) 2002-2003 Open Source Development Labs
 * Copyright (c) 2006-2008 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (c) 2006-2008 Novell Inc.
 *
 * This file is released under the GPLv2.
 *
 * Please read Documentation/kobject.txt before using the kobject
 * interface, ESPECIALLY the parts about reference counts and object
 * destructors.
 */

#ifndef _KOBJECT_H_
#define _KOBJECT_H_

#include <linux/types.h>
#include <linux/list.h>
#include <linux/sysfs.h>
#include <linux/compiler.h>
#include <linux/spinlock.h>
#include <linux/kref.h>
#include <linux/kernel.h>
#include <linux/wait.h>
#include <asm/atomic.h>

// 用户空间助手路径的最大长度
#define UEVENT_HELPER_PATH_LEN		256
// 环境变量指针的数量
#define UEVENT_NUM_ENVP			32	/* number of env pointers */
// 存储变量的缓冲区大小
#define UEVENT_BUFFER_SIZE		2048	/* buffer for the variables */

/* path to the userspace helper executed on an event */
/* 用户空间助手执行事件的路径 */
extern char uevent_helper[];

/* counter to tag the uevent, read only except for the kobject core */
/* 用于标记 uevent 的计数器，除了 kobject 核心以外均为只读 */
extern u64 uevent_seqnum;

/*
 * The actions here must match the index to the string array
 * in lib/kobject_uevent.c
 *
 * Do not add new actions here without checking with the driver-core
 * maintainers. Action strings are not meant to express subsystem
 * or device specific properties. In most cases you want to send a
 * kobject_uevent_env(kobj, KOBJ_CHANGE, env) with additional event
 * specific variables added to the event environment.
 */
/*
 * 这里的操作必须与 lib/kobject_uevent.c 中字符串数组的索引匹配
 *
 * 不要在未与驱动核心维护者确认的情况下添加新的操作。操作字符串并不意味着表达子系统
 * 或设备特定的属性。在大多数情况下，您希望使用 kobject_uevent_env(kobj, KOBJ_CHANGE, env)
 * 并向事件环境中添加额外的事件特定变量。
 */
// 这些动作在发送到用户态时是通过字符串来表达的，其对应关系为：
/*
static const char *kobject_actions[] = {
    [KOBJ_ADD] =        "add",
    [KOBJ_REMOVE] =     "remove",
    [KOBJ_CHANGE] =     "change",
    [KOBJ_MOVE] =       "move",
    [KOBJ_ONLINE] =     "online",
    [KOBJ_OFFLINE] =    "offline",
};
*/
enum kobject_action {
	KOBJ_ADD,        // 添加对象
	KOBJ_REMOVE,     // 移除对象
	KOBJ_CHANGE,     // 改变对象
	KOBJ_MOVE,       // 移动对象
	KOBJ_ONLINE,     // 对象上线
	KOBJ_OFFLINE,    // 对象下线
	KOBJ_MAX         // 枚举的最大值，用于界定枚举的范围
};

// kset是一个基本的容器类，它是一组kobject的集合。当我们想统一管理某些有类似
// 属性的kobjects时，可以将它们加入到一个集合中，这个集合的作用是，
// 当一个事件发生时，可以同时通知到集合中的所有kobjects。

// kobject是通用对象的表示。 Linux内核将所有的kobjects与虚拟文件系统sysfs
// 紧密结合起来，这样就让所有kobjects可视化和层次化。kobject描述了sysfs
// 虚拟文件系统中的层级结构，一个kobject对象就对应了sysfs中的一个目录，
// 而sysfs中的目录结构也体现在各个kobjects之间的父子关系。
struct kobject {
	const char		*name;		/* kobject对象的名字，对应sysfs中的目录名 */
	struct list_head	entry;		/* 在kset中的链表节点 */
	/*
	 * parent用来指明kobj的父节点，即指定了kobj的目录在sysfs中创建的位置。
	 * 如果这个kobj要加入到一个特定的kset中，则在kobject_add()必须给kobj
	 * ->kset赋值，此时parent可以设置为NULL，这样kobj会自动将kobj->kset
	 * 对应的对象作为自己的parent。如果parent设置为NULL，且没有加入到一个
	 * kset中，kobject会被创建到/sys顶层目录下。
	 */
	struct kobject		*parent;	/* 用于构建sysfs中kobjects的层次结构，指向父目录 */
	struct kset		*kset;				/* 所属kset */
	struct kobj_type	*ktype;		/* 特定对象类型相关，用于跟踪object及其属性 */
	// sd指针指向sysfs_dirent结构体，该结构体在sysfs中表示的就是这个kobject。
	// 从sysfs文件系统内部看，该结构体是表示kobject结构体的一个inode结构体。
	// 指向 sysfs 目录项的指针
	struct sysfs_dirent	*sd;
	struct kref		kref;					/* kobject的引用计数，初始值为1 */
	/* kobject是否初始化，由kobject_init()设置 */
	unsigned int state_initialized:1;
	/* 是否已添加到sysfs层次结构中，在kobject_add_internal()中置位。 */
	unsigned int state_in_sysfs:1;
	/** 
	 * 记录是否已经向用户空间发送ADD uevent，如果有，且没有发送
	 * remove uevent，则在自动注销时，补发REMOVE uevent，
	 * 以便让用户空间正确处理。
	 */
	// 当发送KOBJ_ADD消息时，置位。提示已经向用户空间发送ADD消息。
	unsigned int state_add_uevent_sent:1;
	// 当发送KOBJ_REMOVE消息时，置位。提示已经向用户空间发送REMOVE消息。
	unsigned int state_remove_uevent_sent:1;
	/* 是否忽略uevent事件，如果该字段为1，则表示忽略所有上报的uevent事件。 */
	unsigned int uevent_suppress:1;
};

// kobj的name成员赋值。注意这里面是通过kmalloc给name分配内存的。
extern int kobject_set_name(struct kobject *kobj, const char *name, ...)
			    __attribute__((format(printf, 2, 3)));
extern int kobject_set_name_vargs(struct kobject *kobj, const char *fmt,
				  va_list vargs);

// 获取一个kobject对象的名字
static inline const char *kobject_name(const struct kobject *kobj)
{
	return kobj->name;
}

/*
 * 初始化一个kobject对象
 * 这个函数需要传入两个参数，kobj和ktype必须不为NULL，由于kobj都是嵌入到其他结构体，所以一般传kobj参数的方式形如&pcdev->kobj。
 * 该函数即完成对kobj的初始化：
 * 初始化kobj->kref引用计数为初始值1；
 * 初始化kobj->entry空链表头；
 * kobj->ktype = ktype;
 * 然后将kobj->state_initialized置为1，表示该kobject已初始化。
 */
extern void kobject_init(struct kobject *kobj, struct kobj_type *ktype);
/**
 * kobj从一个kset中加入和移除的操作包含在kobject_add()和kobject_del()中，
 * 因此在创建一个kobject对象时，如果想让其加入某个kset，
 * 就要在kobject_add()之前指定。
*/

/**
 * 通过kobject_add()将kobj添加到系统中
 * 这个函数给kobj指定一个名字，这个名字也就是其在sysfs中的目录名，
 * parent用来指明kobj的父节点，即指定了kobj的目录在sysfs中创建的位置。
 * 如果这个kobj要加入到一个特定的kset中，则在kobject_add()必须给
 * kobj->kset赋值，此时parent可以设置为NULL，这样kobj会自动将
 * kobj->kset对应的对象作为自己的parent。如果parent设置为NULL，
 * 且没有加入到一个kset中，kobject会被创建到/sys顶层目录下。
 */
// fmt为该kobject的名称
extern int __must_check kobject_add(struct kobject *kobj,
				    struct kobject *parent,
				    const char *fmt, ...);
// 一次性完成kobject_init()和kobject_add()过程
extern int __must_check kobject_init_and_add(struct kobject *kobj,
					     struct kobj_type *ktype,
					     struct kobject *parent,
					     const char *fmt, ...);

/**
 * 如果你需要分两次对kobject进行删除（比如说在你要销毁对象时无权睡眠），
 * 那么调用kobject_del()将从sysfs中取消kobject的注册。
 * 这使得kobject “不可见”，但它并没有被清理掉，而且该对象的引用计数
 * 仍然是一样的。在稍后的时间调用kobject_put()来完成与该kobject
 * 相关的内存的清理。
 * 
 * kobject_del()可以用来放弃对父对象的引用，如果循环引用被构建的话。
 * 在某些情况下，一个父对象引用一个子对象是有效的。
 * 循环引用必须通过明确调用kobject_del()来打断，
 * 这样一个释放函数就会被调用，前一个循环中的对象会相互释放。
 */
extern void kobject_del(struct kobject *kobj);

// 创建一个kobject对象并返回
extern struct kobject * __must_check kobject_create(void);
// 一次性完成kobject_create，kobject_init和kobject_add
/**
 * @name: kobject名称
 * @parent: the parent kobject of this kobject, if any.
 * 
 * 返回：一个kobject类型的数据结构
 * 不用这个kobject的时候用kobject_put()释放掉这个kobject
 * 未创建成功返回NULL
 */
extern struct kobject * __must_check kobject_create_and_add(const char *name,
						struct kobject *parent);

// 在添加kobj之后要给kobj改名字，则使用kobject_rename()接口
extern int __must_check kobject_rename(struct kobject *, const char *new_name);
extern int __must_check kobject_move(struct kobject *, struct kobject *);

// kref成员是object对象的引用计数，初始值为1，通过kref_get()和
// kref_put()可对该计数进行增减操作。kref_get()和kref_put()
// 是内核中通用的引用计数的操作，针对kobject，使用下面两个封装函数
// kobject_get 和 kobject_put
extern struct kobject *kobject_get(struct kobject *kobj);
extern void kobject_put(struct kobject *kobj);

extern char *kobject_get_path(struct kobject *kobj, gfp_t flag);

/* kobj_type 定义了 kobject 的类型特定信息 */
// 特定对象类型相关，用于跟踪object及其属性
// kobj_type是由具体模块定义的，每一个属性都对应着kobject目录下的一个文件，
// 这样可以在用户态通过读写属性文件，来完成对该属性值的读取和更改。
struct kobj_type {
	// kobj_type都必须实现release方法，用来释放特定模块相关的kobject资源。
	// 当kobject的引用计数为0时调用的释放函数
	void (*release)(struct kobject *kobj);
	// 定义了属性的操作，如何显示和修改sysfs文件，可以对属性进行读写
	// 指向 sysfs 操作函数的指针，这些函数定义了如何在 sysfs 中显示和存储 kobject 的属性
	const struct sysfs_ops *sysfs_ops;
	// 定义了一系列默认属性，描述了属于该ktype的kobject的默认属性，用于sysfs表示
	// 指向属性数组的指针，这些属性在创建 kobject 时自动创建
	struct attribute **default_attrs;
};

/* kobj_uevent_env 用于存储生成环境变量和数据缓冲区的信息，这些信息用于发送 uevent */
struct kobj_uevent_env {
	char *envp[UEVENT_NUM_ENVP];   // 环境变量指针数组
	int envp_idx;                  // 环境变量数组的当前索引
	char buf[UEVENT_BUFFER_SIZE];  // 存储额外数据的缓冲区
	int buflen;                    // 缓冲区中数据的长度
};

/**
 * kset的作用还在于可以对kobject的uevent事件的默认动作做一些扩展，
 * 在kobject_uevent和kobject_uevent_env()函数中，
 * 如果发现kobj属于某个kset，则还会继续调用其kset的uevent_ops，
 * 这个结构体中，filter方法是一个过滤规则，用于判断是否将uevent发出去，
 * 可使用户态忽略某些事件。name方法用于获得特定子系统的名字传递给用户态，
 * 可以用来覆盖默认的名字（默认为kset的名字）。uevent方法即用于完成扩展
 * 的事件通知动作，例如对于”设备”的kset，事件中除了携带通用的环境变量，
 * 还需要携带MAJOR、MIMOR、DEVNAME等变量发往用户态。
 */
/* kset_uevent_ops 定义了与 kset 相关的 uevent 操作 */
struct kset_uevent_ops {
	// 过滤器函数允许kset阻止一个特定kobject的uevent被发送到用户空间。
	// 如果该函数返回0，该uevent将不会被发射出去。
	int (* const filter)(struct kset *kset, struct kobject *kobj);
	// name函数将被调用以覆盖uevent发送到用户空间的kset的默认名称。
	// 默认情况下，该名称将与kset本身相同，但这个函数，
	// 如果存在，可以覆盖该名称。
	const char *(* const name)(struct kset *kset, struct kobject *kobj);
	// 当uevent即将被发送至用户空间时，uevent函数将被调用，
	// 以允许更多的环境变量被添加到uevent中。
	int (* const uevent)(struct kset *kset, struct kobject *kobj,
		      struct kobj_uevent_env *env);
};

/* kobj_attribute 用于定义 kobject 的属性，包括如何读取和写入这些属性 */
// kobj_attribute描述一个kobject的属性，相当于继承了attribute
struct kobj_attribute {
	struct attribute attr;     // 基本属性结构
	// show函数指针用用户读该属性文件的回调操作，需要填充buf缓冲区，将内容传递给用户
	// 用于读取属性值，结果存储在 buf 中
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf);
	// store则对应于用户写该属性文件的回调操作，需要读取该buf缓冲区，了解用户进行何种要求
	// 用于写入属性值，数据来自 buf
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count);
};

/* sysfs 操作的默认实现，通常在 kobj_type 结构中引用 */
extern const struct sysfs_ops kobj_sysfs_ops;

/**
 * struct kset - a set of kobjects of a specific type, belonging to a specific subsystem.
 *
 * A kset defines a group of kobjects.  They can be individually
 * different "types" but overall these kobjects all want to be grouped
 * together and operated on in the same manner.  ksets are used to
 * define the attribute callbacks and other common events that happen to
 * a kobject.
 *
 * @list: the list of all kobjects for this kset
 * @list_lock: a lock for iterating over the kobjects
 * @kobj: the embedded kobject for this kset (recursion, isn't it fun...)
 * @uevent_ops: the set of uevent operations for this kset.  These are
 * called whenever a kobject has something happen to it so that the kset
 * can add new environment variables, or filter out the uevents if so
 * desired.
 */
/**
 * struct kset - 一组特定类型的 kobjects，属于特定的子系统。
 *
 * kset 定义了一组 kobjects 的集合。这些 kobjects 可能是不同的“类型”，
 * 但整体上，这些 kobjects 都希望被分组在一起，并以相同的方式操作。
 * ksets 用于定义属性回调和其他常见的事件，这些事件发生在 kobject 上。
 *
 * @list: 此 kset 的所有 kobjects 的列表
 * @list_lock: 迭代 kobjects 时使用的锁
 * @kobj: 此 kset 的嵌入式 kobject（递归，有趣吧...）
 * @uevent_ops: 此 kset 的 uevent 操作集。每当 kobject 发生某些事件时调用，
 *              以便 kset 可以添加新的环境变量，或根据需要过滤掉 uevents。
 */
/**
 * 使用kset可以统一管理某些kobjects，方便查找和遍历，kobject的entry成员
 * 将所有的同一集合中的成员连接起来。
 * 
 * 另外我们看到，一个kset自身在内核中也是一个kobject对象，因此，一个kset
 * 在sysfs中也对应着一个目录，这个kset的kobject可以作为其子目录的parent，
 * sysfs顶层目录的bus/、devices/等目录就是这样创建的。通常，一个目录下的
 * 所有子目录都是属于同一个kset的，例如/sys/bus/目录下的所有子目录都属于
 * 全局的bus_kset。
 * 
 * kset的作用还在于可以对kobject的uevent事件的默认动作做一些扩展，
 * 在kobject_uevent和kobject_uevent_env()函数中，
 * 如果发现kobj属于某个kset，则还会继续调用其kset的uevent_ops，
 * 这个结构体中，filter方法是一个过滤规则，用于判断是否将uevent发出去，
 * 可使用户态忽略某些事件。name方法用于获得特定子系统的名字传递给用户态，
 * 可以用来覆盖默认的名字（默认为kset的名字）。uevent方法即用于完成扩展
 * 的事件通知动作，例如对于”设备”的kset，事件中除了携带通用的环境变量，
 * 还需要携带MAJOR、MIMOR、DEVNAME等变量发往用户态。
 */
struct kset {
	struct list_head list;	/* 其成员列表 */
	spinlock_t list_lock;		// 保护 kobjects 链表的自旋锁
	struct kobject kobj;		// 该kset的基类（就是说该kset中的kobject都长这样）
	// 指针指向kset_uevent_ops结构体，用于处理kset中的kobject对象的热插拔操作。
	// uevent就是用户事件的缩写，提供了与用户空间热插拔信息进行通信的机制。
	/**
	 * 该kset的uevent操作函数集。当任何Kobject需要上报uevent时，
	 * 都要调用它所从属的kset的uevent_ops，添加环境变量，
	 * 或者过滤event（kset可以决定哪些event可以上报）。
	 * 因此，如果一个kobject不属于任何kset时，是不允许发送uevent的。
	 */
	const struct kset_uevent_ops *uevent_ops;	/* 扩展的事件处理 */
};

extern void kset_init(struct kset *kset);	// 初始化 kset
// 注册 kset
extern int __must_check kset_register(struct kset *kset);
// 注销 kset
extern void kset_unregister(struct kset *kset);
// 创建并添加 kset
extern struct kset * __must_check kset_create_and_add(const char *name,
						const struct kset_uevent_ops *u,
						struct kobject *parent_kobj);

// 从 kobject 获取 kset
static inline struct kset *to_kset(struct kobject *kobj)
{
	return kobj ? container_of(kobj, struct kset, kobj) : NULL;
}

// 获取 kset 的引用
static inline struct kset *kset_get(struct kset *k)
{
	return k ? to_kset(kobject_get(&k->kobj)) : NULL;
}

// 释放 kset 的引用
static inline void kset_put(struct kset *k)
{
	kobject_put(&k->kobj);
}

// 获取 kobject 的类型
static inline struct kobj_type *get_ktype(struct kobject *kobj)
{
	return kobj->ktype;
}

// 在 kset 中查找 kobject
extern struct kobject *kset_find_obj(struct kset *, const char *);

/* The global /sys/kernel/ kobject for people to chain off of */
/* 全局 /sys/kernel/ kobject，供其他人基于此链接 */
extern struct kobject *kernel_kobj;
/* The global /sys/kernel/mm/ kobject for people to chain off of */
/* 全局 /sys/kernel/mm/ kobject，供其他人基于此链接 */
extern struct kobject *mm_kobj;
/* The global /sys/hypervisor/ kobject for people to chain off of */
/* 全局 /sys/hypervisor/ kobject，供其他人基于此链接 */
extern struct kobject *hypervisor_kobj;
/* The global /sys/power/ kobject for people to chain off of */
/* 全局 /sys/power/ kobject，供其他人基于此链接 */
extern struct kobject *power_kobj;
/* The global /sys/firmware/ kobject for people to chain off of */
/* 全局 /sys/firmware/ kobject，供其他人基于此链接 */
extern struct kobject *firmware_kobj;

#if defined(CONFIG_HOTPLUG)
// 发送 kobject uevent
int kobject_uevent(struct kobject *kobj, enum kobject_action action);
// 发送带环境变量的 kobject uevent
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action,
			char *envp[]);

// 添加 uevent 环境变量
int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...)
	__attribute__((format (printf, 2, 3)));

// 解析 kobject 操作类型
int kobject_action_type(const char *buf, size_t count,
			enum kobject_action *type);
#else
// 发送 kobject uevent
static inline int kobject_uevent(struct kobject *kobj,
				 enum kobject_action action)
{ return 0; }
// 发送带环境变量的 kobject uevent
static inline int kobject_uevent_env(struct kobject *kobj,
				      enum kobject_action action,
				      char *envp[])
{ return 0; }

// 添加 uevent 环境变量
static inline int add_uevent_var(struct kobj_uevent_env *env,
				 const char *format, ...)
{ return 0; }

// 解析 kobject 操作类型
static inline int kobject_action_type(const char *buf, size_t count,
				      enum kobject_action *type)
{ return -EINVAL; }
#endif

#endif /* _KOBJECT_H_ */
