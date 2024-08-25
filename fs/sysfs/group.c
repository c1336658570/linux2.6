/*
 * fs/sysfs/group.c - Operations for adding/removing multiple files at once.
 *
 * Copyright (c) 2003 Patrick Mochel
 * Copyright (c) 2003 Open Source Development Lab
 *
 * This file is released undert the GPL v2. 
 *
 */

#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/err.h>
#include "sysfs.h"

/**
 * remove_files - 从sysfs中删除一组文件。
 * @dir_sd: sysfs目录项，其中包含要删除的文件。
 * @kobj: 指向与sysfs目录项相关联的kobject的指针。
 * @grp: 包含要删除的文件属性的组。
 *
 * 此函数遍历由grp指定的属性组，为每个属性调用sysfs_hash_and_remove函数来从sysfs中删除相应的文件。
 */
static void remove_files(struct sysfs_dirent *dir_sd, struct kobject *kobj,
			 const struct attribute_group *grp)
{
	struct attribute *const* attr;  // 指向属性指针数组的指针
	int i;  // 循环变量

	// 遍历属性数组，每个属性对应一个文件
	for (i = 0, attr = grp->attrs; *attr; i++, attr++) {
		// 调用sysfs_hash_and_remove来移除每个属性对应的文件
		sysfs_hash_and_remove(dir_sd, (*attr)->name);
	}
}

/**
 * create_files - 在sysfs中创建一组文件。
 * @dir_sd: 目录sysfs_dirent，其中文件将被创建。
 * @kobj: kobject关联到文件。
 * @grp: 属性组，包含将创建的文件的属性。
 * @update: 指示是否更新文件（更改权限或可见性）。
 *
 * 此函数遍历由grp指定的属性组，并尝试在sysfs中为每个属性创建文件。
 * 如果update为真，则首先移除然后重新添加文件（如果需要）。
 */
static int create_files(struct sysfs_dirent *dir_sd, struct kobject *kobj,
			const struct attribute_group *grp, int update)
{
	struct attribute *const* attr;  // 指向属性指针数组的指针
	int error = 0, i;  // 错误标志和循环变量

	// 遍历属性数组，每个属性可能对应一个文件
	for (i = 0, attr = grp->attrs; *attr && !error; i++, attr++) {
		mode_t mode = 0;  // 文件模式初始化

		/* in update mode, we're changing the permissions or
		 * visibility.  Do this by first removing then
		 * re-adding (if required) the file */
		// 如果处于更新模式，首先移除再根据需要重新添加文件
		if (update)
			sysfs_hash_and_remove(dir_sd, (*attr)->name);

		// 检查属性是否应该可见，如果是，获取其模式
		if (grp->is_visible) {
			mode = grp->is_visible(kobj, *attr, i);
			if (!mode)  // 如果模式为0，跳过此属性
				continue;
		}

		// 尝试添加文件，合并原始模式和新计算的模式
		error = sysfs_add_file_mode(dir_sd, *attr, SYSFS_KOBJ_ATTR,
					    (*attr)->mode | mode);
		if (unlikely(error))  // 如果添加文件失败，跳出循环
			break;
	}
	// 如果创建文件过程中出现错误，移除已经添加的文件
	if (error)
		remove_files(dir_sd, kobj, grp);

	return error;  // 返回错误状态
}

/**
 * internal_create_group - 创建或更新kobject的属性组。
 * @kobj: 要操作的kobject。
 * @update: 标志，指示是否为更新操作。
 * @grp: 要创建或更新的属性组。
 *
 * 此函数用于创建或更新与kobject关联的属性组。如果指定了grp->name，
 * 将会在kobj下创建一个新的子目录。之后，在该目录下创建属性组的文件。
 * 如果创建文件过程中出现错误，将会删除已创建的子目录。
 *
 * 返回值:
 * 成功时返回0，失败时返回负值错误代码。
 */
static int internal_create_group(struct kobject *kobj, int update,
				 const struct attribute_group *grp)
{
	struct sysfs_dirent *sd;  // 目录项指针
	int error;  // 错误代码变量

	// 检查输入参数的有效性
	BUG_ON(!kobj || (!update && !kobj->sd));

	/* Updates may happen before the object has been instantiated */
	// 如果在更新模式但kobject未实例化，返回无效参数错误
	if (unlikely(update && !kobj->sd))
		return -EINVAL;

	// 如果组名存在，创建相应的子目录
	if (grp->name) {
		error = sysfs_create_subdir(kobj, grp->name, &sd);
		if (error)  // 创建子目录失败，返回错误代码
			return error;
	} else
		sd = kobj->sd;  // 使用kobject的现有目录项

	// 增加目录项的引用计数
	sysfs_get(sd);

	// 在目录项下创建属性文件
	error = create_files(sd, kobj, grp, update);
	if (error) {  // 如果创建文件失败
		if (grp->name)  // 如果创建了新的子目录，移除它
			sysfs_remove_subdir(sd);
	}
	// 减少目录项的引用计数
	sysfs_put(sd);
	return error;  // 返回操作结果
}

/**
 * sysfs_create_group - given a directory kobject, create an attribute group
 * @kobj:	The kobject to create the group on
 * @grp:	The attribute group to create
 *
 * This function creates a group for the first time.  It will explicitly
 * warn and error if any of the attribute files being created already exist.
 *
 * Returns 0 on success or error.
 */
/**
 * sysfs_create_group - 在给定的目录kobject上创建属性组
 * @kobj: 要创建组的kobject
 * @grp: 要创建的属性组
 *
 * 该函数首次创建一个属性组。如果任何正在创建的属性文件已经存在，
 * 它将明确警告并返回错误。
 *
 * 返回值:
 * 成功返回0，否则返回错误代码。
 */
int sysfs_create_group(struct kobject *kobj,
		       const struct attribute_group *grp)
{
	// 调用内部创建函数，传入更新标志为0，表示这是一个创建操作
	return internal_create_group(kobj, 0, grp);
}

/**
 * sysfs_update_group - given a directory kobject, create an attribute group
 * @kobj:	The kobject to create the group on
 * @grp:	The attribute group to create
 *
 * This function updates an attribute group.  Unlike
 * sysfs_create_group(), it will explicitly not warn or error if any
 * of the attribute files being created already exist.  Furthermore,
 * if the visibility of the files has changed through the is_visible()
 * callback, it will update the permissions and add or remove the
 * relevant files.
 *
 * The primary use for this function is to call it after making a change
 * that affects group visibility.
 *
 * Returns 0 on success or error.
 */
/**
 * sysfs_update_group - 在给定的目录kobject上更新属性组
 * @kobj: 要更新组的kobject
 * @grp: 要创建的属性组
 *
 * 该函数更新一个属性组。与sysfs_create_group()不同，
 * 如果任何正在创建的属性文件已经存在，它不会发出警告或返回错误。
 * 此外，如果文件的可见性通过is_visible()回调发生变化，
 * 它将更新权限并添加或移除相关文件。
 *
 * 此函数的主要用途是在影响组可见性的更改后调用它。
 *
 * 返回值:
 * 成功返回0，否则返回错误代码。
 */
int sysfs_update_group(struct kobject *kobj,
		       const struct attribute_group *grp)
{
	// 调用内部创建函数，传入更新标志为1，表示这是一个更新操作
	return internal_create_group(kobj, 1, grp);
}

/**
 * sysfs_remove_group - 从kobject中移除一个属性组
 * @kobj: 目标对象
 * @grp: 要移除的属性组
 *
 * 此函数用于从sysfs中移除一个属性组。如果组是具名的，
 * 它会尝试找到并移除该组的目录。所有属于该组的文件都将被移除。
 */
void sysfs_remove_group(struct kobject * kobj, 
			const struct attribute_group * grp)
{
	struct sysfs_dirent *dir_sd = kobj->sd;  // 获取kobject关联的sysfs目录项
	struct sysfs_dirent *sd;

	if (grp->name) {  // 如果组有名字
		sd = sysfs_get_dirent(dir_sd, grp->name);  // 尝试获取该名字的目录项
		if (!sd) {  // 如果目录项不存在
			WARN(!sd, KERN_WARNING "sysfs group %p not found for "
				"kobject '%s'\n", grp, kobject_name(kobj));
			return;  // 打印警告信息并返回
		}
	} else
		sd = sysfs_get(dir_sd);  // 如果没有组名，则直接使用kobject的目录项

	remove_files(sd, kobj, grp);  // 移除该目录项下所有的文件
	if (grp->name)
		sysfs_remove_subdir(sd);  // 如果是具名组，则移除子目录

	sysfs_put(sd);  // 释放目录项
}

EXPORT_SYMBOL_GPL(sysfs_create_group);
EXPORT_SYMBOL_GPL(sysfs_update_group);
EXPORT_SYMBOL_GPL(sysfs_remove_group);
