/* fs/ internal definitions
 *
 * Copyright (C) 2006 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

struct super_block;  // 前向声明超级块结构体
struct linux_binprm; // 前向声明用于 Linux 二进制程序加载的参数结构体
struct path;         // 前向声明用于文件系统路径的结构体

/*
 * block_dev.c
 */
/*
 * block_dev.c
 * 包含与块设备相关的函数定义和数据结构
 */
#ifdef CONFIG_BLOCK  // 如果配置了块设备支持
extern struct super_block *blockdev_superblock; // 声明一个指向块设备超级块的全局指针
extern void __init bdev_cache_init(void); // 声明块设备缓存初始化函数

// 判断给定的超级块是否为块设备的超级块
static inline int sb_is_blkdev_sb(struct super_block *sb)
{
	return sb == blockdev_superblock; // 如果给定的超级块与块设备超级块相同，则返回真
}

extern int __sync_blockdev(struct block_device *bdev, int wait); // 声明同步块设备的函数

#else  // 如果没有配置块设备支持
// 定义一个空的 bdev_cache_init 函数
static inline void bdev_cache_init(void)
{
}

/*
 * 在 CONFIG_BLOCK 未定义的情况下为相关函数提供默认实现
 */
static inline int sb_is_blkdev_sb(struct super_block *sb)
{
	return 0; // 总是返回 0，表示给定的超级块不是块设备的超级块
}

static inline int __sync_blockdev(struct block_device *bdev, int wait)
{
	return 0; // 总是返回 0，表示没有执行任何同步操作
}
#endif // 结束 CONFIG_BLOCK 相关的定义

/*
 * char_dev.c
 */
/*
 * char_dev.c
 * 包含与字符设备相关的函数定义和数据结构
 */
extern void __init chrdev_init(void); // 声明字符设备初始化函数

/*
 * exec.c
 */
/*
 * exec.c
 * 包含与执行程序相关的函数定义和数据结构
 */
extern int check_unsafe_exec(struct linux_binprm *); // 声明检查执行的安全性的函数

/*
 * namespace.c
 */
/*
 * namespace.c
 * 包含与文件系统命名空间相关的函数定义和数据结构
 */

// 从用户空间复制挂载选项
extern int copy_mount_options(const void __user *, unsigned long *);
// 从用户空间复制挂载字符串
extern int copy_mount_string(const void __user *, char **);

// 释放虚拟文件系统挂载点
extern void free_vfsmnt(struct vfsmount *);
// 分配一个虚拟文件系统挂载点
extern struct vfsmount *alloc_vfsmnt(const char *);
// 在给定的挂载点和目录项上查找挂载
extern struct vfsmount *__lookup_mnt(struct vfsmount *, struct dentry *, int);
// 设置一个挂载点
extern void mnt_set_mountpoint(struct vfsmount *, struct dentry *,
				struct vfsmount *);
// 释放挂载点列表
extern void release_mounts(struct list_head *);
// 卸载挂载树
extern void umount_tree(struct vfsmount *, int, struct list_head *);
// 复制挂载树
extern struct vfsmount *copy_tree(struct vfsmount *, struct dentry *, int);

// 初始化文件系统挂载点管理
extern void __init mnt_init(void);

// 虚拟文件系统挂载点的自旋锁
extern spinlock_t vfsmount_lock;

/*
 * fs_struct.c
 */
/*
 * fs_struct.c
 * 包含与文件系统结构相关的函数定义
 */
// 更新文件系统根目录引用
extern void chroot_fs_refs(struct path *, struct path *);

/*
 * file_table.c
 */
/*
 * file_table.c
 * 包含与文件表操作相关的函数定义
 */
// 将所有文件标记为只读
extern void mark_files_ro(struct super_block *);
// 获取一个空闲的文件结构
extern struct file *get_empty_filp(void);

/*
 * super.c
 */
/*
 * super.c
 * 包含与超级块操作相关的函数定义
 */
// 重新挂载文件系统超级块
extern int do_remount_sb(struct super_block *, int, void *, int);

/*
 * open.c
 */
/*
 * open.c
 * 包含与文件打开操作相关的函数定义
 */
struct nameidata;  // 前向声明，用于处理文件名解析数据结构
// 将 nameidata 结构转换为文件结构
extern struct file *nameidata_to_filp(struct nameidata *);
// 释放打开文件的意图
extern void release_open_intent(struct nameidata *);
