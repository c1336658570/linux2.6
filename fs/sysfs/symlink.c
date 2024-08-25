/*
 * fs/sysfs/symlink.c - sysfs symlink implementation
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 *
 * This file is released under the GPLv2.
 *
 * Please see Documentation/filesystems/sysfs.txt for more information.
 */

#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/mount.h>
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/namei.h>
#include <linux/mutex.h>
#include <linux/security.h>

#include "sysfs.h"

/**
 * sysfs_do_create_link - 创建sysfs中的符号链接
 * @kobj: 链接所在的kobject
 * @target: 链接指向的目标kobject
 * @name: 链接的名称
 * @warn: 如果为非零，则在添加链接时发出警告
 *
 * 创建sysfs文件系统中的一个符号链接，链接从kobj指向target。
 */
static int sysfs_do_create_link(struct kobject *kobj, struct kobject *target,
				const char *name, int warn)
{
	struct sysfs_dirent *parent_sd = NULL;
	struct sysfs_dirent *target_sd = NULL;
	struct sysfs_dirent *sd = NULL;
	struct sysfs_addrm_cxt acxt;
	int error;

	BUG_ON(!name);	// 检查name参数是否非空

	if (!kobj)
		// 如果kobj为空，使用sysfs的根目录作为父目录
		parent_sd = &sysfs_root;
	else
		// 否则使用kobj的目录项
		parent_sd = kobj->sd;

	error = -EFAULT;
	if (!parent_sd)	// 如果没有父目录项
		goto out_put;

	/* target->sd can go away beneath us but is protected with
	 * sysfs_assoc_lock.  Fetch target_sd from it.
	 */
	// 加锁以保护target->sd，从中获取target_sd
	spin_lock(&sysfs_assoc_lock);
	if (target->sd)
		// 获取target的目录项
		target_sd = sysfs_get(target->sd);
	spin_unlock(&sysfs_assoc_lock);

	error = -ENOENT;
	// 如果target的目录项为空
	if (!target_sd)
		goto out_put;

	error = -ENOMEM;
	// 创建一个新的目录项用于链接
	sd = sysfs_new_dirent(name, S_IFLNK|S_IRWXUGO, SYSFS_KOBJ_LINK);
	// 如果目录项创建失败
	if (!sd)
		goto out_put;

	// 设置链接的目标
	sd->s_symlink.target_sd = target_sd;
	// 目标目录项的引用现在被链接拥有
	target_sd = NULL;	/* reference is now owned by the symlink */

	// 开始添加目录项的操作
	sysfs_addrm_start(&acxt, parent_sd);
	if (warn)
		// 添加目录项到sysfs
		error = sysfs_add_one(&acxt, sd);
	else
		error = __sysfs_add_one(&acxt, sd);
	// 结束添加操作
	sysfs_addrm_finish(&acxt);

	// 如果操作出错
	if (error)
		goto out_put;

	return 0;

 out_put:
	sysfs_put(target_sd);	// 释放target目录项
	sysfs_put(sd);	// 释放新创建的目录项
	return error;
}

/**
 *	sysfs_create_link - create symlink between two objects.
 *	@kobj:	object whose directory we're creating the link in.
 *	@target:	object we're pointing to.
 *	@name:		name of the symlink.
 */
/**
 *	sysfs_create_link - 在两个对象之间创建符号链接
 *	@kobj:	我们在其目录中创建链接的对象
 *	@target:	链接指向的对象
 *	@cs:		符号链接的名称
 */
// 在sysfs中创建一个符号链接，符号链接名由name指定，链接由kobj指定的目录映射到target指定的目录
int sysfs_create_link(struct kobject *kobj, struct kobject *target,
		      const char *name)
{
	// 调用实际执行创建的函数，并指定发出警告
	return sysfs_do_create_link(kobj, target, name, 1);
}

/**
 *	sysfs_create_link_nowarn - create symlink between two objects.
 *	@kobj:	object whose directory we're creating the link in.
 *	@target:	object we're pointing to.
 *	@name:		name of the symlink.
 *
 *	This function does the same as sysf_create_link(), but it
 *	doesn't warn if the link already exists.
 */
/**
 *	sysfs_create_link_nowarn - 在两个对象之间创建符号链接，但不发出警告
 *	@kobj:	我们在其目录中创建链接的对象
 *	@target:	链接指向的对象
 *	@name:		符号链接的名称
 *
 *	此函数执行与sysfs_create_link()相同的操作，但如果链接已存在则不发出警告。
 */
// 这个函数执行与 sysfs_create_link() 相同的操作，但如果链接已经存在，则不发出警告
int sysfs_create_link_nowarn(struct kobject *kobj, struct kobject *target,
			     const char *name)
{
	// 调用实际执行创建的函数，但不发出警告
	return sysfs_do_create_link(kobj, target, name, 0);
}

/**
 *	sysfs_remove_link - remove symlink in object's directory.
 *	@kobj:	object we're acting for.
 *	@name:	name of the symlink to remove.
 */
/**
 *	sysfs_remove_link - 从对象的目录中移除符号链接
 *	@kobj:	我们代表的对象
 *	@name:	要移除的符号链接的名称
 */
// 在sysfs中删除一个符号链接
void sysfs_remove_link(struct kobject * kobj, const char * name)
{
	struct sysfs_dirent *parent_sd = NULL;

	// 如果kobj为空，使用sysfs的根目录
	if (!kobj)
		parent_sd = &sysfs_root;
	// 否则使用kobj的目录项
	else
		parent_sd = kobj->sd;

	// 从sysfs中删除指定的链接
	sysfs_hash_and_remove(parent_sd, name);
}

/**
 *	sysfs_rename_link - rename symlink in object's directory.
 *	@kobj:	object we're acting for.
 *	@targ:	object we're pointing to.
 *	@old:	previous name of the symlink.
 *	@new:	new name of the symlink.
 *
 *	A helper function for the common rename symlink idiom.
 */
/**
 *	sysfs_rename_link - 在对象的目录中重命名符号链接。
 *	@kobj:	执行操作的对象。
 *	@targ:	链接指向的目标对象。
 *	@old:	符号链接的旧名称。
 *	@new:	符号链接的新名称。
 *
 *	这是一个用于常见重命名符号链接习语的辅助函数。
 */
// 在对象的目录中重命名一个符号链接
int sysfs_rename_link(struct kobject *kobj, struct kobject *targ,
			const char *old, const char *new)
{
	// 定义父目录项和符号链接目录项变量
	struct sysfs_dirent *parent_sd, *sd = NULL;
	int result;

	// 如果kobj为空，使用sysfs的根目录
	if (!kobj)
		parent_sd = &sysfs_root;
	else
		parent_sd = kobj->sd;	// 否则使用kobj的sysfs目录项

	result = -ENOENT;
	// 获取旧链接的目录项
	sd = sysfs_get_dirent(parent_sd, old);
	if (!sd)
		goto out;	// 如果找不到旧链接，直接退出

	result = -EINVAL;	// 默认返回无效参数错误
	// 检查目录项是否是一个符号链接
	if (sysfs_type(sd) != SYSFS_KOBJ_LINK)
		goto out;
	// 检查链接目标是否正确
	if (sd->s_symlink.target_sd->s_dir.kobj != targ)
		goto out;

	// 调用sysfs_rename进行重命名
	result = sysfs_rename(sd, parent_sd, new);

out:
	sysfs_put(sd);	// 释放获取的目录项引用
	return result;	// 返回操作结果
}

// 计算两个sysfs目录项（parent_sd和target_sd）之间的相对路径，
// 并将其存储在提供的path字符串中
// 定义函数，输入为父目录项、目标目录项和路径存放的缓冲区
static int sysfs_get_target_path(struct sysfs_dirent *parent_sd,
				 struct sysfs_dirent *target_sd, char *path)
{
	// 定义基目录和当前遍历的目录项指针
	struct sysfs_dirent *base, *sd;
	char *s = path;	// 用于构建路径的指针
	int len = 0;	// 用于计算路径长度

	/* go up to the root, stop at the base */
	/* 向上遍历到根目录，停在基目录 */
	// 从parent_sd开始向上遍历到根目录，但停在与目标目录相交的最深的目录
	base = parent_sd;
	while (base->s_parent) {
		// 只要存在父目录就继续
		sd = target_sd->s_parent;
		// 从目标目录的父目录开始
		while (sd->s_parent && base != sd)
		// 查找共同的父目录
			sd = sd->s_parent;

		// 如果找到共同的父目录，则停止
		if (base == sd)
			break;

		strcpy(s, "../");	// 添加"../"到路径中表示上一级目录
		s += 3;	// 移动指针过"../"
		base = base->s_parent;	// 移动到下一级父目录
	}

	/* determine end of target string for reverse fillup */
	/* 确定目标字符串结束位置，用于逆向填充 */
	// 计算从共同的父目录到目标目录的路径长度
	sd = target_sd;
	while (sd->s_parent && sd != base) {
		// 累加目录名长度和一个斜杠的长度
		len += strlen(sd->s_name) + 1;
		// 向上移动至父目录
		sd = sd->s_parent;
	}

	/* check limits */
	/* 检查长度限制 */
	// 检查路径长度是否合法
	if (len < 2)	// 如果长度小于2，返回错误
		return -EINVAL;
	len--;	// 减去最后一个不需要的斜杠
	// 如果总长度超出最大路径长度，返回错误
	if ((s - path) + len > PATH_MAX)
		return -ENAMETOOLONG;

	/* reverse fillup of target string from target to base */
	/* 从目标到基目录逆向填充目标字符串 */
	// 逆向填充从目标目录到共同父目录的路径
	sd = target_sd;
	while (sd->s_parent && sd != base) {
		// 获取当前目录名的长度
		int slen = strlen(sd->s_name);

		len -= slen;	// 更新剩余长度
		// 复制目录名到正确位置
		strncpy(s + len, sd->s_name, slen);
		if (len)
			s[--len] = '/';	// 在目录名前添加斜杠

		sd = sd->s_parent;	// 向上移动至父目录
	}

	return 0;	// 返回成功
}

// 从给定的目录项dentry中获取符号链接的路径
static int sysfs_getlink(struct dentry *dentry, char * path)
{
	// 从目录项中获取sysfs目录项
	struct sysfs_dirent *sd = dentry->d_fsdata;
	// 获取父目录项
	struct sysfs_dirent *parent_sd = sd->s_parent;
	// 获取符号链接指向的目标目录项
	struct sysfs_dirent *target_sd = sd->s_symlink.target_sd;
	int error;

	mutex_lock(&sysfs_mutex);	// 加锁，保护文件系统操作
	// 获取从父目录到目标目录的路径
	error = sysfs_get_target_path(parent_sd, target_sd, path);
	mutex_unlock(&sysfs_mutex);	// 解锁

	return error;	// 返回操作结果
}

// 符号链接的追踪函数
static void *sysfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	int error = -ENOMEM;	// 默认错误为内存不足
	// 分配一个新的零初始化页面
	unsigned long page = get_zeroed_page(GFP_KERNEL);
	if (page) {
		// 获取符号链接的路径
		error = sysfs_getlink(dentry, (char *) page); 
		// 如果获取失败，释放页面
		if (error < 0)
			free_page((unsigned long)page);
	}
	// 设置nameidata结构的链接字段
	nd_set_link(nd, error ? ERR_PTR(error) : (char *)page);
	return NULL;  // 返回NULL，因为这个函数不需要返回指向目标的指针
}

// 释放与符号链接相关的资源
static void sysfs_put_link(struct dentry *dentry, struct nameidata *nd, void *cookie)
{
	// 获取链接的字符串
	char *page = nd_get_link(nd);
	if (!IS_ERR(page))
		// 释放页面
		free_page((unsigned long)page);
}

// 定义了用于sysfs符号链接的inode操作。这些操作允许文件系统执行如读取链接、跟随链接、设置属性等操作。
const struct inode_operations sysfs_symlink_inode_operations = {
	.setxattr	= sysfs_setxattr,       // 设置扩展属性，允许对符号链接设置元数据。
	.readlink	= generic_readlink,     // 读取符号链接的目标，使用通用函数。
	.follow_link	= sysfs_follow_link,    // 跟随符号链接到其目标，特定于sysfs的处理方式。
	.put_link	= sysfs_put_link,       // 在跟随符号链接后进行清理操作。
	.setattr	= sysfs_setattr,        // 设置文件属性，如文件权限、时间戳等。
	.getattr	= sysfs_getattr,        // 获取文件属性，如文件大小、权限、时间戳等。
	.permission	= sysfs_permission,     // 检查访问权限，确定用户是否有权对符号链接进行操作。
};


EXPORT_SYMBOL_GPL(sysfs_create_link);
EXPORT_SYMBOL_GPL(sysfs_remove_link);
