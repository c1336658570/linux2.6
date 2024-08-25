/*
 * fs/sysfs/inode.c - basic sysfs inode and dentry operations
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 *
 * This file is released under the GPLv2.
 *
 * Please see Documentation/filesystems/sysfs.txt for more information.
 */

#undef DEBUG 

#include <linux/pagemap.h>
#include <linux/namei.h>
#include <linux/backing-dev.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/xattr.h>
#include <linux/security.h>
#include "sysfs.h"

// 声明一个指向 sysfs 超级块的外部变量
extern struct super_block * sysfs_sb;

// 定义 sysfs 地址空间操作
static const struct address_space_operations sysfs_aops = {
	.readpage	= simple_readpage,       // 简单读页操作，用于从磁盘读取数据到页面缓存
	.write_begin	= simple_write_begin,    // 简单的写操作开始处理，准备数据写入
	.write_end	= simple_write_end,      // 简单的写操作结束处理，完成数据写入并更新页面缓存
};

// 定义 sysfs 的后台设备信息
static struct backing_dev_info sysfs_backing_dev_info = {
	.name		= "sysfs",               // 后台设备的名称
	.ra_pages	= 0,	/* No readahead */ // 不进行预读，ra_pages 设置为0
	.capabilities	= BDI_CAP_NO_ACCT_AND_WRITEBACK, // 不进行写回和帐户管理
};

// 定义 sysfs inode 操作
static const struct inode_operations sysfs_inode_operations = {
	.permission	= sysfs_permission,    // 检查文件访问权限
	.setattr	= sysfs_setattr,        // 设置文件属性
	.getattr	= sysfs_getattr,        // 获取文件属性
	.setxattr	= sysfs_setxattr,       // 设置扩展文件属性
};

// 初始化 sysfs inode
int __init sysfs_inode_init(void)
{
	// 初始化后台设备信息，该函数是通用的后台设备信息初始化函数
	return bdi_init(&sysfs_backing_dev_info);
}

// 初始化 inode 属性
static struct sysfs_inode_attrs *sysfs_init_inode_attrs(struct sysfs_dirent *sd)
{
	struct sysfs_inode_attrs *attrs; // 存储分配的 inode 属性结构
	struct iattr *iattrs;            // 指向 iattr 结构的指针，用于设置文件属性

	// 为 inode 属性分配内存
	attrs = kzalloc(sizeof(struct sysfs_inode_attrs), GFP_KERNEL);
	if (!attrs)
			return NULL; // 内存分配失败，返回 NULL

	iattrs = &attrs->ia_iattr; // 获取指向 iattr 结构的指针

	/* assign default attributes */
	/* 分配默认属性 */
	iattrs->ia_mode = sd->s_mode; // 文件模式（权限）
	iattrs->ia_uid = 0;           // 用户 ID，设置为 0 （root 用户）
	iattrs->ia_gid = 0;           // 组 ID，设置为 0 （root 组）
	// 设置访问时间、修改时间和更改时间为当前时间
	iattrs->ia_atime = iattrs->ia_mtime = iattrs->ia_ctime = CURRENT_TIME;

	return attrs; // 返回初始化的 inode 属性结构
}

// 设置 sysfs 目录项的属性
int sysfs_sd_setattr(struct sysfs_dirent *sd, struct iattr *iattr)
{
	struct sysfs_inode_attrs *sd_attrs;  // 用于存储 inode 属性
	struct iattr *iattrs;                // 指向 inode 属性结构的指针
	unsigned int ia_valid = iattr->ia_valid;  // 标记哪些属性是有效的

	sd_attrs = sd->s_iattr;  // 获取当前目录项的属性

	if (!sd_attrs) {
		/* setting attributes for the first time, allocate now */
		// 如果是第一次设置属性，现在分配空间
		sd_attrs = sysfs_init_inode_attrs(sd);
		if (!sd_attrs)
			return -ENOMEM;  // 如果内存分配失败，返回错误
		sd->s_iattr = sd_attrs;  // 保存新分配的属性
	}
	/* attributes were changed at least once in past */
	// 属性在过去至少被修改过一次
	iattrs = &sd_attrs->ia_iattr;  // 获取属性结构的地址

	if (ia_valid & ATTR_UID)  // 检查 UID 是否需要更新
		iattrs->ia_uid = iattr->ia_uid;
	if (ia_valid & ATTR_GID)  // 检查 GID 是否需要更新
		iattrs->ia_gid = iattr->ia_gid;
	if (ia_valid & ATTR_ATIME)  // 检查访问时间是否需要更新
		iattrs->ia_atime = iattr->ia_atime;
	if (ia_valid & ATTR_MTIME)  // 检查修改时间是否需要更新
		iattrs->ia_mtime = iattr->ia_mtime;
	if (ia_valid & ATTR_CTIME)  // 检查更改时间是否需要更新
		iattrs->ia_ctime = iattr->ia_ctime;
	if (ia_valid & ATTR_MODE) {  // 检查模式（权限）是否需要更新
		umode_t mode = iattr->ia_mode;
		iattrs->ia_mode = sd->s_mode = mode;  // 更新模式
	}
	return 0;  // 成功完成
}

// 设置 sysfs 文件系统中一个目录项的属性
int sysfs_setattr(struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode; // 从 dentry 获取 inode
	struct sysfs_dirent *sd = dentry->d_fsdata; // 从 dentry 获取与文件系统特定数据关联的 sysfs 目录项
	int error;

	if (!sd) // 如果没有关联的 sysfs_dirent 结构，返回错误
		return -EINVAL;

	mutex_lock(&sysfs_mutex); // 锁定 sysfs 全局互斥锁
	error = inode_change_ok(inode, iattr); // 检查 inode 属性更改是否允许
	if (error) // 如果不允许，直接跳到函数结束
		goto out;

	/* ignore size changes */
	iattr->ia_valid &= ~ATTR_SIZE; // 忽略大小改变的属性

	error = inode_setattr(inode, iattr); // 设置 inode 属性
	if (error) // 如果设置失败，跳到函数结束
		goto out;

	error = sysfs_sd_setattr(sd, iattr); // 设置 sysfs 目录项的属性
out:
	mutex_unlock(&sysfs_mutex); // 解锁 sysfs 互斥锁
	return error; // 返回操作结果
}

// 设置 sysfs 目录项的安全数据
static int sysfs_sd_setsecdata(struct sysfs_dirent *sd, void **secdata, u32 *secdata_len)
{
	struct sysfs_inode_attrs *iattrs;  // 定义指向 inode 属性结构的指针
	void *old_secdata;                 // 用于保存旧的安全数据指针
	size_t old_secdata_len;            // 用于保存旧的安全数据长度

	iattrs = sd->s_iattr;              // 获取当前目录项的 inode 属性
	if (!iattrs)                       // 如果当前没有 inode 属性
		iattrs = sysfs_init_inode_attrs(sd);  // 尝试初始化 inode 属性
	if (!iattrs)                       // 如果属性初始化失败
		return -ENOMEM;               // 返回内存不足错误

	old_secdata = iattrs->ia_secdata;  // 保存旧的安全数据指针
	old_secdata_len = iattrs->ia_secdata_len;  // 保存旧的安全数据长度

	iattrs->ia_secdata = *secdata;     // 更新 inode 属性中的安全数据指针为新的安全数据
	iattrs->ia_secdata_len = *secdata_len;  // 更新 inode 属性中的安全数据长度为新的长度

	*secdata = old_secdata;            // 将旧的安全数据指针返回给调用者
	*secdata_len = old_secdata_len;    // 将旧的安全数据长度返回给调用者
	return 0;                          // 返回 0 表示成功
}

// 设置 sysfs 目录项的扩展属性
int sysfs_setxattr(struct dentry *dentry, const char *name, const void *value,
		size_t size, int flags)
{
	struct sysfs_dirent *sd = dentry->d_fsdata; // 获取与目录项关联的文件系统数据
	void *secdata; // 用于存储安全上下文数据
	int error; // 错误码
	u32 secdata_len = 0; // 安全数据长度

	if (!sd) // 如果没有关联的 sysfs_dirent 结构，返回错误
		return -EINVAL;

	// 检查属性名是否为安全相关的扩展属性
	if (!strncmp(name, XATTR_SECURITY_PREFIX, XATTR_SECURITY_PREFIX_LEN)) {
		const char *suffix = name + XATTR_SECURITY_PREFIX_LEN; // 获取安全属性的后缀名
		// 调用安全模块设置 inode 的安全属性
		error = security_inode_setsecurity(dentry->d_inode, suffix,
						value, size, flags);
		if (error) // 如果设置失败，直接返回错误
			goto out;

		// 获取 inode 的安全上下文
		error = security_inode_getsecctx(dentry->d_inode,
						&secdata, &secdata_len);
		if (error) // 如果获取失败，直接返回错误
			goto out;

		// 加锁更新 sysfs 目录项的安全数据
		mutex_lock(&sysfs_mutex);
		error = sysfs_sd_setsecdata(sd, &secdata, &secdata_len);
		mutex_unlock(&sysfs_mutex);

		// 释放获取的安全上下文
		if (secdata)
			security_release_secctx(secdata, secdata_len);
	} else // 如果属性名不是安全相关的属性，返回错误
		return -EINVAL;
out:
	return error; // 返回操作结果
}

// 为 inode 设置默认属性
static inline void set_default_inode_attr(struct inode *inode, mode_t mode)
{
	inode->i_mode = mode;  // 设置文件模式（权限）
	// 设置访问时间、修改时间和更改时间为当前时间
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
}

// 根据给定的属性更新 inode 的属性
static inline void set_inode_attr(struct inode *inode, struct iattr *iattr)
{
	inode->i_uid = iattr->ia_uid;  // 设置用户 ID
	inode->i_gid = iattr->ia_gid;  // 设置组 ID
	inode->i_atime = iattr->ia_atime;  // 设置最后访问时间
	inode->i_mtime = iattr->ia_mtime;  // 设置最后修改时间
	inode->i_ctime = iattr->ia_ctime;  // 设置最后状态更改时间
}

// 计算 sysfs 目录项的链接数
static int sysfs_count_nlink(struct sysfs_dirent *sd)
{
	struct sysfs_dirent *child;  // 用于遍历子目录项
	int nr = 0;  // 链接数

	// 遍历当前目录项的所有子目录项
	for (child = sd->s_dir.children; child; child = child->s_sibling) {
		// 如果子目录项类型为目录，则增加链接数
		if (sysfs_type(child) == SYSFS_DIR)
			nr++;
	}

	return nr + 2;  // 返回链接数加上两个基本链接（"." 和 ".."）
}

// 刷新 sysfs inode 属性以匹配其对应的 sysfs_dirent
static void sysfs_refresh_inode(struct sysfs_dirent *sd, struct inode *inode)
{
	struct sysfs_inode_attrs *iattrs = sd->s_iattr;  // 从目录项获取 inode 属性结构

	inode->i_mode = sd->s_mode;  // 设置 inode 的模式（权限）

	if (iattrs) {
		/* sysfs_dirent has non-default attributes
		 * get them from persistent copy in sysfs_dirent
		 */
		/* 如果 sysfs_dirent 有非默认属性，从 sysfs_dirent 的持久拷贝中获取它们 */
		set_inode_attr(inode, &iattrs->ia_iattr);  // 使用 sysfs_dirent 的属性更新 inode
		// 通知安全模块 inode 的安全上下文已更改
		security_inode_notifysecctx(inode,
																iattrs->ia_secdata,
																iattrs->ia_secdata_len);
	}

	if (sysfs_type(sd) == SYSFS_DIR)  // 如果目录项是一个目录
		inode->i_nlink = sysfs_count_nlink(sd);  // 计算并设置目录的链接数
}

// 获取 sysfs 目录项的属性
int sysfs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat)
{
	struct sysfs_dirent *sd = dentry->d_fsdata;  // 从目录项获取与文件系统相关的数据
	struct inode *inode = dentry->d_inode;  // 获取目录项对应的 inode

	mutex_lock(&sysfs_mutex);  // 锁定 sysfs 的全局互斥锁，保证线程安全
	sysfs_refresh_inode(sd, inode);  // 刷新 inode，确保 inode 的属性是最新的
	mutex_unlock(&sysfs_mutex);  // 解锁 sysfs 的全局互斥锁

	generic_fillattr(inode, stat);  // 填充 stat 结构体，这是通用的文件属性填充函数
	return 0;  // 返回 0，表示成功获取属性
}

// 初始化 sysfs 的 inode
static void sysfs_init_inode(struct sysfs_dirent *sd, struct inode *inode)
{
	struct bin_attribute *bin_attr;  // 用于二进制属性的指针

	inode->i_private = sysfs_get(sd);  // 设置 inode 的私有数据，增加引用计数
	inode->i_mapping->a_ops = &sysfs_aops;  // 设置地址空间操作函数
	inode->i_mapping->backing_dev_info = &sysfs_backing_dev_info;  // 设置后台设备信息
	inode->i_op = &sysfs_inode_operations;  // 设置 inode 操作函数

	set_default_inode_attr(inode, sd->s_mode);  // 设置 inode 的默认属性（权限等）
	sysfs_refresh_inode(sd, inode);  // 刷新 inode 的属性以匹配 sysfs_dirent

	/* initialize inode according to type */
	/* 根据 sysfs_dirent 的类型初始化 inode */
	switch (sysfs_type(sd)) {
	case SYSFS_DIR:  // 如果是目录类型
		inode->i_op = &sysfs_dir_inode_operations;  // 设置目录的 inode 操作
		inode->i_fop = &sysfs_dir_operations;  // 设置目录的文件操作
		break;
	case SYSFS_KOBJ_ATTR:  // 如果是普通属性文件
		inode->i_size = PAGE_SIZE;  // 设置文件大小为一个页面大小
		inode->i_fop = &sysfs_file_operations;  // 设置文件操作
		break;
	case SYSFS_KOBJ_BIN_ATTR:  // 如果是二进制属性文件
		bin_attr = sd->s_bin_attr.bin_attr;  // 获取二进制属性
		inode->i_size = bin_attr->size;  // 设置文件大小为二进制属性的大小
		inode->i_fop = &bin_fops;  // 设置二进制文件的操作
		break;
	case SYSFS_KOBJ_LINK:  // 如果是符号链接
		inode->i_op = &sysfs_symlink_inode_operations;  // 设置符号链接的 inode 操作
		break;
	default:
		BUG();  // 如果类型未知，则触发 bug
	}

	unlock_new_inode(inode);  // 解锁新 inode
}

/**
 *	sysfs_get_inode - get inode for sysfs_dirent
 *	@sb: super block
 *	@sd: sysfs_dirent to allocate inode for
 *
 *	Get inode for @sd.  If such inode doesn't exist, a new inode
 *	is allocated and basics are initialized.  New inode is
 *	returned locked.
 *
 *	LOCKING:
 *	Kernel thread context (may sleep).
 *
 *	RETURNS:
 *	Pointer to allocated inode on success, NULL on failure.
 */
/**
 * sysfs_get_inode - 获取 sysfs_dirent 的 inode
 * @sb: 超级块
 * @sd: 需要分配 inode 的 sysfs_dirent
 *
 * 获取 @sd 的 inode。如果这样的 inode 不存在，将会分配一个新的 inode
 * 并初始化基本属性。新分配的 inode 将会被锁定返回。
 *
 * 锁定：
 * 内核线程上下文（可能会休眠）。
 *
 * 返回值：
 * 成功时返回指向分配的 inode 的指针，失败时返回 NULL。
 */
struct inode *sysfs_get_inode(struct super_block *sb, struct sysfs_dirent *sd)
{
	struct inode *inode;

	inode = iget_locked(sb, sd->s_ino);  // 获取一个锁定的 inode，或者如果它不存在则分配一个
	if (inode && (inode->i_state & I_NEW))  // 检查 inode 是否是新分配的
		sysfs_init_inode(sd, inode);  // 初始化新的 inode

	return inode;  // 返回 inode，如果是新分配的，则返回的 inode 处于锁定状态
}

/*
 * The sysfs_dirent serves as both an inode and a directory entry for sysfs.
 * To prevent the sysfs inode numbers from being freed prematurely we take a
 * reference to sysfs_dirent from the sysfs inode.  A
 * super_operations.delete_inode() implementation is needed to drop that
 * reference upon inode destruction.
 */
/*
 * sysfs_dirent 既用作 sysfs 的 inode 也用作目录项。
 * 为了防止 sysfs 的 inode 编号过早被释放，我们从 sysfs inode 中获取一个对 sysfs_dirent 的引用。
 * 需要一个 super_operations.delete_inode() 实现来在 inode 销毁时释放该引用。
 */
void sysfs_delete_inode(struct inode *inode)
{
	struct sysfs_dirent *sd  = inode->i_private; // 从 inode 的私有数据获取 sysfs_dirent

	truncate_inode_pages(&inode->i_data, 0); // 清除 inode 关联的所有页面
	clear_inode(inode); // 清除 inode，准备其销毁
	sysfs_put(sd); // 释放对 sysfs_dirent 的引用
}

// 在 sysfs 中根据名称查找并移除目录项
int sysfs_hash_and_remove(struct sysfs_dirent *dir_sd, const char *name)
{
	struct sysfs_addrm_cxt acxt;  // 地址管理上下文
	struct sysfs_dirent *sd;  // sysfs 目录项指针

	if (!dir_sd)
		return -ENOENT;  // 如果目录项为空，则返回“文件或目录不存在”

	sysfs_addrm_start(&acxt, dir_sd);  // 开始地址管理操作

	sd = sysfs_find_dirent(dir_sd, name);  // 查找指定名称的目录项
	if (sd)
		sysfs_remove_one(&acxt, sd);  // 如果找到了，就移除这个目录项

	sysfs_addrm_finish(&acxt);  // 完成地址管理操作

	if (sd)
		return 0;  // 如果找到并移除了目录项，返回0
	else
		return -ENOENT;  // 如果没有找到目录项，返回“文件或目录不存在”
}

// 检查 inode 的访问权限
int sysfs_permission(struct inode *inode, int mask)
{
	struct sysfs_dirent *sd = inode->i_private;  // 从 inode 私有数据获取 sysfs_dirent

	mutex_lock(&sysfs_mutex);  // 加锁，确保对 inode 的操作是线程安全的
	sysfs_refresh_inode(sd, inode);  // 刷新 inode，确保其属性是最新的
	mutex_unlock(&sysfs_mutex);  // 解锁

	return generic_permission(inode, mask, NULL);  // 调用通用的权限检查函数
}
