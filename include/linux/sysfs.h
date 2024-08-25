/*
 * sysfs.h - definitions for the device driver filesystem
 *
 * Copyright (c) 2001,2002 Patrick Mochel
 * Copyright (c) 2004 Silicon Graphics, Inc.
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 *
 * Please see Documentation/filesystems/sysfs.txt for more information.
 */

#ifndef _SYSFS_H_
#define _SYSFS_H_

#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <asm/atomic.h>

struct kobject;
struct module;

/* FIXME
 * The *owner field is no longer used.
 * x86 tree has been cleaned up. The owner
 * attribute is still left for other arches.
 */
/* FIXME
 * *owner 字段已不再使用。
 * x86 架构已经清理了此字段。但 owner
 * 属性仍然保留在其他架构中。
 */
struct attribute {
	const char		*name;	// 属性的名称
	// 所属模块（如果存在）
	struct module		*owner;	// 指向拥有这个属性的模块的指针（在某些架构中已不再使用）
	mode_t			mode;	// 属性的访问权限模式（如只读、只写、可执行等）
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lock_class_key	*key;	// 锁类关键字，用于锁依赖检查
	struct lock_class_key	skey;	// 另一个锁类关键字，用于同样的目的
#endif
};

/**
 *	sysfs_attr_init - initialize a dynamically allocated sysfs attribute
 *	@attr: struct attribute to initialize
 *
 *	Initialize a dynamically allocated struct attribute so we can
 *	make lockdep happy.  This is a new requirement for attributes
 *	and initially this is only needed when lockdep is enabled.
 *	Lockdep gives a nice error when your attribute is added to
 *	sysfs if you don't have this.
 */
/**
 *	sysfs_attr_init - 初始化一个动态分配的 sysfs 属性
 *	@attr: 要初始化的 struct attribute
 *
 *	初始化一个动态分配的 struct attribute，以使 lockdep 满意。
 *	这是属性的一个新要求，最初只有在启用 lockdep 时才需要这样做。
 *	如果你没有执行此初始化，当你的属性被添加到 sysfs 时，
 *	lockdep 将会给出一个很好的错误提示。
 */
#ifdef CONFIG_DEBUG_LOCK_ALLOC
#define sysfs_attr_init(attr)				\
do {							\
	static struct lock_class_key __key;		\
							\
	(attr)->key = &__key;				\
} while(0)
#else
#define sysfs_attr_init(attr) do {} while(0)
#endif

struct attribute_group {
	const char		*name;  // 组的名称
	mode_t			(*is_visible)(struct kobject *,
					      struct attribute *, int);  // 函数指针，决定属性是否对用户可见
	struct attribute	**attrs;  // 属性数组的指针
};


/**
 * Use these macros to make defining attributes easier. See include/linux/device.h
 * for examples..
 */
/**
 * 使用这些宏可以简化属性定义。更多例子见 include/linux/device.h。
 */

// 定义一个属性，包括名称、模式（权限）、显示和存储方法
#define __ATTR(_name,_mode,_show,_store) { \
	.attr = {.name = __stringify(_name), .mode = _mode },	\
	.show	= _show,					\
	.store	= _store,					\
}

// 定义一个只读属性，名称由 _name 决定，模式固定为 0444，显示函数由 _name##_show 提供
#define __ATTR_RO(_name) { \
	.attr	= { .name = __stringify(_name), .mode = 0444 },	\
	.show	= _name##_show,					\
}

// 定义一个空属性，用作属性数组的终结符
#define __ATTR_NULL { .attr = { .name = NULL } }

// 从属性结构中获取属性名称的宏
#define attr_name(_attr) (_attr).attr.name

struct vm_area_struct;	// 前向声明，用于描述一个虚拟内存区域

struct bin_attribute {
	struct attribute    attr;   // 嵌入的 attribute 结构，提供基本属性定义
	size_t              size;   // 二进制数据的大小
	void                *private;   // 指向私有数据的指针，供操作函数使用
	ssize_t (*read)(struct kobject *, struct bin_attribute *,
		char *, loff_t, size_t);  // 读取函数指针，用于从设备读取数据
	ssize_t (*write)(struct kobject *, struct bin_attribute *,
		char *, loff_t, size_t);  // 写入函数指针，用于向设备写入数据
	int (*mmap)(struct kobject *, struct bin_attribute *attr,
		struct vm_area_struct *vma);  // 内存映射函数指针，用于支持内存映射操作
};

/**
 *	sysfs_bin_attr_init - initialize a dynamically allocated bin_attribute
 *	@attr: struct bin_attribute to initialize
 *
 *	Initialize a dynamically allocated struct bin_attribute so we
 *	can make lockdep happy.  This is a new requirement for
 *	attributes and initially this is only needed when lockdep is
 *	enabled.  Lockdep gives a nice error when your attribute is
 *	added to sysfs if you don't have this.
 */
/**
 *	sysfs_bin_attr_init - 初始化一个动态分配的 bin_attribute
 *	@attr: 要初始化的 struct bin_attribute
 *
 *	初始化一个动态分配的 struct bin_attribute 以使 lockdep 满意。
 *	这是属性的一个新要求，并且最初这只在 lockdep 启用时需要。
 *	如果你没有这样做，当你的属性被添加到 sysfs 时，lockdep 将给出一个很好的错误提示。
 */
// 使用 sysfs_attr_init 宏来初始化 bin_attribute 结构体中的 attribute 部分
#define sysfs_bin_attr_init(bin_attr) sysfs_attr_init(&(bin_attr)->attr)

// 属性的操作
struct sysfs_ops {
	// 在读sysfs文件时该函数被调用
	ssize_t	(*show)(struct kobject *, struct attribute *,char *);
	// 在写sysfs文件时该函数被调用
	ssize_t	(*store)(struct kobject *,struct attribute *,const char *, size_t);
};

// 前向声明一个结构，通常用于内部表示 sysfs 文件系统的目录项
struct sysfs_dirent;

#ifdef CONFIG_SYSFS	// 如果定义了 CONFIG_SYSFS，以下功能才会包含在内核构建中

int sysfs_schedule_callback(struct kobject *kobj, void (*func)(void *),
			    void *data, struct module *owner);

// 创建一个 sysfs 目录与给定的 kobject 关联
int __must_check sysfs_create_dir(struct kobject *kobj);
// 删除与 kobject 关联的 sysfs 目录
void sysfs_remove_dir(struct kobject *kobj);
// 重命名与 kobject 关联的 sysfs 目录
int __must_check sysfs_rename_dir(struct kobject *kobj, const char *new_name);
// 移动 kobject 关联的 sysfs 目录到一个新的父目录
int __must_check sysfs_move_dir(struct kobject *kobj,
                                struct kobject *new_parent_kobj);

// 创建新的属性，attr参数指向相应的attribute结构体，kobj指向属性所在的kobject对象。成功返回0,失败返回负的错误码
// 创建一个新的 sysfs 文件，该文件与指定的 kobject 和 attribute 关联
int __must_check sysfs_create_file(struct kobject *kobj,
                                   const struct attribute *attr);
// 创建多个 sysfs 文件，与指定的 kobject 和一组 attributes 关联
int __must_check sysfs_create_files(struct kobject *kobj,
                                    const struct attribute **attr);
// 修改与 kobject 和 attribute 关联的 sysfs 文件的权限
int __must_check sysfs_chmod_file(struct kobject *kobj, struct attribute *attr,
                                  mode_t mode);
// 在sysfs中删除一个属性
// 从 sysfs 中删除一个文件
void sysfs_remove_file(struct kobject *kobj, const struct attribute *attr);
// 从 sysfs 中删除多个文件
void sysfs_remove_files(struct kobject *kobj, const struct attribute **attr);

// 在 sysfs 中创建一个二进制文件，attr 参数指向相应的 bin_attribute 结构体，kobj 指向属性所在的 kobject 对象
int __must_check sysfs_create_bin_file(struct kobject *kobj,
				       const struct bin_attribute *attr);
// 在 sysfs 中删除一个二进制文件
void sysfs_remove_bin_file(struct kobject *kobj,
			   const struct bin_attribute *attr);

// 在sysfs中创建一个符号链接，符号链接名由name指定，链接由kobj指定的目录映射到target指定的目录
int __must_check sysfs_create_link(struct kobject *kobj, struct kobject *target,
				   const char *name);
// 在 sysfs 中创建一个符号链接，不发出警告
int __must_check sysfs_create_link_nowarn(struct kobject *kobj,
					  struct kobject *target,
					  const char *name);
// 在sysfs中删除一个符号链接
void sysfs_remove_link(struct kobject *kobj, const char *name);

// 在 sysfs 中重命名一个符号链接
int sysfs_rename_link(struct kobject *kobj, struct kobject *target,
			const char *old_name, const char *new_name);

// 在 sysfs 中创建一个属性组
int __must_check sysfs_create_group(struct kobject *kobj,
				    const struct attribute_group *grp);
// 更新 sysfs 中的一个属性组
int sysfs_update_group(struct kobject *kobj,
		       const struct attribute_group *grp);
// 从 sysfs 中删除一个属性组
void sysfs_remove_group(struct kobject *kobj,
			const struct attribute_group *grp);
// 向 sysfs 中指定的属性组添加一个文件
int sysfs_add_file_to_group(struct kobject *kobj,
			const struct attribute *attr, const char *group);
// 从 sysfs 中的指定属性组删除一个文件
void sysfs_remove_file_from_group(struct kobject *kobj,
			const struct attribute *attr, const char *group);

// 通知 sysfs 关于某个目录或属性的更改
void sysfs_notify(struct kobject *kobj, const char *dir, const char *attr);
// 通知 sysfs 目录项的更改
void sysfs_notify_dirent(struct sysfs_dirent *sd);
// 根据名称获取 sysfs 目录项的引用
struct sysfs_dirent *sysfs_get_dirent(struct sysfs_dirent *parent_sd,
				      const unsigned char *name);
// 增加 sysfs 目录项的引用计数
struct sysfs_dirent *sysfs_get(struct sysfs_dirent *sd);
// 减少 sysfs 目录项的引用计数
void sysfs_put(struct sysfs_dirent *sd);
// 打印 sysfs 最后操作的文件信息，用于调试
void sysfs_printk_last_file(void);
// 初始化 sysfs
int __must_check sysfs_init(void);

#else /* CONFIG_SYSFS */

// 安排一个回调函数在 kobject 关联的 sysfs 目录中执行
static inline int sysfs_schedule_callback(struct kobject *kobj,
		void (*func)(void *), void *data, struct module *owner)
{
	return -ENOSYS;  // 返回错误码，表示该函数在当前配置下不可用
}

// 在 sysfs 中为给定的 kobject 创建一个目录
static inline int sysfs_create_dir(struct kobject *kobj)
{
	return 0;  // 操作成功，返回 0
}

// 在 sysfs 中删除给定的 kobject 的目录
static inline void sysfs_remove_dir(struct kobject *kobj)
{
}

// 在 sysfs 中重命名给定的 kobject 的目录
static inline int sysfs_rename_dir(struct kobject *kobj, const char *new_name)
{
	return 0;  // 操作成功，返回 0
}

// 在 sysfs 中将给定的 kobject 的目录移动到新的父 kobject 的目录下
static inline int sysfs_move_dir(struct kobject *kobj,
				 struct kobject *new_parent_kobj)
{
	return 0;  // 操作成功，返回 0
}

// 在 sysfs 中为给定的 kobject 创建一个属性文件
// attr 参数指向相应的 attribute 结构体，kobj 指向属性所在的 kobject 对象
// 成功返回 0, 失败返回负的错误码
static inline int sysfs_create_file(struct kobject *kobj,
				    const struct attribute *attr)
{
	return 0;  // 操作成功，返回 0
}

// 在 sysfs 中为给定的 kobject 创建多个属性文件
// attr 参数是指向 attribute 结构体指针数组的指针
static inline int sysfs_create_files(struct kobject *kobj,
				    const struct attribute **attr)
{
	return 0;  // 操作成功，返回 0
}

// 修改 sysfs 中特定文件的权限
static inline int sysfs_chmod_file(struct kobject *kobj,
				   struct attribute *attr, mode_t mode)
{
	return 0;	// 假设操作总是成功
}

// 在 sysfs 中删除一个属性文件
static inline void sysfs_remove_file(struct kobject *kobj,
				     const struct attribute *attr)
{
}

// 在 sysfs 中删除多个属性文件
static inline void sysfs_remove_files(struct kobject *kobj,
				     const struct attribute **attr)
{
}

// 在 sysfs 中创建一个二进制文件
static inline int sysfs_create_bin_file(struct kobject *kobj,
					const struct bin_attribute *attr)
{
	return 0;	// 假设操作总是成功
}

// 在 sysfs 中删除一个二进制文件
static inline void sysfs_remove_bin_file(struct kobject *kobj,
					 const struct bin_attribute *attr)
{
}

// 在 sysfs 中创建一个符号链接，符号链接名由 name 指定，链接由 kobj 指定的目录映射到 target 指定的目录
static inline int sysfs_create_link(struct kobject *kobj,
				    struct kobject *target, const char *name)
{
	return 0;  // 假设操作总是成功
}

// 在 sysfs 中创建一个符号链接，不发出警告
static inline int sysfs_create_link_nowarn(struct kobject *kobj,
					   struct kobject *target,
					   const char *name)
{
	return 0;	// 假设操作总是成功
}

// 在sysfs中删除一个符号链接
static inline void sysfs_remove_link(struct kobject *kobj, const char *name)
{
}

// 在 sysfs 中重命名一个符号链接
static inline int sysfs_rename_link(struct kobject *k, struct kobject *t,
				    const char *old_name, const char *new_name)
{
	return 0;	// 假设操作总是成功
}

// 在 sysfs 中创建一个属性组
static inline int sysfs_create_group(struct kobject *kobj,
                                     const struct attribute_group *grp)
{
	return 0;  // 假设操作总是成功
}

// 更新 sysfs 中的一个属性组
static inline int sysfs_update_group(struct kobject *kobj,
                                     const struct attribute_group *grp)
{
	return 0;  // 假设操作总是成功
}

// 从 sysfs 中删除一个属性组
static inline void sysfs_remove_group(struct kobject *kobj,
				      const struct attribute_group *grp)
{
}

// 向 sysfs 中的指定属性组添加一个文件
static inline int sysfs_add_file_to_group(struct kobject *kobj,
		const struct attribute *attr, const char *group)
{
	return 0;	// 假设操作总是成功
}

// 从 sysfs 中的指定属性组删除一个文件
static inline void sysfs_remove_file_from_group(struct kobject *kobj,
		const struct attribute *attr, const char *group)
{
}

// 通知 sysfs 中的更改
static inline void sysfs_notify(struct kobject *kobj, const char *dir,
				const char *attr)
{
}
// 通知 sysfs 目录项的更改
static inline void sysfs_notify_dirent(struct sysfs_dirent *sd)
{
}
// 根据名称获取 sysfs 目录项的引用
static inline
struct sysfs_dirent *sysfs_get_dirent(struct sysfs_dirent *parent_sd,
				      const unsigned char *name)
{
	return NULL;	// 假设没有找到
}
// 增加 sysfs 目录项的引用计数
static inline struct sysfs_dirent *sysfs_get(struct sysfs_dirent *sd)
{
	return NULL;	// 假设没有找到
}
// 减少 sysfs 目录项的引用计数
static inline void sysfs_put(struct sysfs_dirent *sd)
{
}

// 初始化 sysfs
static inline int __must_check sysfs_init(void)
{
	return 0;	// 假设初始化总是成功
}

// 打印 sysfs 最后操作的文件信息，用于调试
static inline void sysfs_printk_last_file(void)
{
}

#endif /* CONFIG_SYSFS */

#endif /* _SYSFS_H_ */
