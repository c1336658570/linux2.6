/*
 * class.c - basic device class management
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2003-2004 Greg Kroah-Hartman
 * Copyright (c) 2003-2004 IBM Corp.
 *
 * This file is released under the GPLv2
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/kdev_t.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/genhd.h>
#include <linux/mutex.h>
#include "base.h"

// 宏定义，从给定的 attribute 结构获取包含它的 class_attribute 结构
#define to_class_attr(_attr) container_of(_attr, struct class_attribute, attr)

// 显示类属性的函数
static ssize_t class_attr_show(struct kobject *kobj, struct attribute *attr,
			       char *buf)
{
	// 通过 attribute 指针获取对应的 class_attribute 结构
	struct class_attribute *class_attr = to_class_attr(attr);
	// 通过 kobject 获取其关联的 class_private 结构
	struct class_private *cp = to_class(kobj);
	ssize_t ret = -EIO;	// 默认返回错误（输入/输出错误）

	// 如果存在 show 方法，则调用它来填充 buf，并获取显示长度
	if (class_attr->show)
		ret = class_attr->show(cp->class, class_attr, buf);
	return ret;	// 返回操作结果
}

// 存储类属性的函数
static ssize_t class_attr_store(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t count)
{
	// 通过 attribute 指针获取对应的 class_attribute 结构
	struct class_attribute *class_attr = to_class_attr(attr);
	// 通过 kobject 获取其关联的 class_private 结构
	struct class_private *cp = to_class(kobj);
	ssize_t ret = -EIO;	// 默认返回错误（输入/输出错误）

	// 如果存在 store 方法，则调用它来处理 buf 中的数据
	if (class_attr->store)
		ret = class_attr->store(cp->class, class_attr, buf, count);
	return ret;	// 返回操作结果
}

// 类对象的释放函数。class_release()是在class引用计数降为零时调用的释放函数。
static void class_release(struct kobject *kobj)
{
	// 从 kobject 获取对应的 class_private 结构
	struct class_private *cp = to_class(kobj);
	// 从 class_private 结构获取对应的 class 结构
	struct class *class = cp->class;

	// 打印调试信息，表示类正在被释放
	pr_debug("class '%s': release.\n", class->name);

	// 如果类定义了自己的释放函数，调用它
	if (class->class_release)
		class->class_release(class);
	else
		// 如果类没有定义释放函数，打印调试警告
		pr_debug("class '%s' does not have a release() function, "
			 "be careful\n", class->name);

	// 释放 class_private 结构的内存
	kfree(cp);
}

// 定义 sysfs 操作方法
static const struct sysfs_ops class_sysfs_ops = {
	// 设置显示属性的函数
	.show	= class_attr_show,
	// 设置存储属性的函数
	.store	= class_attr_store,
};

static struct kobj_type class_ktype = {
	// 指向 sysfs 操作的指针
	.sysfs_ops	= &class_sysfs_ops,
	// 指向释放函数的指针。class_release()是在class引用计数降为零时调用的释放函数。
	.release	= class_release,
};

/* Hotplug events for classes go to the class class_subsys */
/* 用于类的热插拔事件的 kset 指向 class_subsys */
// class_kset代表了/sys/class对应的kset，在classes_init()中创建。
static struct kset *class_kset;

// class_create_file()创建class的属性文件。
/**
 * class_create_file - 在类的 sysfs 目录中创建一个文件
 * @cls: 指向要操作的类对象的指针
 * @attr: 要创建的属性
 *
 * 此函数用于在给定类的 sysfs 目录下创建一个文件，该文件对应于指定的属性。
 * 如果类指针为空，返回一个错误代码。
 * 
 * 返回: 成功时返回 0，失败时返回错误代码。
 */
int class_create_file(struct class *cls, const struct class_attribute *attr)
{
	int error;	// 用于存储错误代码
	if (cls)		// 如果提供了有效的类指针
		// 在 cls 对应的 kobject 下创建一个与 attr 关联的 sysfs 文件
		error = sysfs_create_file(&cls->p->class_subsys.kobj,
					  &attr->attr);
	else
		// 如果 cls 为空，则返回无效参数错误
		error = -EINVAL;
	return error;	// 返回操作结果
}

// class_remove_files()删除class的属性文件。
/**
 * class_remove_file - 从类的 sysfs 目录中移除一个文件
 * @cls: 指向要操作的类对象的指针
 * @attr: 要移除的属性
 *
 * 此函数用于从给定类的 sysfs 目录下移除与指定属性对应的文件。
 */
void class_remove_file(struct class *cls, const struct class_attribute *attr)
{
	if (cls)	// 如果提供了有效的类指针
		// 从 cls 对应的 kobject 下移除与 attr 关联的 sysfs 文件
		sysfs_remove_file(&cls->p->class_subsys.kobj, &attr->attr);
}

// class_get()增加对cls的引用计数
// class的引用计数是由class_private结构中的kset来管的，kset又是由其内部kobject来管的，
// kobject又是调用其结构中的kref来管的。这是一种嵌套的封装技术。
static struct class *class_get(struct class *cls)
{
	if (cls)
		kset_get(&cls->p->class_subsys);
	return cls;
}

// class_put()减少对cls的引用计数，并在计数降为零时调用相应的释放函数，也就是之前见过的class_release函数。
static void class_put(struct class *cls)
{
	if (cls)
		kset_put(&cls->p->class_subsys);
}

// add_class_attrs()把cls->class_attrs中的属性加入sysfs。
static int add_class_attrs(struct class *cls)
{
	int i;
	int error = 0;

	if (cls->class_attrs) {	// 如果类有属性定义
		for (i = 0; attr_name(cls->class_attrs[i]); i++) {	// 遍历所有属性
			error = class_create_file(cls, &cls->class_attrs[i]);	// 尝试创建属性文件
			if (error)	// 如果创建失败
				goto error;	// 跳转到错误处理代码
		}
	}
done:
	return error;	// 返回错误代码（如果有）
error:
	while (--i >= 0)
		// 移除已创建的属性文件
		class_remove_file(cls, &cls->class_attrs[i]);
	goto done;	// 跳转到完成标签，返回错误代码
}

// remove_class_attrs()把cls->class_attrs中的属性删除。
// 到了class这个级别，就和bus一样，除了自己，没有其它结构能为自己添加属性。
/*
 * 移除类的所有属性。
 * @param cls 指向类的指针。
 */
static void remove_class_attrs(struct class *cls)
{
	int i;

	if (cls->class_attrs) {	// 如果类有属性定义
		for (i = 0; attr_name(cls->class_attrs[i]); i++)	// 遍历所有属性
			// 移除属性文件
			class_remove_file(cls, &cls->class_attrs[i]);
	}
}

/* klist_class_dev_get函数：增加与klist节点关联的设备的引用计数 */
// 这是class的设备链表，在节点添加和删除时调用的。相似的klist链表，还有驱动的设备链表
// 还有总线的设备链表，在添加释放节点时分别调用klist_devices_get()和list_devices_put()，
// 是在bus.c中定义的。还有设备的子设备链表，在添加释放节点时分别调用klist_children_get()
// 和klist_children_put()，是在device.c中定义的。看来klist中的get()/put()函数，
// 是在初始化klist时设定的，也由创建方负责实现。
static void klist_class_dev_get(struct klist_node *n)
{
	/* 使用container_of宏从klist_node指针获取包含它的device结构的指针 */
	struct device *dev = container_of(n, struct device, knode_class);

	/* 增加设备的引用计数 */
	get_device(dev);
}

/* klist_class_dev_put函数：减少与klist节点关联的设备的引用计数 */
static void klist_class_dev_put(struct klist_node *n)
{
	/* 使用container_of宏从klist_node指针获取包含它的device结构的指针 */
	struct device *dev = container_of(n, struct device, knode_class);

	/* 减少设备的引用计数 */
	put_device(dev);
}

/** 就是填充class_private 私有数据结构体。然后注册到内核中
 * 先是分配和初始化class_private结构。
 * 
 * 可以看到对cp->class_dirs，只是调用kset_init()定义，并未实际注册到sysfs中。
 * 
 * 调用kobject_set_name()创建kobj中实际的类名。
 * 
 * cls->dev_kobj如果未设置，这里会被设为sysfs_dev_char_kobj。
 * 
 * 调用kset_register()将class注册到sysfs中，所属kset为class_kset，
 * 使用类型为class_ktype。因为没有设置parent，会以/sys/class为父目录。
 * 
 * 最后调用add_class_attrs()添加相关的属性文件。
 */
int __class_register(struct class *cls, struct lock_class_key *key)
{
	struct class_private *cp;
	int error;

	/* 输出调试信息，表示正在注册设备类 */
	pr_debug("device class '%s': registering\n", cls->name);

	cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;
	// 设备节点列表初始化，初始化klist的结构。如果klist_node结构将要被嵌入引用计数的对象
	// （所必需的安全的删除），则获得/释放参数用于初始化该采取的功能并释放嵌入对象的引用。
	klist_init(&cp->class_devices, klist_class_dev_get, klist_class_dev_put);
	// 初始化关联的子系统接口列表
	INIT_LIST_HEAD(&cp->class_interfaces);
	kset_init(&cp->class_dirs);
	__mutex_init(&cp->class_mutex, "struct class mutex", key);
	// 设置类的名字，例如video4linux ，sys/class/video4linux
	error = kobject_set_name(&cp->class_subsys.kobj, "%s", cls->name);
	if (error) {
		kfree(cp);
		return error;
	}

	/* set the default /sys/dev directory for devices of this class */
	// 设备默认目录sys/dev为类设备
	if (!cls->dev_kobj)
	 // 表示该class下的设备在/sys/dev/下的目录，现在一般有char和block两个，如果dev_kobj为NULL，则默认选择char。
		cls->dev_kobj = sysfs_dev_char_kobj;

#if defined(CONFIG_SYSFS_DEPRECATED) && defined(CONFIG_BLOCK)
	/* let the block class directory show up in the root of sysfs */
	/* 如果是块设备类，让类目录显示在sysfs的根目录 */
	if (cls != &block_class)
		cp->class_subsys.kobj.kset = class_kset;
#else
	// 设置 例如：video4linux的顶级容器，sys/class
	cp->class_subsys.kobj.kset = class_kset;
#endif
	//设置 例如：video4linux的类型
	cp->class_subsys.kobj.ktype = &class_ktype;
	// 将类class赋给私有数据结构体class_private
	cp->class = cls;
	cls->p = cp;	// 将私有数据结构体class_private 赋给类class的私有数据结构体class_private

	// 注册进入内核，创建目录 sys/class/video4linux
	error = kset_register(&cp->class_subsys);
	if (error) {
		kfree(cp);
		return error;	/* 如果注册失败，释放内存并返回错误 */
	}
	// 添加类属性，并增加模块引用计数 
	error = add_class_attrs(class_get(cls));
	// 减少模块引用计数
	class_put(cls);
	return error;
}
EXPORT_SYMBOL_GPL(__class_register);

/* 函数 class_unregister：注销一个设备类 */
void class_unregister(struct class *cls)
{	
	/* 输出调试信息，表示正在注销设备类 */
	pr_debug("device class '%s': unregistering\n", cls->name);
	/* 移除设备类属性 */
	remove_class_attrs(cls);
	/* 注销kset */
	kset_unregister(&cls->p->class_subsys);
}

/**
 * 释放函数，当类对象不再需要时被调用。
 * @cls: 指向要被释放的类对象的指针。
 */
// class_create_release 是当类不再需要时自动调用的释放函数，负责释放类对象的内存。
static void class_create_release(struct class *cls)
{
	// 打印调试信息，指出此函数被调用，并显示类的名称
	pr_debug("%s called for %s\n", __func__, cls->name);
	// 释放类对象占用的内存
	kfree(cls);
}

/**
 * class_create - create a struct class structure
 * @owner: pointer to the module that is to "own" this struct class
 * @name: pointer to a string for the name of this class.
 * @key: the lock_class_key for this class; used by mutex lock debugging
 *
 * This is used to create a struct class pointer that can then be used
 * in calls to device_create().
 *
 * Returns &struct class pointer on success, or ERR_PTR() on error.
 *
 * Note, the pointer created here is to be destroyed when finished by
 * making a call to class_destroy().
 */
/**
 * 创建一个设备类对象
 * @owner: 指向拥有这个类的模块的指针
 * @name: 类的名称
 * @key: 用于互斥锁调试的 lock_class_key
 *
 * 此函数用于创建一个 struct class 指针，之后可以在 device_create() 调用中使用。
 *
 * 成功时返回 struct class 指针，失败时返回 ERR_PTR()。
 *
 * 注意，这里创建的指针在使用完毕后应通过调用 class_destroy() 来销毁。
 */
struct class *__class_create(struct module *owner, const char *name,
			     struct lock_class_key *key)
{
	struct class *cls;
	int retval;

	// 为 class 结构分配内存
	cls = kzalloc(sizeof(*cls), GFP_KERNEL);
	if (!cls) {
		retval = -ENOMEM;
		goto error;
	}

	// 初始化 class 结构
	cls->name = name;	//填充类的名字，例如：video4linux gpio i2c等等。
	cls->owner = owner;	//填充类所属模块
	cls->class_release = class_create_release;	// 类的释放函数

	// 注册 class
	retval = __class_register(cls, key);	// 在内核中注册一个类
	if (retval)
		goto error;

	return cls;

error:
	// 如果出现错误，释放已分配的内存并返回错误指针
	kfree(cls);
	return ERR_PTR(retval);
}
EXPORT_SYMBOL_GPL(__class_create);

/**
 * class_destroy - destroys a struct class structure
 * @cls: pointer to the struct class that is to be destroyed
 *
 * Note, the pointer to be destroyed must have been created with a call
 * to class_create().
 */
/**
 * 销毁一个 struct class 结构
 * @cls: 指向要被销毁的类对象的指针
 *
 * 注意，要销毁的指针必须是通过调用 class_create() 创建的。
 */
/**
 * class_destroy()是与class_create()相对的删除class的函数。
 * 
 * 虽然在class_destroy()中没有看到释放class内存的代码，但这是在class_create_release()中做的。
 * class_create_release()之前已经在class_create()中被作为class结构中定义的class_release()函数，
 * 会在class引用计数降为零时被调用。
 * 
 * 在class中，class结构和class_private结构都是在class引用计数降为零时才释放的。
 * 这保证了即使class已经被注销，仍然不会影响其下设备的正常使用。但在bus中，
 * bus_private结构是在bus_unregister()中就被释放的。没有了bus_private，
 * bus下面的device和driver想必都无法正常工作了吧。这或许和bus对应与实际总线有关。总线都没了，下面的设备自然没人用了。
 */
void class_destroy(struct class *cls)
{
	if ((cls == NULL) || (IS_ERR(cls)))
		return;

	// 注销 class
	class_unregister(cls);
}

#ifdef CONFIG_SYSFS_DEPRECATED
char *make_class_name(const char *name, struct kobject *kobj)
{
	char *class_name;
	int size;

	size = strlen(name) + strlen(kobject_name(kobj)) + 2;

	class_name = kmalloc(size, GFP_KERNEL);
	if (!class_name)
		return NULL;

	strcpy(class_name, name);
	strcat(class_name, ":");
	strcat(class_name, kobject_name(kobj));
	return class_name;
}
#endif

// class为了遍历设备链表，特意定义了专门的结构和遍历函数，实现如下。
// 之所以要如此费一番周折，在klist_iter外面加上这一层封装，完全是为了对链表进行选择性遍历。
// 选择的条件就是device_type。device_type是在device结构中使用的类型，
// 其中定义了相似设备使用的一些处理操作，可以说比class的划分还要小一层。
/**
 * class_dev_iter_init - initialize class device iterator
 * @iter: class iterator to initialize
 * @class: the class we wanna iterate over
 * @start: the device to start iterating from, if any
 * @type: device_type of the devices to iterate over, NULL for all
 *
 * Initialize class iterator @iter such that it iterates over devices
 * of @class.  If @start is set, the list iteration will start there,
 * otherwise if it is NULL, the iteration starts at the beginning of
 * the list.
 */
/**
 * 初始化类设备迭代器
 * @iter: 要初始化的类迭代器
 * @class: 我们想要迭代的类
 * @start: 迭代开始的设备（如果有的话）
 * @type: 要迭代的设备类型，如果为 NULL 则迭代所有类型
 *
 * 初始化类迭代器 @iter 以便它可以遍历 @class 的设备。
 * 如果设置了 @start，则列表迭代将从那里开始，
 * 否则，如果它为 NULL，迭代从列表的开始处开始。
 */
void class_dev_iter_init(struct class_dev_iter *iter, struct class *class,
			 struct device *start, const struct device_type *type)
{
	struct klist_node *start_knode = NULL;

	// start被设置，列表遍历从start开始；否则，从列表的表头开始。
	if (start)
		start_knode = &start->knode_class;
	// 初始化 klist 迭代器
	klist_iter_init_node(&class->p->class_devices, &iter->ki, start_knode);
	iter->type = type;		// 设置迭代器的设备类型
}
EXPORT_SYMBOL_GPL(class_dev_iter_init);

/**
 * class_dev_iter_next - iterate to the next device
 * @iter: class iterator to proceed
 *
 * Proceed @iter to the next device and return it.  Returns NULL if
 * iteration is complete.
 *
 * The returned device is referenced and won't be released till
 * iterator is proceed to the next device or exited.  The caller is
 * free to do whatever it wants to do with the device including
 * calling back into class code.
 */
/**
 * class_dev_iter_next - 迭代到下一个设备
 * @iter: 要进行的类迭代器
 *
 * 将 @iter 迭代器移动到下一个设备并返回它。如果迭代完成则返回 NULL。
 *
 * 返回的设备已被引用，并且在迭代器移动到下一个设备或退出之前不会被释放。
 * 调用者可以自由地对设备进行任何操作，包括调用类代码。
 */
struct device *class_dev_iter_next(struct class_dev_iter *iter)
{
	struct klist_node *knode;		// 指向 klist 节点的指针
	struct device *dev;					// 指向设备的指针

	while (1) {
		knode = klist_next(&iter->ki);	// 获取迭代器中的下一个 klist 节点
		if (!knode)
			return NULL;	// 如果没有更多节点，返回 NULL
		// 从 knode 获取设备指针
		dev = container_of(knode, struct device, knode_class);
		// 检查设备类型是否匹配
		if (!iter->type || iter->type == dev->type)
			return dev;	// 返回设备
	}
}
EXPORT_SYMBOL_GPL(class_dev_iter_next);

/**
 * class_dev_iter_exit - finish iteration
 * @iter: class iterator to finish
 *
 * Finish an iteration.  Always call this function after iteration is
 * complete whether the iteration ran till the end or not.
 */
/**
 * class_dev_iter_exit - 结束迭代
 * @iter: 要结束的类迭代器
 *
 * 结束一个迭代过程。无论迭代是否运行到结束，都应该调用这个函数。
 */
void class_dev_iter_exit(struct class_dev_iter *iter)
{
	// 结束 klist 迭代器
	klist_iter_exit(&iter->ki);
}
EXPORT_SYMBOL_GPL(class_dev_iter_exit);

/**
 * class_for_each_device - device iterator
 * @class: the class we're iterating
 * @start: the device to start with in the list, if any.
 * @data: data for the callback
 * @fn: function to be called for each device
 *
 * Iterate over @class's list of devices, and call @fn for each,
 * passing it @data.  If @start is set, the list iteration will start
 * there, otherwise if it is NULL, the iteration starts at the
 * beginning of the list.
 *
 * We check the return of @fn each time. If it returns anything
 * other than 0, we break out and return that value.
 *
 * @fn is allowed to do anything including calling back into class
 * code.  There's no locking restriction.
 */
/**
 * class_for_each_device - 设备迭代器
 * @class: 我们正在迭代的类
 * @start: 如果有的话，列表中开始迭代的设备
 * @data: 回调函数的数据
 * @fn: 对每个设备调用的函数
 *
 * 遍历 @class 的设备列表，并对每个设备调用 @fn 函数，传递 @data 给它。
 * 如果设置了 @start，列表迭代将从那里开始，否则，如果它为 NULL，迭代从列表的开始处开始。
 *
 * 我们每次都检查 @fn 的返回值。如果返回的不是 0，我们就中断循环并返回那个值。
 *
 * @fn 可以进行任何操作，包括调用类代码。没有锁定限制。
 */
int class_for_each_device(struct class *class, struct device *start,
			  void *data, int (*fn)(struct device *, void *))
{
	struct class_dev_iter iter;	// 设备迭代器
	struct device *dev;	// 迭代到的设备
	int error = 0;			// 错误码，默认为 0

	if (!class)
		return -EINVAL;	// 如果 class 为空，返回无效参数错误
	if (!class->p) {
		// 如果 class 未初始化，发出警告
		WARN(1, "%s called for class '%s' before it was initialized",
		     __func__, class->name);
		return -EINVAL;	// 返回无效参数错误
	}

	// 初始化迭代器
	class_dev_iter_init(&iter, class, start, NULL);
	// 遍历设备
	while ((dev = class_dev_iter_next(&iter))) {
		// 对每个设备执行 fn 函数
		error = fn(dev, data);
		if (error)
			break;	// 如果 fn 返回非 0 值，中断循环
	}
	class_dev_iter_exit(&iter);	// 结束迭代器

	return error;		// 返回错误码或 fn 函数的返回值
}
EXPORT_SYMBOL_GPL(class_for_each_device);

/**
 * class_find_device - device iterator for locating a particular device
 * @class: the class we're iterating
 * @start: Device to begin with
 * @data: data for the match function
 * @match: function to check device
 *
 * This is similar to the class_for_each_dev() function above, but it
 * returns a reference to a device that is 'found' for later use, as
 * determined by the @match callback.
 *
 * The callback should return 0 if the device doesn't match and non-zero
 * if it does.  If the callback returns non-zero, this function will
 * return to the caller and not iterate over any more devices.
 *
 * Note, you will need to drop the reference with put_device() after use.
 *
 * @fn is allowed to do anything including calling back into class
 * code.  There's no locking restriction.
 */
/**
 * class_find_device - 用于定位特定设备的设备迭代器
 * @class: 我们要迭代的类
 * @start: 开始迭代的设备
 * @data: 用于匹配函数的数据
 * @match: 用于检查设备的函数
 *
 * 这个函数与上面的 class_for_each_device 函数类似，但它返回一个找到的设备的引用，
 * 以供后续使用，由 @match 回调确定。
 *
 * 如果设备不匹配，回调应返回0；如果匹配，返回非0。如果回调返回非零，这个函数将
 * 返回给调用者，并且不会迭代更多的设备。
 *
 * 注意，使用完后你需要使用 put_device() 释放引用。
 *
 * @fn 允许进行任何操作，包括回调类代码。没有锁定限制。
 */
struct device *class_find_device(struct class *class, struct device *start,
				 void *data,
				 int (*match)(struct device *, void *))
{
	struct class_dev_iter iter;	// 类设备迭代器
	struct device *dev;		// 用于存储当前迭代到的设备

	if (!class)
		return NULL;	// 如果 class 为空，直接返回 NULL
	if (!class->p) {
		// 如果 class 未初始化，发出警告
		WARN(1, "%s called for class '%s' before it was initialized",
		     __func__, class->name);
		return NULL;
	}

	// 初始化迭代器
	class_dev_iter_init(&iter, class, start, NULL);
	// 迭代设备
	while ((dev = class_dev_iter_next(&iter))) {
		// 使用匹配函数检查设备
		if (match(dev, data)) {
			// 如果匹配，增加设备的引用计数
			get_device(dev);
			break;	// 并跳出循环
		}
	}
	class_dev_iter_exit(&iter);	// 退出迭代器

	return dev;	// 返回找到的设备，如果没有找到，则为 NULL
}
EXPORT_SYMBOL_GPL(class_find_device);

/**
 * 注册一个类接口
 * @class_intf: 指向类接口结构体的指针
 *
 * 此函数用于注册一个类接口，并将其添加到指定的类中。如果类接口或类不存在，则返回错误。
 * 注册后，该接口会被添加到类的接口列表中，并对类中的每个设备调用接口的 add_dev 函数（如果定义了的话）。
 *
 * 返回 0 表示成功，负值表示错误。
 */
int class_interface_register(struct class_interface *class_intf)
{
	struct class *parent;
	struct class_dev_iter iter;
	struct device *dev;

	if (!class_intf || !class_intf->class)
		return -ENODEV;	// 如果接口或类不存在，返回错误

	// 获取类的引用
	parent = class_get(class_intf->class);
	if (!parent)
		return -EINVAL;	// 如果类无法获取，返回错误

	mutex_lock(&parent->p->class_mutex);	// 锁定类的互斥锁
	// 将接口添加到类的接口列表
	list_add_tail(&class_intf->node, &parent->p->class_interfaces);
	// 如果定义了 add_dev 函数
	if (class_intf->add_dev) {
		// 初始化设备迭代器
		class_dev_iter_init(&iter, parent, NULL, NULL);
		// 遍历类中的设备
		while ((dev = class_dev_iter_next(&iter)))
			// 对每个设备调用 add_dev 函数
			class_intf->add_dev(dev, class_intf);
		class_dev_iter_exit(&iter);	// 退出迭代器
	}
	// 解锁
	mutex_unlock(&parent->p->class_mutex);

	return 0;
}

/**
 * 注销一个类接口
 * @class_intf: 指向类接口结构体的指针
 *
 * 此函数用于注销一个已注册的类接口，从其所属的类中移除，并对类中的每个设备调用接口的 remove_dev 函数（如果定义了的话）。
 */
void class_interface_unregister(struct class_interface *class_intf)
{
	struct class *parent = class_intf->class;
	struct class_dev_iter iter;
	struct device *dev;

	// 如果没有父类，直接返回
	if (!parent)
		return;

	// 锁定类的互斥锁
	mutex_lock(&parent->p->class_mutex);
	// 从列表中删除接口
	list_del_init(&class_intf->node);
	// 如果定义了 remove_dev 函数
	if (class_intf->remove_dev) {
		// 初始化设备迭代器
		class_dev_iter_init(&iter, parent, NULL, NULL);
		// 遍历类中的设备
		while ((dev = class_dev_iter_next(&iter)))
			// 对每个设备调用 remove_dev 函数
			class_intf->remove_dev(dev, class_intf);
		// 退出迭代器
		class_dev_iter_exit(&iter);
	}
	mutex_unlock(&parent->p->class_mutex);	// 解锁

	// 减少对类的引用计数
	class_put(parent);
}

/**
 * 显示类属性字符串
 * @class: 指向类的指针
 * @attr: 指向类属性的指针
 * @buf: 缓冲区，用于存储属性值
 *
 * 此函数用于获取类属性字符串，并将其格式化为用户空间可读的形式。
 */
ssize_t show_class_attr_string(struct class *class, struct class_attribute *attr,
                        	char *buf)
{
	struct class_attribute_string *cs;
	// 获取包含 attr 的 class_attribute_string 结构
	cs = container_of(attr, struct class_attribute_string, attr);
	// 将字符串写入缓冲区
	return snprintf(buf, PAGE_SIZE, "%s\n", cs->str);
}

EXPORT_SYMBOL_GPL(show_class_attr_string);

struct class_compat {
	struct kobject *kobj;
};

/**
 * class_compat_register - register a compatibility class
 * @name: the name of the class
 *
 * Compatibility class are meant as a temporary user-space compatibility
 * workaround when converting a family of class devices to a bus devices.
 */
/**
 * 注册一个兼容性类
 * @name: 类的名称
 *
 * 兼容性类作为在将一类设备转换为总线设备时的临时用户空间兼容性解决方案。
 */

struct class_compat *class_compat_register(const char *name)
{
	// 定义一个兼容性类结构体指针
	struct class_compat *cls;

	// 为结构体分配内存
	cls = kmalloc(sizeof(struct class_compat), GFP_KERNEL);
	if (!cls)
		return NULL;
	// 创建并添加一个 kobject
	cls->kobj = kobject_create_and_add(name, &class_kset->kobj);
	// 如果 kobject 创建失败
	if (!cls->kobj) {
		kfree(cls);
		return NULL;
	}
	// 返回创建的兼容性类
	return cls;
}
EXPORT_SYMBOL_GPL(class_compat_register);

/**
 * class_compat_unregister - unregister a compatibility class
 * @cls: the class to unregister
 */
/**
 * 注销一个兼容性类
 * @cls: 要注销的类
 */
void class_compat_unregister(struct class_compat *cls)
{
	// 减少 kobject 的引用计数，可能导致其释放
	kobject_put(cls->kobj);
	// 释放兼容性类结构体占用的内存
	kfree(cls);
}
EXPORT_SYMBOL_GPL(class_compat_unregister);

/**
 * class_compat_create_link - create a compatibility class device link to
 *			      a bus device
 * @cls: the compatibility class
 * @dev: the target bus device
 * @device_link: an optional device to which a "device" link should be created
 */
/**
 * 创建一个兼容性类设备链接到总线设备
 * @cls: 兼容性类
 * @dev: 目标总线设备
 * @device_link: 可选的设备，创建一个名为 "device" 的链接
 *
 * 此函数用于创建一个从兼容性类到目标总线设备的链接，并可选地向父设备创建一个链接，
 * 类似于类设备的结构，以提供尽可能多的向后兼容性。
 */
int class_compat_create_link(struct class_compat *cls, struct device *dev,
			     struct device *device_link)
{
	int error;

	// 创建链接
	error = sysfs_create_link(cls->kobj, &dev->kobj, dev_name(dev));
	if (error)
		return error;

	/*
	 * Optionally add a "device" link (typically to the parent), as a
	 * class device would have one and we want to provide as much
	 * backwards compatibility as possible.
	 */
	// 如果提供了 device_link 设备
	if (device_link) {
		// 创建名为 "device" 的链接
		error = sysfs_create_link(&dev->kobj, &device_link->kobj,
					  "device");
		if (error)	// 如果创建失败
			// 移除之前创建的链接
			sysfs_remove_link(cls->kobj, dev_name(dev));
	}

	return error;
}
EXPORT_SYMBOL_GPL(class_compat_create_link);

/**
 * class_compat_remove_link - remove a compatibility class device link to
 *			      a bus device
 * @cls: the compatibility class
 * @dev: the target bus device
 * @device_link: an optional device to which a "device" link was previously
 * 		 created
 */
/**
 * class_compat_remove_link - 删除兼容性类设备与总线设备之间的链接
 * @cls: 兼容性类
 * @dev: 目标总线设备
 * @device_link: 可选的设备，之前可能已经创建了一个“device”链接
 */
void class_compat_remove_link(struct class_compat *cls, struct device *dev,
			      struct device *device_link)
{
	// 如果提供了 device_link，则删除名为 "device" 的 sysfs 链接
	if (device_link)
		sysfs_remove_link(&dev->kobj, "device");
	// 删除以设备名为名的 sysfs 链接
	sysfs_remove_link(cls->kobj, dev_name(dev));
}
EXPORT_SYMBOL_GPL(class_compat_remove_link);

/**
 * classes_init - 初始化类设备系统
 * 初始化类设备的核心数据结构，创建一个名为 "class" 的 kset
 */
int __init classes_init(void)
{
	// 创建并添加一个名为 "class" 的 kset，没有指定释放函数或父 kobject
	class_kset = kset_create_and_add("class", NULL, NULL);
	// 如果创建失败，返回内存错误
	if (!class_kset)
		return -ENOMEM;
	// 创建成功，返回0
	return 0;
}

EXPORT_SYMBOL_GPL(class_create_file);
EXPORT_SYMBOL_GPL(class_remove_file);
EXPORT_SYMBOL_GPL(class_unregister);
EXPORT_SYMBOL_GPL(class_destroy);

EXPORT_SYMBOL_GPL(class_interface_register);
EXPORT_SYMBOL_GPL(class_interface_unregister);
