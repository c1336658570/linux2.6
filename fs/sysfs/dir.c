/*
 * fs/sysfs/dir.c - sysfs core and dir operation implementation
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

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/namei.h>
#include <linux/idr.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/security.h>
#include "sysfs.h"

// 定义互斥锁sysfs_mutex，用于同步对sysfs结构的访问
DEFINE_MUTEX(sysfs_mutex);
// 定义自旋锁sysfs_assoc_lock，用于管理与sysfs条目关联的锁定
DEFINE_SPINLOCK(sysfs_assoc_lock);

// 定义自旋锁sysfs_ino_lock，用于inode号的管理
static DEFINE_SPINLOCK(sysfs_ino_lock);
// 定义IDA(sysfs_ino_ida)，用于管理inode号的分配
static DEFINE_IDA(sysfs_ino_ida);

/**
 *	sysfs_link_sibling - link sysfs_dirent into sibling list
 *	@sd: sysfs_dirent of interest
 *
 *	Link @sd into its sibling list which starts from
 *	sd->s_parent->s_dir.children.
 *
 *	Locking:
 *	mutex_lock(sysfs_mutex)
 */
/**
 * sysfs_link_sibling - 将sysfs_dirent链接到兄弟列表中
 * @sd: 目标sysfs_dirent
 *
 * 将@sd链接到其兄弟列表中，该列表从sd->s_parent->s_dir.children开始。
 *
 * 锁定：
 * mutex_lock(sysfs_mutex) - 在调用此函数前需要持有sysfs_mutex锁
 */
static void sysfs_link_sibling(struct sysfs_dirent *sd)
{
	struct sysfs_dirent *parent_sd = sd->s_parent;  // 获取父目录项
	struct sysfs_dirent **pos;  // 指向目录项指针的指针，用于遍历兄弟列表

	BUG_ON(sd->s_sibling);  // 断言sd的s_sibling为空，确保未被重复链接

	/* Store directory entries in order by ino.  This allows
	 * readdir to properly restart without having to add a
	 * cursor into the s_dir.children list.
	 */
	/* 按inode编号顺序存储目录项，这允许readdir能够正确重启而无需在s_dir.children列表中添加游标。 */
	for (pos = &parent_sd->s_dir.children; *pos; pos = &(*pos)->s_sibling) {
		if (sd->s_ino < (*pos)->s_ino)  // 如果当前sd的inode小于列表中的inode，则找到插入位置
			break;
	}
	sd->s_sibling = *pos;  // 将sd插入到找到的位置
	*pos = sd;  // 更新指针，将sd设为新的头部
}

/**
 *	sysfs_unlink_sibling - unlink sysfs_dirent from sibling list
 *	@sd: sysfs_dirent of interest
 *
 *	Unlink @sd from its sibling list which starts from
 *	sd->s_parent->s_dir.children.
 *
 *	Locking:
 *	mutex_lock(sysfs_mutex)
 */
/**
 * sysfs_unlink_sibling - 从兄弟列表中解链sysfs_dirent
 * @sd: 感兴趣的sysfs_dirent
 *
 * 从其兄弟列表中解链@sd，该列表从sd->s_parent->s_dir.children开始。
 *
 * 锁定：
 * 在调用此函数之前应持有sysfs_mutex锁。
 */
static void sysfs_unlink_sibling(struct sysfs_dirent *sd)
{
	struct sysfs_dirent **pos;  // 用于遍历兄弟链表的指针

	// 遍历从父目录项的子项列表开始的链表
	for (pos = &sd->s_parent->s_dir.children; *pos; pos = &(*pos)->s_sibling) {
		if (*pos == sd) {  // 找到目标sysfs_dirent
			*pos = sd->s_sibling;  // 将目标sysfs_dirent的父节点指向目标的兄弟节点，从而移除目标节点
			sd->s_sibling = NULL;  // 清除目标sysfs_dirent的兄弟链接
			break;  // 跳出循环
		}
	}
}

/**
 *	sysfs_get_active - get an active reference to sysfs_dirent
 *	@sd: sysfs_dirent to get an active reference to
 *
 *	Get an active reference of @sd.  This function is noop if @sd
 *	is NULL.
 *
 *	RETURNS:
 *	Pointer to @sd on success, NULL on failure.
 */
/**
 *	sysfs_get_active - 获取sysfs_dirent的活跃引用
 *	@sd: 要获取活跃引用的sysfs_dirent
 *
 *	获取@sd的一个活跃引用。如果@sd为NULL，则此函数无操作。
 *
 *	返回值：
 *	成功时返回@sd的指针，失败时返回NULL。
 */
struct sysfs_dirent *sysfs_get_active(struct sysfs_dirent *sd)
{
	if (unlikely(!sd))  // 如果sd为NULL，立即返回NULL，无需进一步操作
		return NULL;

	while (1) {
		int v, t;

		v = atomic_read(&sd->s_active);  // 读取sd的当前活跃引用计数
		if (unlikely(v < 0))  // 如果计数小于0，表明sd已经被移除或不可用，返回NULL
			return NULL;

		t = atomic_cmpxchg(&sd->s_active, v, v + 1);  // 尝试增加活跃引用计数
		if (likely(t == v)) {  // 如果比较和交换操作成功
			rwsem_acquire_read(&sd->dep_map, 0, 1, _RET_IP_);  // 获取读取锁，用于同步
			return sd;  // 返回sd指针
		}
		if (t < 0)  // 如果在尝试修改期间引用计数变为负数，表明sd已被移除
			return NULL;

		cpu_relax();  // 让出CPU，优化紧密循环的性能
	}
}

/**
 *	sysfs_put_active - put an active reference to sysfs_dirent
 *	@sd: sysfs_dirent to put an active reference to
 *
 *	Put an active reference to @sd.  This function is noop if @sd
 *	is NULL.
 */
/**
 *	sysfs_put_active - 释放对sysfs_dirent的活跃引用
 *	@sd: 要释放活跃引用的sysfs_dirent
 *
 *	释放对@sd的活跃引用。如果@sd为NULL，此函数无操作。
 */
void sysfs_put_active(struct sysfs_dirent *sd)
{
	struct completion *cmpl;
	int v;

	if (unlikely(!sd))  // 如果sd为NULL，立即返回，无需进一步操作
		return;

	rwsem_release(&sd->dep_map, 1, _RET_IP_);  // 释放之前获取的读取锁
	v = atomic_dec_return(&sd->s_active);  // 原子地减少活跃引用计数并返回新值
	if (likely(v != SD_DEACTIVATED_BIAS))  // 如果新值不等于已停用偏移量，直接返回
		return;

	/* atomic_dec_return() is a mb(), we'll always see the updated
	 * sd->s_sibling.
	 */
	/* 
	 * atomic_dec_return() 包含一个内存屏障，我们总是可以看到更新后的sd->s_sibling。
	 */
	cmpl = (void *)sd->s_sibling;  // 将s_sibling转换为completion指针
	complete(cmpl);  // 标记cmpl所指向的操作已完成
}

/**
 *	sysfs_deactivate - deactivate sysfs_dirent
 *	@sd: sysfs_dirent to deactivate
 *
 *	Deny new active references and drain existing ones.
 */
/**
 *	sysfs_deactivate - 停用 sysfs_dirent
 *	@sd: 要停用的 sysfs_dirent
 *
 *	拒绝新的活跃引用并排干现有的引用。
 */
// 停用 sysfs_dirent 对象
static void sysfs_deactivate(struct sysfs_dirent *sd)
{
	DECLARE_COMPLETION_ONSTACK(wait);  // 在栈上声明并初始化一个完成变量
	int v;

	BUG_ON(sd->s_sibling || !(sd->s_flags & SYSFS_FLAG_REMOVED));  // 检查状态标志以确保 sd 是要被移除的且没有兄弟节点

	if (!(sysfs_type(sd) & SYSFS_ACTIVE_REF))  // 如果 sd 类型不包括需要活跃引用的类型，直接返回
		return;

	sd->s_sibling = (void *)&wait;  // 使用完成变量的地址临时存储到 s_sibling，以便在 sysfs_put_active 中使用

	rwsem_acquire(&sd->dep_map, 0, 0, _RET_IP_);  // 获取依赖映射的读写信号量
	/* atomic_add_return() is a mb(), put_active() will always see
	 * the updated sd->s_sibling.
	 */
	/* atomic_add_return() 是一个带有内存屏障的函数，put_active() 总会看到更新后的 sd->s_sibling。 */
	v = atomic_add_return(SD_DEACTIVATED_BIAS, &sd->s_active);  // 原子增加停用偏移并获取新值

	if (v != SD_DEACTIVATED_BIAS) {  // 如果当前的活跃计数不等于停用偏移，等待其他线程释放它们的引用
		lock_contended(&sd->dep_map, _RET_IP_);  // 标记锁冲突，可能等待解锁
		wait_for_completion(&wait);  // 等待所有活跃引用被释放
	}

	sd->s_sibling = NULL;  // 清除临时存储的完成变量地址

	lock_acquired(&sd->dep_map, _RET_IP_);  // 标记锁已被获取
	rwsem_release(&sd->dep_map, 1, _RET_IP_);  // 释放依赖映射的读写信号量
}

// 为 sysfs 文件系统中的目录项分配一个唯一的 inode 号（ino_t 类型）
static int sysfs_alloc_ino(ino_t *pino)
{
	int ino, rc;  // 定义变量 ino 用于存储分配的 inode 号，rc 用于存储返回状态码

 retry:  // 标签，用于在资源不足时重试分配
	spin_lock(&sysfs_ino_lock);  // 加锁保护 inode 分配过程，防止并发冲突
	rc = ida_get_new_above(&sysfs_ino_ida, 2, &ino);  // 尝试从 IDA 管理器获取一个大于等于 2 的新 inode 号
	spin_unlock(&sysfs_ino_lock);  // 解锁

	if (rc == -EAGAIN) {  // 如果返回 -EAGAIN，表示当前无可用的编号，需要扩充 IDA 空间后重试
		if (ida_pre_get(&sysfs_ino_ida, GFP_KERNEL))  // 尝试预分配 IDA 资源
			goto retry;  // 预分配成功，跳转到 retry 重新尝试分配 inode 号
		rc = -ENOMEM;  // 预分配失败，返回内存不足错误
	}

	*pino = ino;  // 将分配的 inode 号存储到调用者提供的地址中
	return rc;  // 返回操作的状态码
}

// 释放已分配的 inode 号
static void sysfs_free_ino(ino_t ino)
{
	spin_lock(&sysfs_ino_lock);  // 加锁，确保对 IDA 的操作是线程安全的
	ida_remove(&sysfs_ino_ida, ino);  // 从 IDA 中移除指定的 inode 号
	spin_unlock(&sysfs_ino_lock);  // 解锁
}

// 处理与 sysfs 文件系统条目（sysfs_dirent）的资源释放和删除状态检查相关的操作
void release_sysfs_dirent(struct sysfs_dirent *sd)
{
	struct sysfs_dirent *parent_sd;
repeat:
	/* Moving/renaming is always done while holding reference.
	 * sd->s_parent won't change beneath us.
	 */
	parent_sd = sd->s_parent;  // 获取当前目录条目的父目录条目

	if (sysfs_type(sd) == SYSFS_KOBJ_LINK)
		sysfs_put(sd->s_symlink.target_sd);  // 如果是链接类型，释放链接目标的引用
	if (sysfs_type(sd) & SYSFS_COPY_NAME)
		kfree(sd->s_name);  // 如果名字是复制的，释放名字内存
	if (sd->s_iattr && sd->s_iattr->ia_secdata)
		security_release_secctx(sd->s_iattr->ia_secdata,  // 释放安全上下文信息
														sd->s_iattr->ia_secdata_len);
	kfree(sd->s_iattr);  // 释放 inode 属性结构体
	sysfs_free_ino(sd->s_ino);  // 释放 inode 编号
	kmem_cache_free(sysfs_dir_cachep, sd);  // 释放 sysfs_dirent 结构体到缓存

	sd = parent_sd;  // 转到父目录项
	if (sd && atomic_dec_and_test(&sd->s_count))  // 如果父目录的引用计数减到 0
		goto repeat;  // 重复处理
}

// 检查目录条目是否已被标记为删除。如果是，函数返回 1（真），否则返回 0（假）。
static int sysfs_dentry_delete(struct dentry *dentry)
{
	struct sysfs_dirent *sd = dentry->d_fsdata;
	return !!(sd->s_flags & SYSFS_FLAG_REMOVED);  // 检查目录条目是否已被标记为删除
}

// 用于重新验证一个目录项（dentry）以确定它是否仍有效。
static int sysfs_dentry_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	struct sysfs_dirent *sd = dentry->d_fsdata;  // 获取与目录项相关联的 sysfs_dirent
	int is_dir;

	mutex_lock(&sysfs_mutex);  // 锁定 sysfs 互斥锁以保护数据结构

	/* The sysfs dirent has been deleted */
	/* 如果 sysfs dirent 已被删除 */
	if (sd->s_flags & SYSFS_FLAG_REMOVED)
		goto out_bad;  // 如果已标记为删除，则跳转到 out_bad 处理

	/* The sysfs dirent has been moved? */
	/* 如果 sysfs dirent 被移动了？ */
	if (dentry->d_parent->d_fsdata != sd->s_parent)
		goto out_bad;  // 如果父目录项不匹配，则跳转到 out_bad 处理

	/* The sysfs dirent has been renamed */
	/* 如果 sysfs dirent 被重命名 */
	if (strcmp(dentry->d_name.name, sd->s_name) != 0)
		goto out_bad;  // 如果名字不匹配，则跳转到 out_bad 处理

	mutex_unlock(&sysfs_mutex);  // 解锁
out_valid:
	return 1;  // 返回 1 表示目录项有效

out_bad:
	/* Remove the dentry from the dcache hashes.
	 * If this is a deleted dentry we use d_drop instead of d_delete
	 * so sysfs doesn't need to cope with negative dentries.
	 *
	 * If this is a dentry that has simply been renamed we
	 * use d_drop to remove it from the dcache lookup on its
	 * old parent.  If this dentry persists later when a lookup
	 * is performed at its new name the dentry will be readded
	 * to the dcache hashes.
	 */
	/* 从dcache哈希表中移除dentry。
	 * 如果这是一个已删除的dentry，我们使用d_drop而不是d_delete，
	 * 这样sysfs就不需要处理负dentry。
	 *
	 * 如果这是一个仅仅被重命名的dentry，
	 * 我们使用d_drop从其旧父目录的dcache查找中移除它。
	 * 如果稍后在其新名称下执行查找时，这个dentry将被重新添加到dcache哈希表中。
	 */
	/* 从 dcache 哈希中删除目录项 */
	is_dir = (sysfs_type(sd) == SYSFS_DIR);  // 检查是否为目录
	mutex_unlock(&sysfs_mutex);  // 解锁
	if (is_dir) {
		/* If we have submounts we must allow the vfs caches
		 * to lie about the state of the filesystem to prevent
		 * leaks and other nasty things.
		 */
		/* 如果我们有子挂载点，我们必须允许VFS缓存对文件系统的状态进行误报，
		 * 以防止泄露和其他不良问题。
		 */
		/* 如果有子挂载点，我们必须允许 vfs 缓存对文件系统状态撒谎，以防止泄露和其他严重问题。 */
		if (have_submounts(dentry))
			goto out_valid;  // 如果有子挂载点，跳转到 out_valid 处理
		shrink_dcache_parent(dentry);  // 缩减父目录项的 dcache
	}
	d_drop(dentry);  // 从 dcache 删除目录项
	return 0;  // 返回 0 表示目录项无效
}

// 用于管理 sysfs 文件系统目录项（dentry）的操作。具体操作包括如何重新验证、删除和释放 inode 资源。
static void sysfs_dentry_iput(struct dentry *dentry, struct inode *inode)
{
	struct sysfs_dirent * sd = dentry->d_fsdata;  // 从 dentry 中获取与之关联的 sysfs_dirent

	sysfs_put(sd);  // 释放对 sysfs_dirent 的引用
	iput(inode);    // 释放对 inode 的引用
}

// dentry_operations 结构，用于定义 sysfs 文件系统中目录项的各种操作。
static const struct dentry_operations sysfs_dentry_ops = {
	.d_revalidate	= sysfs_dentry_revalidate,  // 指定重新验证目录项的函数
	.d_delete	= sysfs_dentry_delete,      // 指定删除目录项的函数
	.d_iput		= sysfs_dentry_iput,        // 指定释放 inode 的函数
};

// sysfs_new_dirent函数用于创建一个新的sysfs目录项
struct sysfs_dirent *sysfs_new_dirent(const char *name, umode_t mode, int type)
{
	char *dup_name = NULL;	// 用于复制名称的临时指针
	struct sysfs_dirent *sd;	// 声明一个sysfs_dirent结构体指针

	// 如果type标志包含SYSFS_COPY_NAME，则复制name字符串
	if (type & SYSFS_COPY_NAME) {
		// 使用内核内存分配并复制名称
		name = dup_name = kstrdup(name, GFP_KERNEL);
		if (!name)	// 如果内存分配失败
			return NULL;	// 返回NULL
	}

	// 使用内存缓存分配一个sysfs_dirent结构体
	sd = kmem_cache_zalloc(sysfs_dir_cachep, GFP_KERNEL);
	if (!sd)	// 如果内存分配失败
		goto err_out1;	// 跳转到错误处理代码1

	 // 为sysfs目录项分配一个inode编号
	if (sysfs_alloc_ino(&sd->s_ino))
		goto err_out2;	// 如果分配失败，跳转到错误处理代码2

	// 初始化目录项引用计数和活动状态计数
	atomic_set(&sd->s_count, 1);
	atomic_set(&sd->s_active, 0);

	// 设置目录项的名称、模式和标志
	sd->s_name = name;
	sd->s_mode = mode;
	sd->s_flags = type;

	return sd;	// 返回新创建的目录项指针

 err_out2:
	kmem_cache_free(sysfs_dir_cachep, sd);	// 释放已分配的内存
 err_out1:
	kfree(dup_name);	// 释放复制的名称
	return NULL;	// 返回NULL
}

/**
 *	sysfs_addrm_start - prepare for sysfs_dirent add/remove
 *	@acxt: pointer to sysfs_addrm_cxt to be used
 *	@parent_sd: parent sysfs_dirent
 *
 *	This function is called when the caller is about to add or
 *	remove sysfs_dirent under @parent_sd.  This function acquires
 *	sysfs_mutex.  @acxt is used to keep and pass context to
 *	other addrm functions.
 *
 *	LOCKING:
 *	Kernel thread context (may sleep).  sysfs_mutex is locked on
 *	return.
 */
/**
 *	sysfs_addrm_start - 准备添加或移除sysfs_dirent
 *	@acxt: 将要使用的sysfs_addrm_cxt的指针
 *	@parent_sd: 父sysfs_dirent
 *
 *	当调用者即将在@parent_sd下添加或移除sysfs_dirent时，调用此函数。
 *	此函数获取sysfs_mutex。@acxt用于保存并传递上下文到其他addrm函数。
 *
 *	LOCKING:
 *	内核线程上下文（可能会睡眠）。返回时，sysfs_mutex被锁定。
 */
void sysfs_addrm_start(struct sysfs_addrm_cxt *acxt,
		       struct sysfs_dirent *parent_sd)
{
	memset(acxt, 0, sizeof(*acxt));	// 清零acxt结构体，准备填入新的数据
	acxt->parent_sd = parent_sd;	// 设置父目录项

	mutex_lock(&sysfs_mutex);	// 锁定sysfs的全局互斥体
}

/**
 *	__sysfs_add_one - add sysfs_dirent to parent without warning
 *	@acxt: addrm context to use
 *	@sd: sysfs_dirent to be added
 *
 *	Get @acxt->parent_sd and set sd->s_parent to it and increment
 *	nlink of parent inode if @sd is a directory and link into the
 *	children list of the parent.
 *
 *	This function should be called between calls to
 *	sysfs_addrm_start() and sysfs_addrm_finish() and should be
 *	passed the same @acxt as passed to sysfs_addrm_start().
 *
 *	LOCKING:
 *	Determined by sysfs_addrm_start().
 *
 *	RETURNS:
 *	0 on success, -EEXIST if entry with the given name already
 *	exists.
 */
/**
 *	__sysfs_add_one - 在不发出警告的情况下将sysfs_dirent添加到父目录
 *	@acxt: 使用的addrm上下文
 *	@sd: 要添加的sysfs_dirent
 *
 *	获取@acxt->parent_sd并将sd->s_parent设置为它，并且如果@sd是一个目录，
 *	则增加父节点的nlink，并将其链接到父目录的子列表中。
 *
 *	此函数应该在sysfs_addrm_start()和sysfs_addrm_finish()调用之间被调用，
 *	并应传递与传给sysfs_addrm_start()相同的@acxt。
 *
 *	LOCKING:
 *	由sysfs_addrm_start()确定。
 *
 *	RETURNS:
 *	成功时返回0，如果给定名称的条目已存在，则返回-EEXIST。
 */
int __sysfs_add_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd)
{
	struct sysfs_inode_attrs *ps_iattr;

	// 如果父目录中已存在同名条目，则返回-EEXIST
	if (sysfs_find_dirent(acxt->parent_sd, sd->s_name))
		return -EEXIST;

	// 获取父目录项的引用并设置为sd的父目录
	sd->s_parent = sysfs_get(acxt->parent_sd);

	// 将sd链接到其兄弟节点链表中
	sysfs_link_sibling(sd);

	/* Update timestamps on the parent */
	/* 更新父目录的时间戳 */
	ps_iattr = acxt->parent_sd->s_iattr;
	if (ps_iattr) {
		struct iattr *ps_iattrs = &ps_iattr->ia_iattr;
		// 设置当前时间为修改和创建时间
		ps_iattrs->ia_ctime = ps_iattrs->ia_mtime = CURRENT_TIME;
	}

	return 0;
}

/**
 *	sysfs_pathname - return full path to sysfs dirent
 *	@sd: sysfs_dirent whose path we want
 *	@path: caller allocated buffer
 *
 *	Gives the name "/" to the sysfs_root entry; any path returned
 *	is relative to wherever sysfs is mounted.
 *
 *	XXX: does no error checking on @path size
 */
/**
 *	sysfs_pathname - 返回sysfs dirent的完整路径
 *	@sd: 我们想要获取路径的sysfs_dirent
 *	@path: 调用者分配的缓冲区
 *
 *	将名字“/”赋予sysfs_root条目；返回的任何路径都是相对于sysfs挂载位置的相对路径。
 *
 *	XXX: 对@path大小没有进行错误检查
 */
static char *sysfs_pathname(struct sysfs_dirent *sd, char *path)
{
	if (sd->s_parent) {	// 如果有父目录
		// 递归调用以获取父目录的路径
		sysfs_pathname(sd->s_parent, path);
		strcat(path, "/");	// 在路径后加上分隔符"/"
	}
	// 将当前目录项的名称追加到路径上
	strcat(path, sd->s_name);
	return path;	// 返回构建的完整路径
}

/**
 *	sysfs_add_one - add sysfs_dirent to parent
 *	@acxt: addrm context to use
 *	@sd: sysfs_dirent to be added
 *
 *	Get @acxt->parent_sd and set sd->s_parent to it and increment
 *	nlink of parent inode if @sd is a directory and link into the
 *	children list of the parent.
 *
 *	This function should be called between calls to
 *	sysfs_addrm_start() and sysfs_addrm_finish() and should be
 *	passed the same @acxt as passed to sysfs_addrm_start().
 *
 *	LOCKING:
 *	Determined by sysfs_addrm_start().
 *
 *	RETURNS:
 *	0 on success, -EEXIST if entry with the given name already
 *	exists.
 */
/**
 *	sysfs_add_one - 向父目录添加sysfs_dirent
 *	@acxt: 用于操作的addrm上下文
 *	@sd: 要添加的sysfs_dirent
 *
 *	获取@acxt->parent_sd并将其设置为sd->s_parent，如果sd是目录，则增加父节点inode的nlink，
 *	并将其链接到父目录的子列表中。
 *
 *	此函数应在sysfs_addrm_start()和sysfs_addrm_finish()调用之间被调用，并且应传递与
 *	sysfs_addrm_start()相同的@acxt。
 *
 *	锁定:
 *	由sysfs_addrm_start()确定。
 *
 *	返回:
 *	成功时返回0，如果给定名称的条目已存在则返回-EEXIST。
 */
int sysfs_add_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd)
{
	int ret;

	// 尝试添加dirent
	ret = __sysfs_add_one(acxt, sd);
	if (ret == -EEXIST) {	 // 如果dirent已经存在
	 	// 分配内存以保存路径
		char *path = kzalloc(PATH_MAX, GFP_KERNEL);
		// 输出警告信息
		WARN(1, KERN_WARNING
		     "sysfs: cannot create duplicate filename '%s'\n",
		     (path == NULL) ? sd->s_name :	// 如果内存分配失败，只显示名字
		     strcat(strcat(sysfs_pathname(acxt->parent_sd, path), "/"),
		            sd->s_name));	// 如果内存分配成功，显示完整路径
		kfree(path);	// 释放分配的内存
	}

	return ret;	// 返回结果
}

/**
 *	sysfs_remove_one - remove sysfs_dirent from parent
 *	@acxt: addrm context to use
 *	@sd: sysfs_dirent to be removed
 *
 *	Mark @sd removed and drop nlink of parent inode if @sd is a
 *	directory.  @sd is unlinked from the children list.
 *
 *	This function should be called between calls to
 *	sysfs_addrm_start() and sysfs_addrm_finish() and should be
 *	passed the same @acxt as passed to sysfs_addrm_start().
 *
 *	LOCKING:
 *	Determined by sysfs_addrm_start().
 */
/**
 *	sysfs_remove_one - 从父目录中移除sysfs_dirent
 *	@acxt: 用于操作的addrm上下文
 *	@sd: 要移除的sysfs_dirent
 *
 *	如果@sd是目录，则标记@sd为已移除并减少父节点inode的nlink。
 *	@sd从子列表中断开链接。
 *
 *	此函数应在sysfs_addrm_start()和sysfs_addrm_finish()调用之间被调用，
 *	并且应传递与sysfs_addrm_start()相同的@acxt。
 *
 *	锁定:
 *	由sysfs_addrm_start()确定。
 */
void sysfs_remove_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd)
{
	struct sysfs_inode_attrs *ps_iattr;

	// 断言sd未被标记为已移除
	BUG_ON(sd->s_flags & SYSFS_FLAG_REMOVED);

	// 从其父目录的子列表中断开sd的链接
	sysfs_unlink_sibling(sd);

	/* Update timestamps on the parent */
	/* 更新父目录的时间戳 */
	ps_iattr = acxt->parent_sd->s_iattr;
	if (ps_iattr) {	// 如果父目录具有inode属性
		struct iattr *ps_iattrs = &ps_iattr->ia_iattr;
		// 更新ctime和mtime为当前时间
		ps_iattrs->ia_ctime = ps_iattrs->ia_mtime = CURRENT_TIME;
	}

	// 标记sd为已移除
	sd->s_flags |= SYSFS_FLAG_REMOVED;
	// 将sd添加到已移除的链表中
	sd->s_sibling = acxt->removed;
	// 更新addrm上下文的removed指针
	acxt->removed = sd;
}

/**
 *	sysfs_addrm_finish - finish up sysfs_dirent add/remove
 *	@acxt: addrm context to finish up
 *
 *	Finish up sysfs_dirent add/remove.  Resources acquired by
 *	sysfs_addrm_start() are released and removed sysfs_dirents are
 *	cleaned up.
 *
 *	LOCKING:
 *	sysfs_mutex is released.
 */
/**
 *	sysfs_addrm_finish - 完成sysfs_dirent的添加或移除
 *	@acxt: 要完成的addrm上下文
 *
 *	完成对sysfs_dirent的添加或移除操作。释放由sysfs_addrm_start()获取的资源，
 *	并清理已移除的sysfs_dirents。
 *
 *	锁定:
 *	释放sysfs_mutex。
 */
void sysfs_addrm_finish(struct sysfs_addrm_cxt *acxt)
{
	/* release resources acquired by sysfs_addrm_start() */
	/* 释放由sysfs_addrm_start()获取的资源 */
	mutex_unlock(&sysfs_mutex);	// 释放之前在sysfs_addrm_start()中获取的互斥锁

	/* kill removed sysfs_dirents */
	/* 清理已移除的sysfs_dirents */
	while (acxt->removed) {	// 遍历已移除的目录项列表
	// 取出列表中的第一个元素
		struct sysfs_dirent *sd = acxt->removed;

		// 更新列表头指针到下一个元素
		acxt->removed = sd->s_sibling;
		// 清除当前目录项的s_sibling指针，断开与链表的链接
		sd->s_sibling = NULL;

		// 取消激活目录项，使其不再可访问
		sysfs_deactivate(sd);
		// 如果目录项与二进制文件相关联，则取消映射
		unmap_bin_file(sd);
		// 减少目录项的引用计数，可能会导致其释放
		sysfs_put(sd);
	}
}

/**
 *	sysfs_find_dirent - find sysfs_dirent with the given name
 *	@parent_sd: sysfs_dirent to search under
 *	@name: name to look for
 *
 *	Look for sysfs_dirent with name @name under @parent_sd.
 *
 *	LOCKING:
 *	mutex_lock(sysfs_mutex)
 *
 *	RETURNS:
 *	Pointer to sysfs_dirent if found, NULL if not.
 */
/**
 * sysfs_find_dirent - 在给定名称下查找 sysfs_dirent
 * @parent_sd: 要搜索的父 sysfs_dirent
 * @name: 要查找的名称
 *
 * 在 @parent_sd 下查找名称为 @name 的 sysfs_dirent。
 *
 * 锁定:
 * mutex_lock(sysfs_mutex) 
 *
 * 返回:
 * 如果找到，则返回指向 sysfs_dirent 的指针；如果未找到，则返回 NULL。
 */
struct sysfs_dirent *sysfs_find_dirent(struct sysfs_dirent *parent_sd,
				       const unsigned char *name)
{
	struct sysfs_dirent *sd;

	// 遍历父目录项的子目录链表	
	for (sd = parent_sd->s_dir.children; sd; sd = sd->s_sibling)	// 从父目录项的子目录列表开始，遍历所有子目录项。
		if (!strcmp(sd->s_name, name))	// 检查当前目录项的名称是否与搜索名称匹配。
			return sd;	// 返回该目录项
	return NULL;	// 如果在循环中没有找到匹配的目录项，则函数返回 NULL。
}

/**
 *	sysfs_get_dirent - find and get sysfs_dirent with the given name
 *	@parent_sd: sysfs_dirent to search under
 *	@name: name to look for
 *
 *	Look for sysfs_dirent with name @name under @parent_sd and get
 *	it if found.
 *
 *	LOCKING:
 *	Kernel thread context (may sleep).  Grabs sysfs_mutex.
 *
 *	RETURNS:
 *	Pointer to sysfs_dirent if found, NULL if not.
 */
/**
 *	sysfs_get_dirent - 查找并获取给定名称的sysfs_dirent
 *	@parent_sd: 要搜索的sysfs_dirent
 *	@name: 要查找的名称
 *
 *	在@parent_sd下查找名为@name的sysfs_dirent，并在找到时获取它。
 *
 *	锁定:
 *	内核线程上下文（可能会休眠）。会获取sysfs_mutex。
 *
 *	返回:
 *	如果找到，返回指向sysfs_dirent的指针；如果没有找到，返回NULL。
 */
struct sysfs_dirent *sysfs_get_dirent(struct sysfs_dirent *parent_sd,
				      const unsigned char *name)
{
	struct sysfs_dirent *sd;

	// 锁定sysfs_mutex以保护对sysfs_dirent结构的并发访问
	mutex_lock(&sysfs_mutex);
	// 查找名为name的sysfs_dirent
	sd = sysfs_find_dirent(parent_sd, name);
	// 如果找到，增加对找到的dirent的引用计数
	sysfs_get(sd);
	// 解锁mutex
	mutex_unlock(&sysfs_mutex);

	// 返回找到的dirent，或者如果没有找到，则返回NULL
	return sd;
}
EXPORT_SYMBOL_GPL(sysfs_get_dirent);

/**
 * create_dir - 创建一个目录
 * @kobj: 关联的kobject对象
 * @parent_sd: 父目录的sysfs_dirent
 * @name: 新目录的名称
 * @p_sd: 新创建的目录项的指针的地址
 *
 * 在sysfs中为给定的kobject创建一个新目录。
 * 
 * 返回:
 * 成功时返回0，失败时返回负数错误码。
 */
static int create_dir(struct kobject *kobj, struct sysfs_dirent *parent_sd,
		      const char *name, struct sysfs_dirent **p_sd)
{
	umode_t mode = S_IFDIR | S_IRWXU | S_IRUGO | S_IXUGO; // 目录的权限设置为可读可写可执行
	struct sysfs_addrm_cxt acxt; // 地址管理上下文
	struct sysfs_dirent *sd; // 新目录的sysfs_dirent
	int rc;

	/* allocate */
	// 分配一个新的sysfs_dirent
	sd = sysfs_new_dirent(name, mode, SYSFS_DIR);
	if (!sd) // 如果分配失败
		return -ENOMEM; // 返回内存不足错误

	sd->s_dir.kobj = kobj; // 设置sysfs_dirent的kobject

	/* link in */
	// 开始链接操作
	sysfs_addrm_start(&acxt, parent_sd); // 开始添加或移除操作
	rc = sysfs_add_one(&acxt, sd); // 将新目录项添加到父目录中
	sysfs_addrm_finish(&acxt); // 完成添加或移除操作

	if (rc == 0) // 如果添加成功
		*p_sd = sd; // 设置调用者的指针指向新目录项
	else
		sysfs_put(sd); // 如果添加失败，释放目录项

	return rc; // 返回结果
}

/**
 * sysfs_create_subdir - 在sysfs中为kobject创建子目录
 * @kobj: 目标kobject
 * @name: 子目录的名称
 * @p_sd: 创建的目录项的指针的地址
 *
 * 返回:
 * 成功时返回0，失败时返回负数错误码。
 */
int sysfs_create_subdir(struct kobject *kobj, const char *name,
			struct sysfs_dirent **p_sd)
{
	// 调用create_dir创建目录
	return create_dir(kobj, kobj->sd, name, p_sd);
}

/**
 *	sysfs_create_dir - create a directory for an object.
 *	@kobj:		object we're creating directory for. 
 */
/**
 *	sysfs_create_dir - 为对象创建一个目录。
 *	@kobj: 我们为其创建目录的对象。
 *
 *	此函数为指定的kobject在sysfs中创建一个目录。
 *	如果kobject有一个父对象，新目录将在父目录下创建；
 *	否则，它将在sysfs的根目录下创建。
 *
 *	成功时返回0，失败时返回错误码。
 */
int sysfs_create_dir(struct kobject *kobj)
{
	struct sysfs_dirent *parent_sd, *sd; // 父目录项和新目录项的指针
	int error = 0; // 错误码初始化为0

	BUG_ON(!kobj); // 如果kobj为空，则中断

	// 确定新目录的父目录
	if (kobj->parent)
		parent_sd = kobj->parent->sd; // 如果有父对象，使用父对象的目录项
	else
		parent_sd = &sysfs_root; // 如果没有父对象，使用sysfs的根目录

	// 创建目录
	error = create_dir(kobj, parent_sd, kobject_name(kobj), &sd);
	if (!error) // 如果创建成功
		kobj->sd = sd; // 更新kobj的目录项指针

	return error; // 返回操作结果
}

/**
 *	sysfs_lookup - 在目录中查找sysfs条目。
 *	@dir: 父目录inode
 *	@dentry: 要查找的dentry
 *	@nd: nameidata（未使用）
 *
 *	在由@dir和@dentry指定的目录中查找sysfs条目。
 *	如果找到，将inode附加到dentry上。如果条目不存在，返回错误。
 *
 *	返回找到的条目的dentry或错误指针。
 */
static struct dentry * sysfs_lookup(struct inode *dir, struct dentry *dentry,
				struct nameidata *nd)
{
	struct dentry *ret = NULL; // 函数返回的dentry或错误指针
	struct sysfs_dirent *parent_sd = dentry->d_parent->d_fsdata; // 获取父目录的sysfs_dirent
	struct sysfs_dirent *sd; // 目标sysfs_dirent
	struct inode *inode; // 目标inode

	mutex_lock(&sysfs_mutex); // 锁定sysfs

	sd = sysfs_find_dirent(parent_sd, dentry->d_name.name); // 查找名称匹配的sysfs_dirent

	/* no such entry */
	if (!sd) { // 如果没有找到
		ret = ERR_PTR(-ENOENT); // 设置错误指针为-ENOENT
		goto out_unlock; // 跳到解锁部分
	}

	/* attach dentry and inode */
	inode = sysfs_get_inode(dir->i_sb, sd); // 获取inode
	if (!inode) { // 如果inode获取失败
		ret = ERR_PTR(-ENOMEM); // 设置错误指针为-ENOMEM
		goto out_unlock; // 跳到解锁部分
	}

	/* instantiate and hash dentry */
	ret = d_find_alias(inode); // 查找inode的别名dentry
	if (!ret) { // 如果没有找到别名
		dentry->d_op = &sysfs_dentry_ops; // 设置dentry操作
		dentry->d_fsdata = sysfs_get(sd); // 设置dentry的fsdata
		d_add(dentry, inode); // 添加并hash dentry
	} else {
		d_move(ret, dentry); // 移动别名到当前dentry
		iput(inode); // 释放inode
	}

 out_unlock:
	mutex_unlock(&sysfs_mutex); // 解锁sysfs
	return ret; // 返回结果
}

/**
 *	sysfs_dir_inode_operations - sysfs目录的inode操作
 *
 *	.lookup: 在目录中查找目录项的函数。
 *	.permission: 检查访问权限。
 *	.setattr: 设置文件的属性。
 *	.getattr: 获取文件的属性。
 *	.setxattr: 设置文件的扩展属性。
 */
/**
 * sysfs_dir_inode_operations - 定义sysfs目录inode的操作
 * 这里包含了sysfs目录类型inode的各种文件系统操作函数。
 */
const struct inode_operations sysfs_dir_inode_operations = {
	.lookup		= sysfs_lookup,      // 查找文件或目录
	.permission	= sysfs_permission,  // 检查访问权限
	.setattr	= sysfs_setattr,     // 设置属性
	.getattr	= sysfs_getattr,     // 获取属性
	.setxattr	= sysfs_setxattr,    // 设置扩展属性
};

/**
 * remove_dir - 移除一个sysfs目录
 * @sd: 要移除的sysfs_dirent目录
 * 
 * 该函数负责启动删除过程，调用相关函数移除目录，然后完成删除过程。
 */
static void remove_dir(struct sysfs_dirent *sd)
{
	struct sysfs_addrm_cxt acxt;  // 定义一个添加/删除上下文

	sysfs_addrm_start(&acxt, sd->s_parent);  // 开始一个添加/删除操作
	sysfs_remove_one(&acxt, sd);             // 移除一个sysfs_dirent
	sysfs_addrm_finish(&acxt);               // 完成添加/删除操作
}

/**
 *	sysfs_remove_subdir - 公开函数，用于从sysfs中移除一个子目录
 *	@sd: 代表要移除的目录的sysfs_dirent
 *
 *	一个调用remove_dir以移除目录的封装函数。
 */
void sysfs_remove_subdir(struct sysfs_dirent *sd)
{
	remove_dir(sd); // 调用remove_dir函数来执行移除操作
}

/**
 * __sysfs_remove_dir - 移除sysfs目录及其所有子项
 * @dir_sd: 指向要移除的目录的sysfs_dirent结构体的指针
 *
 * 这个函数移除一个sysfs目录以及所有的非目录子节点。它首先初始化一个移除上下文，
 * 然后遍历目录下的所有子节点。如果子节点是普通文件或链接，它会使用 sysfs_remove_one 函数移除它。
 * 如果子节点是目录，它会跳过（不递归处理），直到处理完所有非目录节点。
 * 最后，该函数使用 remove_dir 函数移除整个目录。
 */
static void __sysfs_remove_dir(struct sysfs_dirent *dir_sd)
{
	struct sysfs_addrm_cxt acxt;  // 移除操作的上下文
	struct sysfs_dirent **pos;    // 子节点的位置指针

	if (!dir_sd)
		return;  // 如果目录不存在，直接返回

	pr_debug("sysfs %s: removing dir\n", dir_sd->s_name);  // 打印调试信息，表明正在移除的目录
	sysfs_addrm_start(&acxt, dir_sd);  // 开始一个移除操作
	pos = &dir_sd->s_dir.children;  // 获取目录的子节点列表的头部
	while (*pos) {  // 遍历所有子节点
		struct sysfs_dirent *sd = *pos;  // 当前处理的子节点

		if (sysfs_type(sd) != SYSFS_DIR)  // 如果当前子节点不是目录
			sysfs_remove_one(&acxt, sd);  // 移除这个子节点
		else
			pos = &(*pos)->s_sibling;  // 如果是目录，跳到下一个兄弟节点
	}
	sysfs_addrm_finish(&acxt);  // 结束移除操作

	remove_dir(dir_sd);  // 移除目录本身
}

/**
 *	sysfs_remove_dir - remove an object's directory.
 *	@kobj:	object.
 *
 *	The only thing special about this is that we remove any files in
 *	the directory before we remove the directory, and we've inlined
 *	what used to be sysfs_rmdir() below, instead of calling separately.
 */
/**
 * sysfs_remove_dir - 移除一个对象的目录。
 * @kobj: 对象。
 *
 * 这个函数的特殊之处在于它会在移除目录之前先移除目录中的所有文件，
 * 并且已经将以前的 sysfs_rmdir() 函数的内容内联在下面，而不是分开调用。
 */
void sysfs_remove_dir(struct kobject *kobj)
{
	struct sysfs_dirent *sd = kobj->sd; // 获取与kobject关联的sysfs_dirent结构

	spin_lock(&sysfs_assoc_lock); // 获取关联锁以保护对kobj结构的修改
	kobj->sd = NULL; // 清除kobject结构中对sysfs_dirent的引用
	spin_unlock(&sysfs_assoc_lock); // 释放锁

	__sysfs_remove_dir(sd); // 调用__sysfs_remove_dir函数移除目录及其内容
}

/**
 * sysfs_rename - 重命名sysfs目录项。
 * @sd: 要重命名的目录项。
 * @new_parent_sd: 新的父目录项。
 * @new_name: 新名称。
 *
 * 重命名sysfs目录项，包括更新名称和可能更改其父目录。
 */
int sysfs_rename(struct sysfs_dirent *sd,
                 struct sysfs_dirent *new_parent_sd, const char *new_name)
{
	const char *dup_name = NULL;
	int error;

	mutex_lock(&sysfs_mutex); // 锁定sysfs互斥锁以保护对目录项的修改

	error = 0;
	// 如果父目录和名称都未改变，则无需操作
	if ((sd->s_parent == new_parent_sd) &&
			(strcmp(sd->s_name, new_name) == 0))
		goto out;	/* nothing to rename */

	// 检查新名称在新的父目录下是否已存在
	error = -EEXIST;
	if (sysfs_find_dirent(new_parent_sd, new_name))
		goto out;

	/* rename sysfs_dirent */
	// 如果名称有变化，分配并更新新的名称
	if (strcmp(sd->s_name, new_name) != 0) {
		error = -ENOMEM;
		new_name = dup_name = kstrdup(new_name, GFP_KERNEL); // 复制新名称
		if (!new_name)
			goto out;

		// 释放旧名称并更新目录项的名称
		dup_name = sd->s_name;
		sd->s_name = new_name;
	}

	/* Remove from old parent's list and insert into new parent's list. */
	// 如果父目录发生变化，更新父目录关联
	if (sd->s_parent != new_parent_sd) {
		sysfs_unlink_sibling(sd); // 从旧的父目录中移除
		sysfs_get(new_parent_sd); // 增加新父目录的引用计数
		sysfs_put(sd->s_parent);  // 减少旧父目录的引用计数
		sd->s_parent = new_parent_sd; // 更新父目录
		sysfs_link_sibling(sd); // 链接到新父目录
	}

	error = 0;
out:
	mutex_unlock(&sysfs_mutex); // 解锁
	kfree(dup_name); // 释放已复制的旧名称
	return error;
}

/**
 * sysfs_rename_dir - 重命名一个kobject关联的sysfs目录。
 * @kobj: 要重命名的kobject。
 * @new_name: 新目录名称。
 */
int sysfs_rename_dir(struct kobject *kobj, const char *new_name)
{
	// 调用sysfs_rename，父目录保持不变
	return sysfs_rename(kobj->sd, kobj->sd->s_parent, new_name);
}

/**
 * sysfs_move_dir - 移动一个kobject关联的sysfs目录到新的父目录。
 * @kobj: 要移动的kobject。
 * @new_parent_kobj: 新的父kobject。
 *
 * 返回值：成功时返回0，失败时返回非0错误码。
 */
int sysfs_move_dir(struct kobject *kobj, struct kobject *new_parent_kobj)
{
	struct sysfs_dirent *sd = kobj->sd;  // 当前kobject的sysfs目录项
	struct sysfs_dirent *new_parent_sd;  // 新父目录项

	BUG_ON(!sd->s_parent);  // 如果当前目录项没有父目录，则触发错误
	// 确定新的父目录项，如果提供了new_parent_kobj且其有目录项，则使用之，否则使用sysfs根目录
	new_parent_sd = new_parent_kobj && new_parent_kobj->sd ?
		new_parent_kobj->sd : &sysfs_root;

	// 调用sysfs_rename进行移动，名称不变，只改变父目录
	return sysfs_rename(sd, new_parent_sd, sd->s_name);
}

/* Relationship between s_mode and the DT_xxx types */
/**
 * dt_type - 从sysfs_dirent的s_mode字段推导出目录项的类型。
 * @sd: sysfs目录项。
 *
 * 返回值：目录项的类型，与Linux内核中的DT_xxx类型对应。
 */
static inline unsigned char dt_type(struct sysfs_dirent *sd)
{
	// 将s_mode字段右移12位后与15进行与运算，得到目录项类型
	return (sd->s_mode >> 12) & 15;
}

/**
 * sysfs_dir_release - 关闭sysfs目录文件时的回调函数。
 * @inode: 文件对应的inode。
 * @filp: 文件结构体。
 *
 * 当最后一个对sysfs目录文件的引用被释放时调用，释放与文件关联的资源。
 * 返回值：总是返回0。
 */
static int sysfs_dir_release(struct inode *inode, struct file *filp)
{
	// 释放文件私有数据中保存的sysfs目录项引用
	sysfs_put(filp->private_data);
	return 0;
}

/**
 * sysfs_dir_pos - 查找给定位置的sysfs_dirent
 * @parent_sd: 父sysfs_dirent
 * @ino: inode编号
 * @pos: 当前位置的sysfs_dirent
 *
 * 该函数用于查找给定位置的sysfs_dirent。如果提供的位置有效且与给定的inode编号匹配，
 * 就返回该位置，否则从父节点的子列表中查找。
 *
 * 返回值：找到的sysfs_dirent或NULL。
 */
static struct sysfs_dirent *sysfs_dir_pos(struct sysfs_dirent *parent_sd,
	ino_t ino, struct sysfs_dirent *pos)
{
	if (pos) {
		// 检查当前位置是否有效且未被移除，且父节点和inode编号匹配
		int valid = !(pos->s_flags & SYSFS_FLAG_REMOVED) &&
			pos->s_parent == parent_sd &&
			ino == pos->s_ino;
		sysfs_put(pos);  // 释放当前位置的引用
		if (valid)
			return pos;  // 如果有效，返回当前位置
	}
	// 如果当前位置无效或未提供，则从头开始查找
	pos = NULL;
	if ((ino > 1) && (ino < INT_MAX)) {  // 确保inode编号在有效范围内
		pos = parent_sd->s_dir.children;  // 从父节点的子列表开始
		while (pos && (ino > pos->s_ino))  // 寻找匹配或更大的inode编号
			pos = pos->s_sibling;  // 移动到下一个兄弟节点
	}
	return pos;  // 返回找到的位置或NULL
}

/**
 * sysfs_dir_next_pos - 获取给定位置的下一个sysfs_dirent
 * @parent_sd: 父sysfs_dirent
 * @ino: inode编号
 * @pos: 当前位置的sysfs_dirent
 *
 * 该函数用于获取给定位置的下一个sysfs_dirent。
 *
 * 返回值：下一个sysfs_dirent或NULL。
 */
static struct sysfs_dirent *sysfs_dir_next_pos(struct sysfs_dirent *parent_sd,
	ino_t ino, struct sysfs_dirent *pos)
{
	pos = sysfs_dir_pos(parent_sd, ino, pos);  // 获取当前位置
	if (pos)
		pos = pos->s_sibling;  // 获取下一个兄弟节点
	return pos;  // 返回下一个位置或NULL
}

/**
 * sysfs_readdir - 读取sysfs目录项
 * @filp: 文件指针，包含目录的上下文信息
 * @dirent: 目录条目缓冲区，由用户空间提供
 * @filldir: 辅助函数，用来把找到的每个条目传递给用户空间
 *
 * 该函数枚举sysfs目录中的所有条目，并使用filldir回调将其发送到用户空间。
 *
 * 返回值: 成功时返回0，否则返回负值错误代码。
 */
static int sysfs_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
	struct dentry *dentry = filp->f_path.dentry;  // 当前目录项
	struct sysfs_dirent * parent_sd = dentry->d_fsdata;  // 父目录的sysfs_dirent
	struct sysfs_dirent *pos = filp->private_data;  // 遍历位置
	ino_t ino;  // 节点编号

	// 特殊条目"."的处理
	if (filp->f_pos == 0) {
		ino = parent_sd->s_ino;  // 获取当前目录的inode编号
		// 填充"."条目
		if (filldir(dirent, ".", 1, filp->f_pos, ino, DT_DIR) == 0)
			filp->f_pos++;  // 移动到下一个位置
	}

	// 特殊条目".."的处理
	if (filp->f_pos == 1) {
		if (parent_sd->s_parent)
			ino = parent_sd->s_parent->s_ino;  // 获取父目录的inode编号
		else
			ino = parent_sd->s_ino;
		// 填充".."条目
		if (filldir(dirent, "..", 2, filp->f_pos, ino, DT_DIR) == 0)
			filp->f_pos++;  // 移动到下一个位置
	}

	mutex_lock(&sysfs_mutex);  // 锁定sysfs互斥锁
	// 遍历当前目录下的所有条目
	for (pos = sysfs_dir_pos(parent_sd, filp->f_pos, pos);
	     pos;
	     pos = sysfs_dir_next_pos(parent_sd, filp->f_pos, pos)) {
		const char * name;
		unsigned int type;
		int len, ret;

		name = pos->s_name;  // 获取条目名称
		len = strlen(name);  // 计算名称长度
		ino = pos->s_ino;  // 获取inode编号
		type = dt_type(pos);  // 获取类型
		filp->f_pos = ino;  // 更新文件位置
		filp->private_data = sysfs_get(pos);  // 更新私有数据

		mutex_unlock(&sysfs_mutex);  // 解锁
		ret = filldir(dirent, name, len, filp->f_pos, ino, type);  // 调用filldir函数填充数据
		mutex_lock(&sysfs_mutex);  // 再次锁定
		if (ret < 0)
			break;  // 如果填充出错，则退出循环
	}
	mutex_unlock(&sysfs_mutex);  // 解锁
	if ((filp->f_pos > 1) && !pos) { /* EOF */
		filp->f_pos = INT_MAX;  // 设置位置为最大值，表示读取结束
		filp->private_data = NULL;  // 清空私有数据
	}
	return 0;
}

/**
 * sysfs_dir_operations - 定义sysfs目录操作的文件操作结构体。
 *
 * 该结构提供了对sysfs目录操作的底层文件操作方法。
 */
const struct file_operations sysfs_dir_operations = {
	.read       = generic_read_dir,   // 通用目录读取操作，返回-ENOTDIR错误，因为目录不可读。
	.readdir    = sysfs_readdir,      // 为sysfs提供读目录功能的函数，枚举目录中的文件。
	.release    = sysfs_dir_release,  // 在文件描述符关闭时调用，负责释放资源。
	.llseek     = generic_file_llseek, // 通用文件定位操作，允许改变文件内的读写位置。
};
