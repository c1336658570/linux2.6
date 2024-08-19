/*
 * kobject.c - library routines for handling generic kernel objects
 *
 * Copyright (c) 2002-2003 Patrick Mochel <mochel@osdl.org>
 * Copyright (c) 2006-2007 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (c) 2006-2007 Novell Inc.
 *
 * This file is released under the GPLv2.
 *
 *
 * Please see the file Documentation/kobject.txt for critical information
 * about using the kobject interface.
 */

#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/stat.h>
#include <linux/slab.h>

/*
 * populate_dir - populate directory with attributes.
 * @kobj: object we're working on.
 *
 * Most subsystems have a set of default attributes that are associated
 * with an object that registers with them.  This is a helper called during
 * object registration that loops through the default attributes of the
 * subsystem and creates attributes files for them in sysfs.
 */
/*
 * populate_dir - 用属性填充目录
 * @kobj: 我们正在处理的对象
 *
 * 大多数子系统都有一组与注册对象相关的默认属性。
 * 这是在对象注册期间调用的一个帮助函数，
 * 它循环遍历子系统的默认属性，并在sysfs中为它们创建属性文件。
 */
static int populate_dir(struct kobject *kobj)
{
	// 获取给定 kobject 的类型信息。
	struct kobj_type *t = get_ktype(kobj);
	struct attribute *attr;  // 定义一个指向属性的指针。
	int error = 0;  // 错误码初始化为0，表示没有错误。
	int i;  // 循环计数变量。

	// 检查是否获得了有效的 kobject 类型且该类型有默认属性。
	if (t && t->default_attrs) {
		// 遍历所有默认属性
		for (i = 0; (attr = t->default_attrs[i]) != NULL; i++) {
			// 为每个属性创建sysfs文件
			error = sysfs_create_file(kobj, attr);
			// 如果创建文件失败，跳出循环
			if (error)
				break;
			}
	}
	// 返回最后的错误码，如果没有错误则返回0
	return error;
}

// 用于在 sysfs 文件系统中为一个 kobject 对象创建目录，并在该目录中填充默认的属性文件
static int create_dir(struct kobject *kobj)
{
	int error = 0;  // 初始化错误码为0，表示没有错误。

	// 检查 kobject 是否有名称，只有具名的 kobject 才能在 sysfs 中创建目录。
	if (kobject_name(kobj)) {
		// 尝试在 sysfs 中为 kobject 创建目录。
		error = sysfs_create_dir(kobj);
		if (!error) {  // 如果目录创建成功。
			// 填充目录中的默认属性。
			error = populate_dir(kobj);
			if (error)  // 如果填充过程中出现错误。
				// 移除已创建的目录。
				sysfs_remove_dir(kobj);
		}
	}
	// 返回最终的错误码。如果所有操作都成功，返回0。
	return error;
}

//用于计算一个 kobject 在 sysfs 文件系统中的路径长度
static int get_kobj_path_length(struct kobject *kobj)
{
	int length = 1; // 初始化长度为1，考虑到根目录的斜线
	struct kobject *parent = kobj; // 从当前的kobject开始

	/* walk up the ancestors until we hit the one pointing to the
	 * root.
	 * Add 1 to strlen for leading '/' of each level.
	 */
	/* 一直向上遍历祖先，直到我们到达指向根的对象。
		* 每一级的前导 '/' 添加1到长度。
		*/
	do {
		// 如果父对象没有名称，返回0（无效的路径）
		if (kobject_name(parent) == NULL)
				return 0;
		// 增加当前 kobject 名称的长度加上一个字符（用于'/')
		length += strlen(kobject_name(parent)) + 1;
		// 移动到上一级父对象
		parent = parent->parent;
	} while (parent); // 继续循环，直到没有更多的父对象
	return length; // 返回计算得到的路径长度
}

// 填充一个由 kobject 指定的对象在 sysfs 文件系统中的完整路径
static void fill_kobj_path(struct kobject *kobj, char *path, int length)
{
	struct kobject *parent;

	--length; // 预留一个字符位置用于最终的字符串结束符 '\0'

	// 从传入的 kobject 开始，向上遍历其所有父对象
	for (parent = kobj; parent; parent = parent->parent) {
		int cur = strlen(kobject_name(parent)); // 获取当前 kobject 名称的长度
		/* back up enough to print this name with '/' */
		/* 回退足够的位置来打印这个名称和前面的 '/' */
		length -= cur;
		strncpy(path + length, kobject_name(parent), cur); // 复制名称到路径字符串中的正确位置
		*(path + --length) = '/'; // 在名称前添加斜杠，并更新位置
	}

	// 打印调试信息，显示 kobject 的名称、指针、函数名和最终的路径
	pr_debug("kobject: '%s' (%p): %s: path = '%s'\n", kobject_name(kobj),
		kobj, __func__, path);
}

/**
 * kobject_get_path - generate and return the path associated with a given kobj and kset pair.
 *
 * @kobj:	kobject in question, with which to build the path
 * @gfp_mask:	the allocation type used to allocate the path
 *
 * The result must be freed by the caller with kfree().
 */
/**
 * kobject_get_path - 生成并返回与给定的 kobj 和 kset 对应的路径。
 *
 * @kobj:	要构建路径的 kobject。
 * @gfp_mask:	用于分配路径的内存分配类型。
 *
 * 结果必须由调用者使用 kfree() 释放。
 */
char *kobject_get_path(struct kobject *kobj, gfp_t gfp_mask)
{
	char *path; // 用于存储生成的路径字符串
	int len; // 路径的长度

	// 计算 kobject 的路径长度
	len = get_kobj_path_length(kobj);
	if (len == 0)
		return NULL; // 如果长度为0，返回 NULL，表示无法获取路径

	// 使用指定的内存分配类型分配路径所需的内存
	path = kzalloc(len, gfp_mask);
	if (!path)
		return NULL; // 如果内存分配失败，返回 NULL

	// 填充路径字符串
	fill_kobj_path(kobj, path, len);

	// 返回生成的路径字符串
	return path;
}
EXPORT_SYMBOL_GPL(kobject_get_path);

/* add the kobject to its kset's list */
/* 将 kobject 添加到其 kset 的列表中 */
static void kobj_kset_join(struct kobject *kobj)
{
	// 如果 kobject 没有关联的 kset，则直接返回
	if (!kobj->kset)
		return;

	// 增加 kset 的引用计数
	kset_get(kobj->kset);

	// 锁定 kset 的列表锁
	spin_lock(&kobj->kset->list_lock);

	// 将 kobject 添加到 kset 的列表尾部
	list_add_tail(&kobj->entry, &kobj->kset->list);

	// 解锁 kset 的列表锁
	spin_unlock(&kobj->kset->list_lock);
}

/* remove the kobject from its kset's list */
/* 从其 kset 的列表中移除 kobject */
static void kobj_kset_leave(struct kobject *kobj)
{
	// 如果 kobject 没有关联的 kset，则直接返回
	if (!kobj->kset)
		return;

	// 锁定 kset 的列表锁
	spin_lock(&kobj->kset->list_lock);

	// 从 kset 的列表中删除 kobject，并重新初始化 kobject 的列表节点
	list_del_init(&kobj->entry);

	// 解锁 kset 的列表锁
	spin_unlock(&kobj->kset->list_lock);

	// 减少 kset 的引用计数
	kset_put(kobj->kset);
}

static void kobject_init_internal(struct kobject *kobj)
{
	// 如果传入的 kobject 指针为空，则直接返回，避免空指针操作。
	if (!kobj)
		return;
	// 初始化 kobject 的引用计数结构体。
	kref_init(&kobj->kref);
	// 初始化 kobject 链表头，用于将此 kobject 插入到 kset 或其他结构中。
	INIT_LIST_HEAD(&kobj->entry);
	// 将 kobject 在 sysfs 中的状态标记为未注册。
	kobj->state_in_sysfs = 0;
	// 将添加事件的状态标记为未发送。
	kobj->state_add_uevent_sent = 0;
	// 将移除事件的状态标记为未发送。
	kobj->state_remove_uevent_sent = 0;
	// 将 kobject 的初始化状态标记为已初始化。
	kobj->state_initialized = 1;
}

static int kobject_add_internal(struct kobject *kobj)
{
	int error = 0; // 初始化错误码为0，用于跟踪函数中可能出现的错误。
	struct kobject *parent;

	// 如果传入的 kobject 指针为空，则返回错误码 -ENOENT，表示条目不存在。
	if (!kobj)
		return -ENOENT;

	// 检查 kobject 是否有名称，如果名称为空或第一个字符为空字符，则警告并返回 -EINVAL。
	if (!kobj->name || !kobj->name[0]) {
		WARN(1, "kobject: (%p): attempted to be registered with empty "
					"name!\n", kobj);
		return -EINVAL;
	}

	// 获取 kobject 的父对象的引用，确保父对象在后续操作中有效。
	parent = kobject_get(kobj->parent);

	/* join kset if set, use it as parent if we do not already have one */
	// 如果 kobject 有指定的 kset，则尝试加入该 kset
	// 如果没有指定父对象，使用 kset 的 kobject 作为父对象
	if (kobj->kset) {
		if (!parent)
				parent = kobject_get(&kobj->kset->kobj);
		kobj_kset_join(kobj);		// 将 kobject 加入到 kset 中。
		kobj->parent = parent;	// 更新 kobject 的父对象指针。
	}

	// 打印调试信息，显示 kobject 的名称、内存地址、当前函数和父对象的名称或 "<NULL>"。
	pr_debug("kobject: '%s' (%p): %s: parent: '%s', set: '%s'\n",
						kobject_name(kobj), kobj, __func__,
						parent ? kobject_name(parent) : "<NULL>",
						kobj->kset ? kobject_name(&kobj->kset->kobj) : "<NULL>");

	// 尝试为 kobject 创建目录
	error = create_dir(kobj);
	if (error) {
		// 如果创建目录失败，将 kobject 从 kset 中移除，减少对父对象的引用。
		kobj_kset_leave(kobj);
		kobject_put(parent);
		kobj->parent = NULL;	// 清除 kobject 的父对象指针。

		/* be noisy on error issues */
		// 如果出现错误，打印错误信息并输出调用栈
		// 如果错误码为 -EEXIST，打印具体的错误信息，提示同名目录已存在的问题。
		if (error == -EEXIST)
				printk(KERN_ERR "%s failed for %s with "
								"-EEXIST, don't try to register things with "
								"the same name in the same directory.\n",
								__func__, kobject_name(kobj));
		else
				printk(KERN_ERR "%s failed for %s (%d)\n",
								__func__, kobject_name(kobj), error);
		dump_stack();	// 打印调用栈以帮助诊断错误。
	} else {
		// 如果目录创建成功，标记 kobject 已在 sysfs 中注册
		kobj->state_in_sysfs = 1;
	}

	return error;	// 返回操作结果的错误码。
}

/**
 * kobject_set_name_vargs - Set the name of an kobject
 * @kobj: struct kobject to set the name of
 * @fmt: format string used to build the name
 * @vargs: vargs to format the string.
 */
/**
 * kobject_set_name_vargs - 设置 kobject 的名称
 * @kobj: 要设置名称的 struct kobject
 * @fmt: 用于构建名称的格式字符串
 * @vargs: 用于格式化字符串的变量参数列表。
 */
int kobject_set_name_vargs(struct kobject *kobj, const char *fmt,
				  va_list vargs)
{
	const char *old_name = kobj->name; // 保存旧名称以便之后释放
	char *s;

	// 如果 kobject 已经有名称，且未提供新的格式字符串，则不进行更新
	if (kobj->name && !fmt)
		return 0;

	// 使用格式字符串和变量参数列表创建新名称，分配内存
	kobj->name = kvasprintf(GFP_KERNEL, fmt, vargs);
	// 如果内存分配失败，返回 -ENOMEM
	if (!kobj->name)
		return -ENOMEM;

	/* ewww... some of these buggers have '/' in the name ... */
	/* 将名称中的所有 '/' 字符替换为 '!'，因为在某些上下文中 '/' 是不合法的 */
	while ((s = strchr(kobj->name, '/')))
		s[0] = '!';

	// 释放旧名称的内存
	kfree(old_name);
	return 0; // 返回 0 表示操作成功
}

/**
 * kobject_set_name - Set the name of a kobject
 * @kobj: struct kobject to set the name of
 * @fmt: format string used to build the name
 *
 * This sets the name of the kobject.  If you have already added the
 * kobject to the system, you must call kobject_rename() in order to
 * change the name of the kobject.
 */
/**
 * kobject_set_name - 设置 kobject 的名称
 * @kobj: 要设置名称的 struct kobject
 * @fmt: 用于构建名称的格式字符串
 *
 * 此函数设置 kobject 的名称。如果你已经将 kobject 添加到系统中，
 * 那么你必须调用 kobject_rename() 来更改 kobject 的名称。
 */
int kobject_set_name(struct kobject *kobj, const char *fmt, ...)
{
	va_list vargs; // 定义一个 va_list 类型的变量，用来处理可变参数
	int retval; // 用于存储返回值

	va_start(vargs, fmt); // 初始化 vargs，以获得可变参数列表
	retval = kobject_set_name_vargs(kobj, fmt, vargs); // 调用 kobject_set_name_vargs 来设置名称
	va_end(vargs); // 清理 vargs，结束可变参数的处理

	return retval; // 返回从 kobject_set_name_vargs 获取的结果
}
EXPORT_SYMBOL(kobject_set_name);

/**
 * kobject_init - initialize a kobject structure
 * @kobj: pointer to the kobject to initialize
 * @ktype: pointer to the ktype for this kobject.
 *
 * This function will properly initialize a kobject such that it can then
 * be passed to the kobject_add() call.
 *
 * After this function is called, the kobject MUST be cleaned up by a call
 * to kobject_put(), not by a call to kfree directly to ensure that all of
 * the memory is cleaned up properly.
 */
/**
 * kobject_init - 初始化 kobject 结构体
 * @kobj: 指向要初始化的 kobject 的指针
 * @ktype: 指向此 kobject 的 ktype 的指针。
 *
 * 此函数将适当地初始化一个 kobject，以便它可以传递给 kobject_add() 调用。
 *
 * 在调用此函数后，必须通过调用 kobject_put() 来清理 kobject，不能直接通过调用 kfree 来清理，
 * 以确保所有的内存都被正确清理。
 */
void kobject_init(struct kobject *kobj, struct kobj_type *ktype)
{
	char *err_str; // 用于存储错误信息的字符串

	// 检查 kobj 是否为 NULL，如果是，则设置错误信息并跳转到错误处理代码
	if (!kobj) {
		err_str = "invalid kobject pointer!";
		goto error;
	}
	// 检查 ktype 是否为 NULL，如果是，则设置错误信息并跳转到错误处理代码
	if (!ktype) {
		err_str = "must have a ktype to be initialized properly!\n";
		goto error;
	}
	// 检查 kobject 是否已经被初始化，如果已经初始化，则打印错误并记录调用栈
	if (kobj->state_initialized) {
		/* do not error out as sometimes we can recover */
		printk(KERN_ERR "kobject (%p): tried to init an initialized "
						"object, something is seriously wrong.\n", kobj);
		dump_stack();
	}

	kobject_init_internal(kobj); // 调用内部初始化函数
	kobj->ktype = ktype; // 设置 kobject 的 ktype
	return; // 正常结束函数

error:
	// 错误处理代码：打印错误信息并记录调用栈
	printk(KERN_ERR "kobject (%p): %s\n", kobj, err_str);
	dump_stack();
}
EXPORT_SYMBOL(kobject_init);

/**
 * kobject_add_varg - 使用格式化参数添加 kobject 到系统
 * @kobj: 要添加的 kobject
 * @parent: kobject 的父对象
 * @fmt: 名称的格式化字符串
 * @vargs: 与格式化字符串相对应的变量参数列表
 *
 * 此函数先设置 kobject 的名称，然后将其添加到系统中。如果设置名称失败，
 * 将不会继续添加操作。
 */
static int kobject_add_varg(struct kobject *kobj, struct kobject *parent,
			    const char *fmt, va_list vargs)
{
	int retval; // 用于存储返回值

	// 调用 kobject_set_name_vargs 来根据提供的格式字符串和参数列表设置 kobject 的名称
	retval = kobject_set_name_vargs(kobj, fmt, vargs);
	if (retval) { // 如果设置名称失败
		printk(KERN_ERR "kobject: can not set name properly!\n"); // 打印错误信息
		return retval; // 返回错误码
	}
	kobj->parent = parent; // 设置 kobject 的父对象
	return kobject_add_internal(kobj); // 调用 kobject_add_internal 将 kobject 添加到系统
}

/**
 * kobject_add - the main kobject add function
 * @kobj: the kobject to add
 * @parent: pointer to the parent of the kobject.
 * @fmt: format to name the kobject with.
 *
 * The kobject name is set and added to the kobject hierarchy in this
 * function.
 *
 * If @parent is set, then the parent of the @kobj will be set to it.
 * If @parent is NULL, then the parent of the @kobj will be set to the
 * kobject associted with the kset assigned to this kobject.  If no kset
 * is assigned to the kobject, then the kobject will be located in the
 * root of the sysfs tree.
 *
 * If this function returns an error, kobject_put() must be called to
 * properly clean up the memory associated with the object.
 * Under no instance should the kobject that is passed to this function
 * be directly freed with a call to kfree(), that can leak memory.
 *
 * Note, no "add" uevent will be created with this call, the caller should set
 * up all of the necessary sysfs files for the object and then call
 * kobject_uevent() with the UEVENT_ADD parameter to ensure that
 * userspace is properly notified of this kobject's creation.
 */
/**
 * kobject_add - 主要的 kobject 添加函数
 * @kobj: 要添加的 kobject
 * @parent: kobject 的父对象的指针。
 * @fmt: 用来命名 kobject 的格式字符串。
 *
 * 这个函数设置 kobject 的名称并将其添加到 kobject 层级结构中。
 *
 * 如果 @parent 设置了，则 @kobj 的父对象将被设置为它。
 * 如果 @parent 为 NULL，则 @kobj 的父对象将被设置为与此 kobject 关联的 kset 的 kobject。
 * 如果没有分配 kset 到 kobject，则 kobject 将位于 sysfs 树的根目录。
 *
 * 如果此函数返回错误，必须调用 kobject_put() 来适当地清理与对象相关联的内存。
 * 在任何情况下都不应直接使用 kfree() 释放传递给此函数的 kobject，这可能会导致内存泄漏。
 *
 * 注意，此调用不会创建 "add" uevent，调用者应设置对象的所有必要的 sysfs 文件，
 * 然后调用 kobject_uevent() 并使用 UEVENT_ADD 参数来确保用户空间正确地得到此 kobject 创建的通知。
 */
int kobject_add(struct kobject *kobj, struct kobject *parent,
		const char *fmt, ...)
{
	va_list args; // 定义一个处理可变参数的 va_list
	int retval;   // 存储函数返回值

	// 如果 kobj 为空，则返回 -EINVAL（无效的参数）
	if (!kobj)
		return -EINVAL;

	// 检查 kobj 是否已经初始化，如果未初始化，则打印错误并返回 -EINVAL
	if (!kobj->state_initialized) {
		printk(KERN_ERR "kobject '%s' (%p): tried to add an "
		       "uninitialized object, something is seriously wrong.\n",
		       kobject_name(kobj), kobj);
		dump_stack();
		return -EINVAL;
	}

	va_start(args, fmt); // 初始化 args，以便读取可变参数列表
	retval = kobject_add_varg(kobj, parent, fmt, args); // 调用 kobject_add_varg 来添加 kobject
	va_end(args); // 清理 args

	return retval; // 返回操作结果
}
EXPORT_SYMBOL(kobject_add);

/**
 * kobject_init_and_add - initialize a kobject structure and add it to the kobject hierarchy
 * @kobj: pointer to the kobject to initialize
 * @ktype: pointer to the ktype for this kobject.
 * @parent: pointer to the parent of this kobject.
 * @fmt: the name of the kobject.
 *
 * This function combines the call to kobject_init() and
 * kobject_add().  The same type of error handling after a call to
 * kobject_add() and kobject lifetime rules are the same here.
 */
/**
 * kobject_init_and_add - 初始化 kobject 结构并将其添加到 kobject 层级结构中
 * @kobj: 指向要初始化的 kobject 的指针
 * @ktype: 指向此 kobject 的 ktype 的指针。
 * @parent: 指向此 kobject 的父对象的指针。
 * @fmt: kobject 的名称。
 *
 * 此函数结合了对 kobject_init() 和 kobject_add() 的调用。
 * 在调用 kobject_add() 后进行的错误处理和 kobject 生命周期规则在这里也适用。
 */
int kobject_init_and_add(struct kobject *kobj, struct kobj_type *ktype,
			 struct kobject *parent, const char *fmt, ...)
{
	va_list args; // 用于处理可变参数
	int retval;   // 存储函数返回值

	kobject_init(kobj, ktype); // 初始化 kobject

	va_start(args, fmt); // 初始化可变参数列表
	retval = kobject_add_varg(kobj, parent, fmt, args); // 将 kobject 添加到系统中
	va_end(args); // 清理可变参数列表

	return retval; // 返回添加操作的结果
}
EXPORT_SYMBOL_GPL(kobject_init_and_add);

/**
 * kobject_rename - change the name of an object
 * @kobj: object in question.
 * @new_name: object's new name
 *
 * It is the responsibility of the caller to provide mutual
 * exclusion between two different calls of kobject_rename
 * on the same kobject and to ensure that new_name is valid and
 * won't conflict with other kobjects.
 */
/**
 * kobject_rename - 更改对象的名称
 * @kobj: 待更名的对象。
 * @new_name: 对象的新名称。
 *
 * 调用者有责任提供对相同 kobject 的不同 kobject_rename 调用之间的互斥，
 * 并确保 new_name 有效且不会与其他 kobjects 冲突。
 */
int kobject_rename(struct kobject *kobj, const char *new_name)
{
	int error = 0; // 错误代码初始化为0
	const char *devpath = NULL; // 设备路径字符串
	const char *dup_name = NULL, *name; // 新名称的副本和当前名称
	char *devpath_string = NULL; // 用于环境变量的设备路径字符串
	char *envp[2]; // 环境变量数组

	kobj = kobject_get(kobj); // 增加 kobject 的引用计数
	if (!kobj)
		return -EINVAL; // 如果 kobj 为空，则返回错误
	if (!kobj->parent)
		return -EINVAL; // 如果 kobj 没有父对象，则返回错误

	devpath = kobject_get_path(kobj, GFP_KERNEL); // 获取 kobject 的路径
	if (!devpath) {
		error = -ENOMEM; // 如果获取路径失败，则返回内存不足错误
		goto out;
	}
	devpath_string = kmalloc(strlen(devpath) + 15, GFP_KERNEL); // 分配环境变量字符串的内存
	if (!devpath_string) {
		error = -ENOMEM; // 如果内存分配失败，则返回内存不足错误
		goto out;
	}
	sprintf(devpath_string, "DEVPATH_OLD=%s", devpath); // 格式化环境变量字符串
	envp[0] = devpath_string;
	envp[1] = NULL;

	name = dup_name = kstrdup(new_name, GFP_KERNEL); // 复制新名称
	if (!name) {
		error = -ENOMEM; // 如果名称复制失败，则返回内存不足错误
		goto out;
	}

	error = sysfs_rename_dir(kobj, new_name); // 在 sysfs 中重命名目录
	if (error)
		goto out;

	/* Install the new kobject name */
	/* 安装新的 kobject 名称 */
	dup_name = kobj->name;
	kobj->name = name;

	/* This function is mostly/only used for network interface.
	 * Some hotplug package track interfaces by their name and
	 * therefore want to know when the name is changed by the user. */
	/* 这个函数主要/只用于网络接口。
	 * 一些热插拔包跟踪接口名称，因此希望知道用户何时更改名称。 */
	kobject_uevent_env(kobj, KOBJ_MOVE, envp); // 发送 kobject 移动事件

out:
	kfree(dup_name); // 释放旧的名称内存
	kfree(devpath_string); // 释放设备路径字符串内存
	kfree(devpath); // 释放设备路径内存
	kobject_put(kobj); // 减少 kobject 的引用计数

	return error; // 返回操作结果
}
EXPORT_SYMBOL_GPL(kobject_rename);

/**
 * kobject_move - move object to another parent
 * @kobj: object in question.
 * @new_parent: object's new parent (can be NULL)
 */
/**
 * kobject_move - 将对象移动到另一个父对象
 * @kobj: 待移动的对象。
 * @new_parent: 对象的新父对象（可以为 NULL）
 */
int kobject_move(struct kobject *kobj, struct kobject *new_parent)
{
	int error; // 错误代码
	struct kobject *old_parent; // 原父对象
	const char *devpath = NULL; // 设备路径
	char *devpath_string = NULL; // 设备路径字符串
	char *envp[2]; // 环境变量数组

	kobj = kobject_get(kobj); // 增加 kobj 的引用计数
	if (!kobj)
		return -EINVAL; // 如果 kobj 为空，则返回 -EINVAL

	new_parent = kobject_get(new_parent); // 增加 new_parent 的引用计数
	if (!new_parent) {
		if (kobj->kset)
			new_parent = kobject_get(&kobj->kset->kobj); // 如果 new_parent 为空且 kobj 有 kset，则取 kset 的 kobj 作为新父对象
	}
	/* old object path */
	/* 获取旧的对象路径 */
	devpath = kobject_get_path(kobj, GFP_KERNEL); // 获取 kobj 的路径
	if (!devpath) {
		error = -ENOMEM; // 如果获取路径失败，则返回 -ENOMEM
		goto out;
	}
	devpath_string = kmalloc(strlen(devpath) + 15, GFP_KERNEL); // 为环境变量字符串分配内存
	if (!devpath_string) {
		error = -ENOMEM; // 如果内存分配失败，则返回 -ENOMEM
		goto out;
	}
	sprintf(devpath_string, "DEVPATH_OLD=%s", devpath); // 格式化环境变量字符串
	envp[0] = devpath_string;
	envp[1] = NULL;

	error = sysfs_move_dir(kobj, new_parent); // 在 sysfs 中移动目录
	if (error)
		goto out;

	old_parent = kobj->parent; // 保存原父对象
	kobj->parent = new_parent; // 设置新父对象
	new_parent = NULL;

	kobject_put(old_parent); // 减少原父对象的引用计数
	kobject_uevent_env(kobj, KOBJ_MOVE, envp); // 发送 kobject 移动事件

out:
	kobject_put(new_parent); // 减少新父对象的引用计数
	kobject_put(kobj); // 减少 kobj 的引用计数
	kfree(devpath_string); // 释放设备路径字符串内存
	kfree(devpath); // 释放设备路径内存
	return error; // 返回操作结果
}

/**
 * kobject_del - unlink kobject from hierarchy.
 * @kobj: object.
 */
/**
 * kobject_del - 从层级结构中移除 kobject。
 * @kobj: 对象。
 */
void kobject_del(struct kobject *kobj)
{
	if (!kobj) // 检查 kobj 是否为 NULL，如果是则直接返回，不进行任何操作
		return;

	sysfs_remove_dir(kobj); // 从 sysfs 中移除与 kobject 相关联的目录
	kobj->state_in_sysfs = 0; // 设置 kobject 的状态为未在 sysfs 中注册

	kobj_kset_leave(kobj); // 将 kobject 从其所属的 kset 中移除

	kobject_put(kobj->parent); // 减少对父 kobject 的引用计数
	kobj->parent = NULL; // 将 kobject 的父对象指针设置为 NULL
}

/**
 * kobject_get - increment refcount for object.
 * @kobj: object.
 */
/**
 * kobject_get - 增加对象的引用计数。
 * @kobj: 对象。
 */
struct kobject *kobject_get(struct kobject *kobj)
{
	if (kobj) // 如果传入的 kobj 不是 NULL
		kref_get(&kobj->kref); // 调用 kref_get 函数增加 kobj 的引用计数
	return kobj; // 返回传入的 kobj 指针
}

/*
 * kobject_cleanup - free kobject resources.
 * @kobj: object to cleanup
 */
/*
 * kobject_cleanup - 清理 kobject 的资源。
 * @kobj: 要清理的对象
 */
static void kobject_cleanup(struct kobject *kobj)
{
	struct kobj_type *t = get_ktype(kobj); // 获取 kobject 的类型
	const char *name = kobj->name; // 保存 kobject 的名称，用于最后可能的释放操作

	// 打印调试信息，显示 kobject 的名称和地址
	pr_debug("kobject: '%s' (%p): %s\n",
						kobject_name(kobj), kobj, __func__);

	// 如果 kobject 有类型但没有 release 函数，则打印调试警告
	if (t && !t->release)
		pr_debug("kobject: '%s' (%p): does not have a release() "
							"function, it is broken and must be fixed.\n",
							kobject_name(kobj), kobj);

	/* send "remove" if the caller did not do it but sent "add" */
	// 如果已发送添加事件但未发送移除事件，则自动发送移除事件
	if (kobj->state_add_uevent_sent && !kobj->state_remove_uevent_sent) {
		pr_debug("kobject: '%s' (%p): auto cleanup 'remove' event\n",
							kobject_name(kobj), kobj);
		kobject_uevent(kobj, KOBJ_REMOVE);
	}

	/* remove from sysfs if the caller did not do it */
	// 如果 kobject 仍在 sysfs 中，则自动从 sysfs 删除
	if (kobj->state_in_sysfs) {
		pr_debug("kobject: '%s' (%p): auto cleanup kobject_del\n",
							kobject_name(kobj), kobj);
		kobject_del(kobj);
	}

	// 如果 kobject 的类型有 release 函数，则调用它来释放 kobject
	if (t && t->release) {
		pr_debug("kobject: '%s' (%p): calling ktype release\n",
							kobject_name(kobj), kobj);
		t->release(kobj);
	}

	/* free name if we allocated it */
	// 如果分配了名称内存，则释放它
	if (name) {
		pr_debug("kobject: '%s': free name\n", name);
		kfree(name);
	}
}

/**
 * kobject_release - 当引用计数为零时释放 kobject 的资源。
 * @kref: 指向 kobject 的 kref 结构的指针。
 *
 * 这个函数被设计为在引用计数达到零时调用，它会调用 kobject_cleanup 来执行实际的清理工作。
 */
static void kobject_release(struct kref *kref)
{
	// 使用 container_of 宏从 kref 指针获取包含它的 kobject 指针
	kobject_cleanup(container_of(kref, struct kobject, kref));
}

/**
 * kobject_put - decrement refcount for object.
 * @kobj: object.
 *
 * Decrement the refcount, and if 0, call kobject_cleanup().
 */
/**
 * kobject_put - 减少对象的引用计数。
 * @kobj: 对象。
 *
 * 减少引用计数，并且如果计数为 0，则调用 kobject_cleanup()。
 */
void kobject_put(struct kobject *kobj)
{
	if (kobj) { // 如果 kobj 不为空
		// 检查 kobj 是否已初始化，如果没有初始化但调用了 kobject_put() 则发出警告
		if (!kobj->state_initialized)
			WARN(1, KERN_WARNING "kobject: '%s' (%p): is not "
							"initialized, yet kobject_put() is being "
							"called.\n", kobject_name(kobj), kobj);
		
		// 调用 kref_put 函数减少 kobj 的引用计数，并指定当引用计数达到0时调用 kobject_release 函数
		kref_put(&kobj->kref, kobject_release);
	}
}

/**
 * dynamic_kobj_release - 释放动态 kobject 的函数
 * @kobj: 要释放的 kobject 对象
 *
 * 当 kobject 的引用计数减至零时调用此函数来释放 kobject 占用的内存。
 */
static void dynamic_kobj_release(struct kobject *kobj)
{
	pr_debug("kobject: (%p): %s\n", kobj, __func__); // 打印调试信息，包含 kobject 的地址和当前函数名
	kfree(kobj); // 释放 kobject 所占用的内存
}

/**
 * dynamic_kobj_ktype - 用于动态 kobjects 的 kobj_type 结构
 *
 * 定义了一个 kobj_type 结构，包含了释放函数和 sysfs 操作集。
 */
static struct kobj_type dynamic_kobj_ktype = {
    .release    = dynamic_kobj_release, // 指定 kobject 的释放函数
    .sysfs_ops  = &kobj_sysfs_ops, // 指定 kobject 的 sysfs 操作集
};

/**
 * kobject_create - create a struct kobject dynamically
 *
 * This function creates a kobject structure dynamically and sets it up
 * to be a "dynamic" kobject with a default release function set up.
 *
 * If the kobject was not able to be created, NULL will be returned.
 * The kobject structure returned from here must be cleaned up with a
 * call to kobject_put() and not kfree(), as kobject_init() has
 * already been called on this structure.
 */
/**
 * kobject_create - 动态创建一个 struct kobject
 *
 * 此函数动态创建一个 kobject 结构体，并将其设置为“动态” kobject，使用默认的释放函数进行设置。
 *
 * 如果无法创建 kobject，将返回 NULL。
 * 从这里返回的 kobject 结构体必须通过调用 kobject_put() 清理，而不是 kfree()，
 * 因为已经在此结构体上调用了 kobject_init()。
 */
struct kobject *kobject_create(void)
{
	struct kobject *kobj; // 声明一个指向 kobject 的指针

	kobj = kzalloc(sizeof(*kobj), GFP_KERNEL); // 动态分配内存，初始化为零
	if (!kobj)
		return NULL; // 如果内存分配失败，返回 NULL

	kobject_init(kobj, &dynamic_kobj_ktype); // 使用动态 kobject 类型初始化 kobject
	return kobj; // 返回创建的 kobject
}

/**
 * kobject_create_and_add - create a struct kobject dynamically and register it with sysfs
 *
 * @name: the name for the kset
 * @parent: the parent kobject of this kobject, if any.
 *
 * This function creates a kobject structure dynamically and registers it
 * with sysfs.  When you are finished with this structure, call
 * kobject_put() and the structure will be dynamically freed when
 * it is no longer being used.
 *
 * If the kobject was not able to be created, NULL will be returned.
 */
/**
 * kobject_create_and_add - 动态创建并注册一个 struct kobject 到 sysfs
 *
 * @name: kset 的名称
 * @parent: 这个 kobject 的父 kobject，如果有的话。
 *
 * 此函数动态创建一个 kobject 结构体并将其注册到 sysfs。
 * 当你不再使用这个结构体时，调用 kobject_put()，当它不再被使用时将动态释放。
 *
 * 如果 kobject 未能创建，将返回 NULL。
 */
struct kobject *kobject_create_and_add(const char *name, struct kobject *parent)
{
	struct kobject *kobj; // kobject 指针
	int retval; // 用于存储函数返回值

	kobj = kobject_create(); // 动态创建一个 kobject
	if (!kobj)
		return NULL; // 如果创建失败，返回 NULL

	retval = kobject_add(kobj, parent, "%s", name); // 将 kobject 添加到 sysfs，设置名称和父对象
	if (retval) { // 检查添加操作是否成功
		printk(KERN_WARNING "%s: kobject_add error: %d\n",
		       __func__, retval); // 如果添加失败，打印警告信息
		kobject_put(kobj); // 减少引用计数，准备释放 kobject
		kobj = NULL; // 将 kobj 指针设置为 NULL
	}
	return kobj; // 返回 kobject 指针，如果成功则非 NULL，失败则为 NULL
}
EXPORT_SYMBOL_GPL(kobject_create_and_add);

/**
 * kset_init - initialize a kset for use
 * @k: kset
 */
/**
 * kset_init - 初始化 kset 以供使用
 * @k: kset
 */
void kset_init(struct kset *k)
{
	kobject_init_internal(&k->kobj); // 初始化 kset 中的 kobject
	INIT_LIST_HEAD(&k->list); // 初始化 kset 的列表头部
	spin_lock_init(&k->list_lock); // 初始化 kset 的自旋锁
}

/* default kobject attribute operations */
/* 默认的 kobject 属性操作 */
static ssize_t kobj_attr_show(struct kobject *kobj, struct attribute *attr,
			      char *buf)
{
	struct kobj_attribute *kattr; // 定义指向 kobj_attribute 的指针
	ssize_t ret = -EIO; // 初始化返回值为 -EIO，表示输入/输出错误

	// 使用 container_of 宏从 attr 获取包含它的 struct kobj_attribute 的指针
	kattr = container_of(attr, struct kobj_attribute, attr);
	// 检查是否存在 show 方法
	if (kattr->show)
		ret = kattr->show(kobj, kattr, buf); // 如果存在，则调用该方法，并将结果存储在 ret 中
	return ret; // 返回操作的结果
}

/**
 * kobj_attr_store - 将数据写入到 kobject 的属性中
 * @kobj: 相关的 kobject 对象
 * @attr: 要写入的属性
 * @buf: 包含要写入数据的缓冲区
 * @count: 要写入的数据长度
 *
 * 这个函数用于将用户提供的数据写入到指定的 kobject 属性中。
 * 如果属性有自定义的存储方法，则调用该方法。
 * 如果没有实现存储方法，则返回错误 -EIO。
 */
static ssize_t kobj_attr_store(struct kobject *kobj, struct attribute *attr,
			       const char *buf, size_t count)
{
	struct kobj_attribute *kattr; // 指向包含的 kobj_attribute 的指针
	ssize_t ret = -EIO; // 初始化返回值为 -EIO，表示输入/输出错误

	// 通过 attr 获取其封装的 struct kobj_attribute 结构
	kattr = container_of(attr, struct kobj_attribute, attr);
	// 检查是否定义了 store 方法
	if (kattr->store)
		ret = kattr->store(kobj, kattr, buf, count); // 如果有，调用该方法进行数据存储
	return ret; // 返回操作结果
}

/**
 * kobj_sysfs_ops - 定义 sysfs 操作，包括属性的显示和存储方法
 */
const struct sysfs_ops kobj_sysfs_ops = {
	.show   = kobj_attr_show,  // 显示 kobject 属性的方法
	.store  = kobj_attr_store, // 存储 kobject 属性的方法
};

/**
 * kset_register - initialize and add a kset.
 * @k: kset.
 */
/**
 * kset_register - 初始化并添加一个 kset。
 * @k: kset。
 */
int kset_register(struct kset *k)
{
	int err; // 用于存储错误代码

	if (!k)
		return -EINVAL; // 如果 k 为空，则返回 -EINVAL，表示无效的参数

	kset_init(k); // 初始化 kset
	err = kobject_add_internal(&k->kobj); // 添加 kset 的 kobject 到系统
	if (err)
			return err; // 如果添加失败，返回错误代码

	kobject_uevent(&k->kobj, KOBJ_ADD); // 发送一个添加事件
	return 0; // 操作成功，返回 0
}

/**
 * kset_unregister - remove a kset.
 * @k: kset.
 */
/**
 * kset_unregister - 移除一个 kset。
 * @k: kset。
 */
void kset_unregister(struct kset *k)
{
	if (!k)
		return; // 如果 k 为空，则直接返回，不执行任何操作

	kobject_put(&k->kobj); // 减少 kset 的 kobject 的引用计数
}

/**
 * kset_find_obj - search for object in kset.
 * @kset: kset we're looking in.
 * @name: object's name.
 *
 * Lock kset via @kset->subsys, and iterate over @kset->list,
 * looking for a matching kobject. If matching object is found
 * take a reference and return the object.
 */
/**
 * kset_find_obj - 在 kset 中搜索对象。
 * @kset: 我们正在查找的 kset。
 * @name: 对象的名称。
 *
 * 通过 @kset->subsys 锁定 kset，并遍历 @kset->list，
 * 查找匹配的 kobject。如果找到匹配的对象，则增加引用计数并返回该对象。
 */
struct kobject *kset_find_obj(struct kset *kset, const char *name)
{
	struct kobject *k;
	struct kobject *ret = NULL;  // 初始化返回值为空

	spin_lock(&kset->list_lock);  // 对 kset 的列表加锁，以保证线程安全
	list_for_each_entry(k, &kset->list, entry) {  // 遍历 kset 中的所有 kobject
		if (kobject_name(k) && !strcmp(kobject_name(k), name)) {  // 如果找到名称匹配的 kobject
			ret = kobject_get(k);  // 增加该 kobject 的引用计数并设置为返回值
			break;  // 找到后退出循环
		}
	}
	spin_unlock(&kset->list_lock);  // 解锁
	return ret;  // 返回找到的 kobject，如果没有找到则为 NULL
}

/**
 * kset_release - 释放 kset 对象
 * @kobj: 指向 kset 中的 kobject 对象的指针
 *
 * 当 kset 的引用计数为零时，这个函数被调用来释放 kset 占用的资源。
 */
static void kset_release(struct kobject *kobj)
{
	struct kset *kset = container_of(kobj, struct kset, kobj); // 从 kobject 结构中获取包含它的 kset 结构
	pr_debug("kobject: '%s' (%p): %s\n",
						kobject_name(kobj), kobj, __func__); // 打印调试信息，显示 kobject 的名称、地址和当前函数名
	kfree(kset); // 释放 kset 结构占用的内存
}

/**
 * kset_ktype - 定义 kset 对象的类型
 *
 * 这个结构指定了 kset 对象的 sysfs 操作和释放函数。
 */
static struct kobj_type kset_ktype = {
    .sysfs_ops   = &kobj_sysfs_ops, // 指向 sysfs 操作的指针，这些操作用于 kset 对象的 sysfs 表示
    .release     = kset_release,   // 指向 kset 的释放函数
};

/**
 * kset_create - create a struct kset dynamically
 *
 * @name: the name for the kset
 * @uevent_ops: a struct kset_uevent_ops for the kset
 * @parent_kobj: the parent kobject of this kset, if any.
 *
 * This function creates a kset structure dynamically.  This structure can
 * then be registered with the system and show up in sysfs with a call to
 * kset_register().  When you are finished with this structure, if
 * kset_register() has been called, call kset_unregister() and the
 * structure will be dynamically freed when it is no longer being used.
 *
 * If the kset was not able to be created, NULL will be returned.
 */
/**
 * kset_create - 动态创建一个 struct kset
 *
 * @name: kset 的名称
 * @uevent_ops: kset 的 uevent 操作结构体
 * @parent_kobj: 此 kset 的父 kobject，如果有的话。
 *
 * 此函数动态创建一个 kset 结构体。然后可以通过调用 kset_register() 将此结构体
 * 注册到系统中并在 sysfs 中显示。当你使用完这个结构体后，如果已经调用了
 * kset_register()，应调用 kset_unregister()，当不再使用时结构体将被动态释放。
 *
 * 如果无法创建 kset，将返回 NULL。
 */
static struct kset *kset_create(const char *name,
				const struct kset_uevent_ops *uevent_ops,
				struct kobject *parent_kobj)
{
	struct kset *kset; // kset 指针
	int retval; // 用于存储返回值

	kset = kzalloc(sizeof(*kset), GFP_KERNEL); // 动态分配并初始化 kset 内存
	if (!kset)
		return NULL; // 如果内存分配失败，返回 NULL

	retval = kobject_set_name(&kset->kobj, name); // 设置 kset 的 kobject 名称
	if (retval) { // 如果设置名称失败
		kfree(kset); // 释放已分配的 kset 内存
		return NULL; // 返回 NULL
	}

	kset->uevent_ops = uevent_ops; // 设置 kset 的 uevent 操作
	kset->kobj.parent = parent_kobj; // 设置 kset 的父 kobject
	
	/*
	 * The kobject of this kset will have a type of kset_ktype and belong to
	 * no kset itself.  That way we can properly free it when it is
	 * finished being used.
	 */
	// 设置 kset 的 kobject 类型为 kset_ktype，并确保 kset 自身不属于任何其他 kset
	kset->kobj.ktype = &kset_ktype;
	kset->kobj.kset = NULL;

	return kset; // 返回创建的 kset
}

/**
 * kset_create_and_add - create a struct kset dynamically and add it to sysfs
 *
 * @name: the name for the kset
 * @uevent_ops: a struct kset_uevent_ops for the kset
 * @parent_kobj: the parent kobject of this kset, if any.
 *
 * This function creates a kset structure dynamically and registers it
 * with sysfs.  When you are finished with this structure, call
 * kset_unregister() and the structure will be dynamically freed when it
 * is no longer being used.
 *
 * If the kset was not able to be created, NULL will be returned.
 */
/**
 * kset_create_and_add - 动态创建一个 struct kset 并将其添加到 sysfs
 *
 * @name: kset 的名称
 * @uevent_ops: kset 的事件操作结构体
 * @parent_kobj: 这个 kset 的父 kobject，如果有的话。
 *
 * 此函数动态创建一个 kset 结构体并将其注册到 sysfs。
 * 当你完成此结构体的使用后，应调用 kset_unregister()，当它不再被使用时结构体将被动态释放。
 *
 * 如果无法创建 kset，将返回 NULL。
 */
struct kset *kset_create_and_add(const char *name,
				 const struct kset_uevent_ops *uevent_ops,
				 struct kobject *parent_kobj)
{
	struct kset *kset; // 定义一个指向 kset 的指针
	int error; // 用于存储错误代码

	kset = kset_create(name, uevent_ops, parent_kobj); // 创建 kset
	if (!kset)
		return NULL; // 如果创建失败，返回 NULL

	error = kset_register(kset); // 注册 kset 到 sysfs
	if (error) {
		kfree(kset); // 如果注册失败，释放已分配的 kset
		return NULL; // 返回 NULL
	}
	return kset; // 返回创建并注册的 kset
}


EXPORT_SYMBOL_GPL(kset_create_and_add);

EXPORT_SYMBOL(kobject_get);
EXPORT_SYMBOL(kobject_put);
EXPORT_SYMBOL(kobject_del);

EXPORT_SYMBOL(kset_register);
EXPORT_SYMBOL(kset_unregister);
