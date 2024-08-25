/*
 * fs/sysfs/symlink.c - operations for initializing and mounting sysfs
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 *
 * This file is released under the GPLv2.
 *
 * Please see Documentation/filesystems/sysfs.txt for more information.
 */

#define DEBUG 

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/magic.h>
#include <linux/slab.h>

#include "sysfs.h"

// sysfs 文件系统的挂载点全局变量
static struct vfsmount *sysfs_mount;
// sysfs 目录项的内存缓存
struct kmem_cache *sysfs_dir_cachep;

// 定义 sysfs 文件系统的操作函数集
static const struct super_operations sysfs_ops = {
	.statfs		= simple_statfs,          // 获取文件系统统计信息的简单实现
	.drop_inode	= generic_delete_inode,   // 标准的 inode 丢弃函数
	.delete_inode	= sysfs_delete_inode,    // 特定于 sysfs 的 inode 删除函数
};

// sysfs 根目录的定义
struct sysfs_dirent sysfs_root = {
	.s_name		= "",                    // 根目录名称为空字符串
	.s_count	= ATOMIC_INIT(1),        // 引用计数初始化为 1
	.s_flags	= SYSFS_DIR,             // 标记为目录类型
	.s_mode		= S_IFDIR | S_IRWXU | S_IRUGO | S_IXUGO, // 目录权限，拥有者具有读写执行权限，其他用户具有读和执行权限
	.s_ino		= 1,                     // inode 编号为 1
};

static int sysfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *inode;    // 用于存储创建的根 inode
	struct dentry *root;    // 用于存储根目录项

	// 设置文件系统的块大小和块大小位数
	sb->s_blocksize = PAGE_CACHE_SIZE;
	sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
	sb->s_magic = SYSFS_MAGIC;  // 文件系统的魔数，用于识别文件系统类型
	sb->s_op = &sysfs_ops;      // 设置文件系统操作函数集
	sb->s_time_gran = 1;        // 设置时间粒度为1，表示支持纳秒级时间戳

	/* get root inode, initialize and unlock it */
	/* 获取根 inode，初始化并解锁 */
	mutex_lock(&sysfs_mutex);  // 加锁以保护 sysfs 的全局状态
	inode = sysfs_get_inode(sb, &sysfs_root);  // 获取根 inode
	mutex_unlock(&sysfs_mutex);  // 解锁
	if (!inode) {  // 检查 inode 是否成功获取
		pr_debug("sysfs: could not get root inode\n");
		return -ENOMEM;  // 如果未能获取 inode，返回内存不足错误
	}

	/* instantiate and link root dentry */
	/* 实例化并链接根目录项 */
	root = d_alloc_root(inode);  // 为根 inode 创建一个目录项
	if (!root) {  // 检查目录项是否成功创建
		pr_debug("%s: could not get root dentry!\n", __func__);
		iput(inode);  // 释放之前获取的 inode
		return -ENOMEM;  // 返回内存不足错误
	}
	root->d_fsdata = &sysfs_root;  // 将目录项的文件系统特定数据指向 sysfs 根目录
	sb->s_root = root;  // 设置超级块的根目录项
	return 0;  // 操作成功，返回 0
}

// 获取 sysfs 文件系统的超级块
static int sysfs_get_sb(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data, struct vfsmount *mnt)
{
	// 使用 get_sb_single 函数，这是为只有一个实例的文件系统（如 sysfs）提供支持的通用函数
	return get_sb_single(fs_type, flags, data, sysfs_fill_super, mnt);
}

// 定义 sysfs 文件系统类型
static struct file_system_type sysfs_fs_type = {
	.name		= "sysfs", // 文件系统的名称
	.get_sb		= sysfs_get_sb, // 函数指针，用于获取文件系统的超级块
	.kill_sb	= kill_anon_super, // 函数指针，用于销毁匿名文件系统的超级块
};

// 初始化 sysfs 文件系统
int __init sysfs_init(void)
{
	int err = -ENOMEM; // 初始错误代码设为内存不足

	// 创建 sysfs 目录项的内存缓存
	sysfs_dir_cachep = kmem_cache_create("sysfs_dir_cache",
					      sizeof(struct sysfs_dirent),
					      0, 0, NULL);
	if (!sysfs_dir_cachep) // 检查内存缓存是否创建成功
		goto out; // 如果失败，跳到函数末尾

	// 初始化 sysfs inode 缓存
	err = sysfs_inode_init();
	if (err) // 检查是否成功
		goto out_err; // 失败则清理已分配资源并退出

	// 注册 sysfs 文件系统
	err = register_filesystem(&sysfs_fs_type);
	if (!err) { // 如果注册成功
		// 尝试挂载 sysfs 文件系统
		sysfs_mount = kern_mount(&sysfs_fs_type);
		if (IS_ERR(sysfs_mount)) { // 检查挂载是否成功
			printk(KERN_ERR "sysfs: could not mount!\n"); // 打印错误信息
			err = PTR_ERR(sysfs_mount); // 获取具体的错误码
			sysfs_mount = NULL; // 清空挂载点变量
			unregister_filesystem(&sysfs_fs_type); // 注销文件系统
			goto out_err; // 跳转到错误处理部分
		}
	} else
		goto out_err; // 注册文件系统失败，处理错误

out:
	return err; // 返回错误码，成功时为 0
out_err:
	kmem_cache_destroy(sysfs_dir_cachep); // 销毁创建的内存缓存
	sysfs_dir_cachep = NULL; // 清空内存缓存指针
	goto out; // 跳转到出口
}

// 取消定义可能之前定义的 sysfs_get 宏，确保可以重新定义 sysfs_get 函数
#undef sysfs_get
// 定义 sysfs_get 函数
struct sysfs_dirent *sysfs_get(struct sysfs_dirent *sd)
{
	return __sysfs_get(sd);  // 调用内部函数 __sysfs_get 增加目录项的引用计数
}
EXPORT_SYMBOL_GPL(sysfs_get);

// 取消定义可能之前定义的 sysfs_put 宏，确保可以重新定义 sysfs_put 函数
#undef sysfs_put
// 定义 sysfs_put 函数
void sysfs_put(struct sysfs_dirent *sd)
{
	__sysfs_put(sd);  // 调用内部函数 __sysfs_put 减少目录项的引用计数
}
EXPORT_SYMBOL_GPL(sysfs_put);
