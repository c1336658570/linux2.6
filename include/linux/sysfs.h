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
	const char		*name;
	mode_t			(*is_visible)(struct kobject *,
					      struct attribute *, int);
	struct attribute	**attrs;
};



/**
 * Use these macros to make defining attributes easier. See include/linux/device.h
 * for examples..
 */

#define __ATTR(_name,_mode,_show,_store) { \
	.attr = {.name = __stringify(_name), .mode = _mode },	\
	.show	= _show,					\
	.store	= _store,					\
}

#define __ATTR_RO(_name) { \
	.attr	= { .name = __stringify(_name), .mode = 0444 },	\
	.show	= _name##_show,					\
}

#define __ATTR_NULL { .attr = { .name = NULL } }

#define attr_name(_attr) (_attr).attr.name

struct vm_area_struct;

struct bin_attribute {
	struct attribute	attr;
	size_t			size;
	void			*private;
	ssize_t (*read)(struct kobject *, struct bin_attribute *,
			char *, loff_t, size_t);
	ssize_t (*write)(struct kobject *, struct bin_attribute *,
			 char *, loff_t, size_t);
	int (*mmap)(struct kobject *, struct bin_attribute *attr,
		    struct vm_area_struct *vma);
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
#define sysfs_bin_attr_init(bin_attr) sysfs_attr_init(&(bin_attr)->attr)

// 属性的操作
struct sysfs_ops {
	// 在读sysfs文件时该函数被调用
	ssize_t	(*show)(struct kobject *, struct attribute *,char *);
	// 在写sysfs文件时该函数被调用
	ssize_t	(*store)(struct kobject *,struct attribute *,const char *, size_t);
};

struct sysfs_dirent;

#ifdef CONFIG_SYSFS

int sysfs_schedule_callback(struct kobject *kobj, void (*func)(void *),
			    void *data, struct module *owner);

int __must_check sysfs_create_dir(struct kobject *kobj);
void sysfs_remove_dir(struct kobject *kobj);
int __must_check sysfs_rename_dir(struct kobject *kobj, const char *new_name);
int __must_check sysfs_move_dir(struct kobject *kobj,
				struct kobject *new_parent_kobj);

// 创建新的属性，attr参数指向相应的attribute结构体，kobj指向属性所在的kobject对象
// 成功返回0,失败返回负的错误码
int __must_check sysfs_create_file(struct kobject *kobj,
				   const struct attribute *attr);
int __must_check sysfs_create_files(struct kobject *kobj,
				   const struct attribute **attr);
int __must_check sysfs_chmod_file(struct kobject *kobj, struct attribute *attr,
				  mode_t mode);
// 在sysfs中删除一个属性
void sysfs_remove_file(struct kobject *kobj, const struct attribute *attr);
void sysfs_remove_files(struct kobject *kobj, const struct attribute **attr);

int __must_check sysfs_create_bin_file(struct kobject *kobj,
				       const struct bin_attribute *attr);
void sysfs_remove_bin_file(struct kobject *kobj,
			   const struct bin_attribute *attr);

// 在sysfs中创建一个符号链接，符号链接名由name指定，链接由kobj指定的目录映射到target指定的目录
int __must_check sysfs_create_link(struct kobject *kobj, struct kobject *target,
				   const char *name);
int __must_check sysfs_create_link_nowarn(struct kobject *kobj,
					  struct kobject *target,
					  const char *name);
// 在sysfs中删除一个符号链接
void sysfs_remove_link(struct kobject *kobj, const char *name);

int sysfs_rename_link(struct kobject *kobj, struct kobject *target,
			const char *old_name, const char *new_name);

int __must_check sysfs_create_group(struct kobject *kobj,
				    const struct attribute_group *grp);
int sysfs_update_group(struct kobject *kobj,
		       const struct attribute_group *grp);
void sysfs_remove_group(struct kobject *kobj,
			const struct attribute_group *grp);
int sysfs_add_file_to_group(struct kobject *kobj,
			const struct attribute *attr, const char *group);
void sysfs_remove_file_from_group(struct kobject *kobj,
			const struct attribute *attr, const char *group);

void sysfs_notify(struct kobject *kobj, const char *dir, const char *attr);
void sysfs_notify_dirent(struct sysfs_dirent *sd);
struct sysfs_dirent *sysfs_get_dirent(struct sysfs_dirent *parent_sd,
				      const unsigned char *name);
struct sysfs_dirent *sysfs_get(struct sysfs_dirent *sd);
void sysfs_put(struct sysfs_dirent *sd);
void sysfs_printk_last_file(void);
int __must_check sysfs_init(void);

#else /* CONFIG_SYSFS */

static inline int sysfs_schedule_callback(struct kobject *kobj,
		void (*func)(void *), void *data, struct module *owner)
{
	return -ENOSYS;
}

static inline int sysfs_create_dir(struct kobject *kobj)
{
	return 0;
}

static inline void sysfs_remove_dir(struct kobject *kobj)
{
}

static inline int sysfs_rename_dir(struct kobject *kobj, const char *new_name)
{
	return 0;
}

static inline int sysfs_move_dir(struct kobject *kobj,
				 struct kobject *new_parent_kobj)
{
	return 0;
}

// 创建新的属性，attr参数指向相应的attribute结构体，kobj指向属性所在的kobject对象
// 成功返回0,失败返回负的错误码
static inline int sysfs_create_file(struct kobject *kobj,
				    const struct attribute *attr)
{
	return 0;
}

static inline int sysfs_create_files(struct kobject *kobj,
				    const struct attribute **attr)
{
	return 0;
}

static inline int sysfs_chmod_file(struct kobject *kobj,
				   struct attribute *attr, mode_t mode)
{
	return 0;
}

// 在sysfs中删除一个属性
static inline void sysfs_remove_file(struct kobject *kobj,
				     const struct attribute *attr)
{
}

static inline void sysfs_remove_files(struct kobject *kobj,
				     const struct attribute **attr)
{
}

static inline int sysfs_create_bin_file(struct kobject *kobj,
					const struct bin_attribute *attr)
{
	return 0;
}

static inline void sysfs_remove_bin_file(struct kobject *kobj,
					 const struct bin_attribute *attr)
{
}

// 在sysfs中创建一个符号链接，符号链接名由name指定，链接由kobj指定的目录映射到target指定的目录
static inline int sysfs_create_link(struct kobject *kobj,
				    struct kobject *target, const char *name)
{
	return 0;
}

static inline int sysfs_create_link_nowarn(struct kobject *kobj,
					   struct kobject *target,
					   const char *name)
{
	return 0;
}

// 在sysfs中删除一个符号链接
static inline void sysfs_remove_link(struct kobject *kobj, const char *name)
{
}

static inline int sysfs_rename_link(struct kobject *k, struct kobject *t,
				    const char *old_name, const char *new_name)
{
	return 0;
}

static inline int sysfs_create_group(struct kobject *kobj,
				     const struct attribute_group *grp)
{
	return 0;
}

static inline int sysfs_update_group(struct kobject *kobj,
				const struct attribute_group *grp)
{
	return 0;
}

static inline void sysfs_remove_group(struct kobject *kobj,
				      const struct attribute_group *grp)
{
}

static inline int sysfs_add_file_to_group(struct kobject *kobj,
		const struct attribute *attr, const char *group)
{
	return 0;
}

static inline void sysfs_remove_file_from_group(struct kobject *kobj,
		const struct attribute *attr, const char *group)
{
}

static inline void sysfs_notify(struct kobject *kobj, const char *dir,
				const char *attr)
{
}
static inline void sysfs_notify_dirent(struct sysfs_dirent *sd)
{
}
static inline
struct sysfs_dirent *sysfs_get_dirent(struct sysfs_dirent *parent_sd,
				      const unsigned char *name)
{
	return NULL;
}
static inline struct sysfs_dirent *sysfs_get(struct sysfs_dirent *sd)
{
	return NULL;
}
static inline void sysfs_put(struct sysfs_dirent *sd)
{
}

static inline int __must_check sysfs_init(void)
{
	return 0;
}

static inline void sysfs_printk_last_file(void)
{
}

#endif /* CONFIG_SYSFS */

#endif /* _SYSFS_H_ */
