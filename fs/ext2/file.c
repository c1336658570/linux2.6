/*
 *  linux/fs/ext2/file.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext2 fs regular file handling primitives
 *
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 * 	(jj@sunsite.ms.mff.cuni.cz)
 */

#include <linux/time.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include "ext2.h"
#include "xattr.h"
#include "acl.h"

/*
 * Called when filp is released. This happens when all file descriptors
 * for a single struct file are closed. Note that different open() calls
 * for the same file yield different struct file structures.
 */
/*
 * 当 filp 被释放时调用。这发生在单个 struct file 的所有文件描述符关闭时。
 * 注意，针对同一个文件的不同 open() 调用会产生不同的 struct file 结构体。
 */
static int ext2_release_file (struct inode * inode, struct file * filp)
{
	if (filp->f_mode & FMODE_WRITE) {
		// 锁定截断互斥锁
		mutex_lock(&EXT2_I(inode)->truncate_mutex);
		// 丢弃保留区域
		ext2_discard_reservation(inode);
		// 解锁截断互斥锁
		mutex_unlock(&EXT2_I(inode)->truncate_mutex);
	}
	return 0;
}

int ext2_fsync(struct file *file, struct dentry *dentry, int datasync)
{
	int ret;
	// 获取文件的超级块
	struct super_block *sb = dentry->d_inode->i_sb;
	// 获取块设备的inode的映射
	struct address_space *mapping = sb->s_bdev->bd_inode->i_mapping;

	// 执行一个简单的同步操作
	ret = simple_fsync(file, dentry, datasync);
	// 如果返回了输入输出错误或者检测到映射中有输入输出错误的标志位
	if (ret == -EIO || test_and_clear_bit(AS_EIO, &mapping->flags)) {
		/* We don't really know where the IO error happened... */
		/* 我们并不确切知道 IO 错误发生在何处... */
		// 记录一个 IO 错误，并输出一条错误信息
		ext2_error(sb, __func__,
			   "detected IO error when writing metadata buffers");
		ret = -EIO;	// 将返回值设置为输入输出错误代码
	}
	return ret;	// 返回结果
}

/*
 * We have mostly NULL's here: the current defaults are ok for
 * the ext2 filesystem.
 */
/*
 * 这里大部分是 NULL：当前默认值对于 ext2 文件系统来说是可以的。
 */
const struct file_operations ext2_file_operations = {
	// 通用文件偏移定位
	.llseek		= generic_file_llseek,
	// 同步读
	.read		= do_sync_read,
	// 同步写
	.write		= do_sync_write,
	// 异步 IO 读
	.aio_read	= generic_file_aio_read,
	// 异步 IO 写
	.aio_write	= generic_file_aio_write,
	// ioctl 操作
	.unlocked_ioctl = ext2_ioctl,
#ifdef CONFIG_COMPAT
	// 兼容模式的 ioctl 操作
	.compat_ioctl	= ext2_compat_ioctl,
#endif
	// 文件映射
	.mmap		= generic_file_mmap,
	// 打开文件时处理磁盘配额
	.open		= dquot_file_open,
	// 释放文件
	.release	= ext2_release_file,
	// 文件同步
	.fsync		= ext2_fsync,
	// 利用 splice 进行的读操作
	.splice_read	= generic_file_splice_read,
	// 利用 splice 进行的写操作
	.splice_write	= generic_file_splice_write,
};

#ifdef CONFIG_EXT2_FS_XIP
const struct file_operations ext2_xip_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= xip_file_read,
	.write		= xip_file_write,
	.unlocked_ioctl = ext2_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ext2_compat_ioctl,
#endif
	.mmap		= xip_file_mmap,
	.open		= dquot_file_open,
	.release	= ext2_release_file,
	.fsync		= ext2_fsync,
};
#endif

const struct inode_operations ext2_file_inode_operations = {
	.truncate	= ext2_truncate,
#ifdef CONFIG_EXT2_FS_XATTR
	.setxattr	= generic_setxattr,
	.getxattr	= generic_getxattr,
	.listxattr	= ext2_listxattr,
	.removexattr	= generic_removexattr,
#endif
	.setattr	= ext2_setattr,
	.check_acl	= ext2_check_acl,
	.fiemap		= ext2_fiemap,
};
