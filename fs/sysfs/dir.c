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

DEFINE_MUTEX(sysfs_mutex);
DEFINE_SPINLOCK(sysfs_assoc_lock);

static DEFINE_SPINLOCK(sysfs_ino_lock);
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
static void sysfs_link_sibling(struct sysfs_dirent *sd)
{
	struct sysfs_dirent *parent_sd = sd->s_parent;
	struct sysfs_dirent **pos;

	BUG_ON(sd->s_sibling);

	/* Store directory entries in order by ino.  This allows
	 * readdir to properly restart without having to add a
	 * cursor into the s_dir.children list.
	 */
	for (pos = &parent_sd->s_dir.children; *pos; pos = &(*pos)->s_sibling) {
		if (sd->s_ino < (*pos)->s_ino)
			break;
	}
	sd->s_sibling = *pos;
	*pos = sd;
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
static void sysfs_unlink_sibling(struct sysfs_dirent *sd)
{
	struct sysfs_dirent **pos;

	for (pos = &sd->s_parent->s_dir.children; *pos;
	     pos = &(*pos)->s_sibling) {
		if (*pos == sd) {
			*pos = sd->s_sibling;
			sd->s_sibling = NULL;
			break;
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
struct sysfs_dirent *sysfs_get_active(struct sysfs_dirent *sd)
{
	if (unlikely(!sd))
		return NULL;

	while (1) {
		int v, t;

		v = atomic_read(&sd->s_active);
		if (unlikely(v < 0))
			return NULL;

		t = atomic_cmpxchg(&sd->s_active, v, v + 1);
		if (likely(t == v)) {
			rwsem_acquire_read(&sd->dep_map, 0, 1, _RET_IP_);
			return sd;
		}
		if (t < 0)
			return NULL;

		cpu_relax();
	}
}

/**
 *	sysfs_put_active - put an active reference to sysfs_dirent
 *	@sd: sysfs_dirent to put an active reference to
 *
 *	Put an active reference to @sd.  This function is noop if @sd
 *	is NULL.
 */
void sysfs_put_active(struct sysfs_dirent *sd)
{
	struct completion *cmpl;
	int v;

	if (unlikely(!sd))
		return;

	rwsem_release(&sd->dep_map, 1, _RET_IP_);
	v = atomic_dec_return(&sd->s_active);
	if (likely(v != SD_DEACTIVATED_BIAS))
		return;

	/* atomic_dec_return() is a mb(), we'll always see the updated
	 * sd->s_sibling.
	 */
	cmpl = (void *)sd->s_sibling;
	complete(cmpl);
}

/**
 *	sysfs_deactivate - deactivate sysfs_dirent
 *	@sd: sysfs_dirent to deactivate
 *
 *	Deny new active references and drain existing ones.
 */
static void sysfs_deactivate(struct sysfs_dirent *sd)
{
	DECLARE_COMPLETION_ONSTACK(wait);
	int v;

	BUG_ON(sd->s_sibling || !(sd->s_flags & SYSFS_FLAG_REMOVED));

	if (!(sysfs_type(sd) & SYSFS_ACTIVE_REF))
		return;

	sd->s_sibling = (void *)&wait;

	rwsem_acquire(&sd->dep_map, 0, 0, _RET_IP_);
	/* atomic_add_return() is a mb(), put_active() will always see
	 * the updated sd->s_sibling.
	 */
	v = atomic_add_return(SD_DEACTIVATED_BIAS, &sd->s_active);

	if (v != SD_DEACTIVATED_BIAS) {
		lock_contended(&sd->dep_map, _RET_IP_);
		wait_for_completion(&wait);
	}

	sd->s_sibling = NULL;

	lock_acquired(&sd->dep_map, _RET_IP_);
	rwsem_release(&sd->dep_map, 1, _RET_IP_);
}

static int sysfs_alloc_ino(ino_t *pino)
{
	int ino, rc;

 retry:
	spin_lock(&sysfs_ino_lock);
	rc = ida_get_new_above(&sysfs_ino_ida, 2, &ino);
	spin_unlock(&sysfs_ino_lock);

	if (rc == -EAGAIN) {
		if (ida_pre_get(&sysfs_ino_ida, GFP_KERNEL))
			goto retry;
		rc = -ENOMEM;
	}

	*pino = ino;
	return rc;
}

static void sysfs_free_ino(ino_t ino)
{
	spin_lock(&sysfs_ino_lock);
	ida_remove(&sysfs_ino_ida, ino);
	spin_unlock(&sysfs_ino_lock);
}

void release_sysfs_dirent(struct sysfs_dirent * sd)
{
	struct sysfs_dirent *parent_sd;

 repeat:
	/* Moving/renaming is always done while holding reference.
	 * sd->s_parent won't change beneath us.
	 */
	parent_sd = sd->s_parent;

	if (sysfs_type(sd) == SYSFS_KOBJ_LINK)
		sysfs_put(sd->s_symlink.target_sd);
	if (sysfs_type(sd) & SYSFS_COPY_NAME)
		kfree(sd->s_name);
	if (sd->s_iattr && sd->s_iattr->ia_secdata)
		security_release_secctx(sd->s_iattr->ia_secdata,
					sd->s_iattr->ia_secdata_len);
	kfree(sd->s_iattr);
	sysfs_free_ino(sd->s_ino);
	kmem_cache_free(sysfs_dir_cachep, sd);

	sd = parent_sd;
	if (sd && atomic_dec_and_test(&sd->s_count))
		goto repeat;
}

static int sysfs_dentry_delete(struct dentry *dentry)
{
	struct sysfs_dirent *sd = dentry->d_fsdata;
	return !!(sd->s_flags & SYSFS_FLAG_REMOVED);
}

static int sysfs_dentry_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	struct sysfs_dirent *sd = dentry->d_fsdata;
	int is_dir;

	mutex_lock(&sysfs_mutex);

	/* The sysfs dirent has been deleted */
	if (sd->s_flags & SYSFS_FLAG_REMOVED)
		goto out_bad;

	/* The sysfs dirent has been moved? */
	if (dentry->d_parent->d_fsdata != sd->s_parent)
		goto out_bad;

	/* The sysfs dirent has been renamed */
	if (strcmp(dentry->d_name.name, sd->s_name) != 0)
		goto out_bad;

	mutex_unlock(&sysfs_mutex);
out_valid:
	return 1;
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
	is_dir = (sysfs_type(sd) == SYSFS_DIR);
	mutex_unlock(&sysfs_mutex);
	if (is_dir) {
		/* If we have submounts we must allow the vfs caches
		 * to lie about the state of the filesystem to prevent
		 * leaks and other nasty things.
		 */
		if (have_submounts(dentry))
			goto out_valid;
		shrink_dcache_parent(dentry);
	}
	d_drop(dentry);
	return 0;
}

static void sysfs_dentry_iput(struct dentry *dentry, struct inode *inode)
{
	struct sysfs_dirent * sd = dentry->d_fsdata;

	sysfs_put(sd);
	iput(inode);
}

static const struct dentry_operations sysfs_dentry_ops = {
	.d_revalidate	= sysfs_dentry_revalidate,
	.d_delete	= sysfs_dentry_delete,
	.d_iput		= sysfs_dentry_iput,
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
struct sysfs_dirent *sysfs_find_dirent(struct sysfs_dirent *parent_sd,
				       const unsigned char *name)
{
	struct sysfs_dirent *sd;

	for (sd = parent_sd->s_dir.children; sd; sd = sd->s_sibling)
		if (!strcmp(sd->s_name, name))
			return sd;
	return NULL;
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
struct sysfs_dirent *sysfs_get_dirent(struct sysfs_dirent *parent_sd,
				      const unsigned char *name)
{
	struct sysfs_dirent *sd;

	mutex_lock(&sysfs_mutex);
	sd = sysfs_find_dirent(parent_sd, name);
	sysfs_get(sd);
	mutex_unlock(&sysfs_mutex);

	return sd;
}
EXPORT_SYMBOL_GPL(sysfs_get_dirent);

static int create_dir(struct kobject *kobj, struct sysfs_dirent *parent_sd,
		      const char *name, struct sysfs_dirent **p_sd)
{
	umode_t mode = S_IFDIR| S_IRWXU | S_IRUGO | S_IXUGO;
	struct sysfs_addrm_cxt acxt;
	struct sysfs_dirent *sd;
	int rc;

	/* allocate */
	sd = sysfs_new_dirent(name, mode, SYSFS_DIR);
	if (!sd)
		return -ENOMEM;
	sd->s_dir.kobj = kobj;

	/* link in */
	sysfs_addrm_start(&acxt, parent_sd);
	rc = sysfs_add_one(&acxt, sd);
	sysfs_addrm_finish(&acxt);

	if (rc == 0)
		*p_sd = sd;
	else
		sysfs_put(sd);

	return rc;
}

int sysfs_create_subdir(struct kobject *kobj, const char *name,
			struct sysfs_dirent **p_sd)
{
	return create_dir(kobj, kobj->sd, name, p_sd);
}

/**
 *	sysfs_create_dir - create a directory for an object.
 *	@kobj:		object we're creating directory for. 
 */
int sysfs_create_dir(struct kobject * kobj)
{
	struct sysfs_dirent *parent_sd, *sd;
	int error = 0;

	BUG_ON(!kobj);

	if (kobj->parent)
		parent_sd = kobj->parent->sd;
	else
		parent_sd = &sysfs_root;

	error = create_dir(kobj, parent_sd, kobject_name(kobj), &sd);
	if (!error)
		kobj->sd = sd;
	return error;
}

static struct dentry * sysfs_lookup(struct inode *dir, struct dentry *dentry,
				struct nameidata *nd)
{
	struct dentry *ret = NULL;
	struct sysfs_dirent *parent_sd = dentry->d_parent->d_fsdata;
	struct sysfs_dirent *sd;
	struct inode *inode;

	mutex_lock(&sysfs_mutex);

	sd = sysfs_find_dirent(parent_sd, dentry->d_name.name);

	/* no such entry */
	if (!sd) {
		ret = ERR_PTR(-ENOENT);
		goto out_unlock;
	}

	/* attach dentry and inode */
	inode = sysfs_get_inode(dir->i_sb, sd);
	if (!inode) {
		ret = ERR_PTR(-ENOMEM);
		goto out_unlock;
	}

	/* instantiate and hash dentry */
	ret = d_find_alias(inode);
	if (!ret) {
		dentry->d_op = &sysfs_dentry_ops;
		dentry->d_fsdata = sysfs_get(sd);
		d_add(dentry, inode);
	} else {
		d_move(ret, dentry);
		iput(inode);
	}

 out_unlock:
	mutex_unlock(&sysfs_mutex);
	return ret;
}

const struct inode_operations sysfs_dir_inode_operations = {
	.lookup		= sysfs_lookup,
	.permission	= sysfs_permission,
	.setattr	= sysfs_setattr,
	.getattr	= sysfs_getattr,
	.setxattr	= sysfs_setxattr,
};

static void remove_dir(struct sysfs_dirent *sd)
{
	struct sysfs_addrm_cxt acxt;

	sysfs_addrm_start(&acxt, sd->s_parent);
	sysfs_remove_one(&acxt, sd);
	sysfs_addrm_finish(&acxt);
}

void sysfs_remove_subdir(struct sysfs_dirent *sd)
{
	remove_dir(sd);
}


static void __sysfs_remove_dir(struct sysfs_dirent *dir_sd)
{
	struct sysfs_addrm_cxt acxt;
	struct sysfs_dirent **pos;

	if (!dir_sd)
		return;

	pr_debug("sysfs %s: removing dir\n", dir_sd->s_name);
	sysfs_addrm_start(&acxt, dir_sd);
	pos = &dir_sd->s_dir.children;
	while (*pos) {
		struct sysfs_dirent *sd = *pos;

		if (sysfs_type(sd) != SYSFS_DIR)
			sysfs_remove_one(&acxt, sd);
		else
			pos = &(*pos)->s_sibling;
	}
	sysfs_addrm_finish(&acxt);

	remove_dir(dir_sd);
}

/**
 *	sysfs_remove_dir - remove an object's directory.
 *	@kobj:	object.
 *
 *	The only thing special about this is that we remove any files in
 *	the directory before we remove the directory, and we've inlined
 *	what used to be sysfs_rmdir() below, instead of calling separately.
 */

void sysfs_remove_dir(struct kobject * kobj)
{
	struct sysfs_dirent *sd = kobj->sd;

	spin_lock(&sysfs_assoc_lock);
	kobj->sd = NULL;
	spin_unlock(&sysfs_assoc_lock);

	__sysfs_remove_dir(sd);
}

int sysfs_rename(struct sysfs_dirent *sd,
	struct sysfs_dirent *new_parent_sd, const char *new_name)
{
	const char *dup_name = NULL;
	int error;

	mutex_lock(&sysfs_mutex);

	error = 0;
	if ((sd->s_parent == new_parent_sd) &&
	    (strcmp(sd->s_name, new_name) == 0))
		goto out;	/* nothing to rename */

	error = -EEXIST;
	if (sysfs_find_dirent(new_parent_sd, new_name))
		goto out;

	/* rename sysfs_dirent */
	if (strcmp(sd->s_name, new_name) != 0) {
		error = -ENOMEM;
		new_name = dup_name = kstrdup(new_name, GFP_KERNEL);
		if (!new_name)
			goto out;

		dup_name = sd->s_name;
		sd->s_name = new_name;
	}

	/* Remove from old parent's list and insert into new parent's list. */
	if (sd->s_parent != new_parent_sd) {
		sysfs_unlink_sibling(sd);
		sysfs_get(new_parent_sd);
		sysfs_put(sd->s_parent);
		sd->s_parent = new_parent_sd;
		sysfs_link_sibling(sd);
	}

	error = 0;
 out:
	mutex_unlock(&sysfs_mutex);
	kfree(dup_name);
	return error;
}

int sysfs_rename_dir(struct kobject *kobj, const char *new_name)
{
	return sysfs_rename(kobj->sd, kobj->sd->s_parent, new_name);
}

int sysfs_move_dir(struct kobject *kobj, struct kobject *new_parent_kobj)
{
	struct sysfs_dirent *sd = kobj->sd;
	struct sysfs_dirent *new_parent_sd;

	BUG_ON(!sd->s_parent);
	new_parent_sd = new_parent_kobj && new_parent_kobj->sd ?
		new_parent_kobj->sd : &sysfs_root;

	return sysfs_rename(sd, new_parent_sd, sd->s_name);
}

/* Relationship between s_mode and the DT_xxx types */
static inline unsigned char dt_type(struct sysfs_dirent *sd)
{
	return (sd->s_mode >> 12) & 15;
}

static int sysfs_dir_release(struct inode *inode, struct file *filp)
{
	sysfs_put(filp->private_data);
	return 0;
}

static struct sysfs_dirent *sysfs_dir_pos(struct sysfs_dirent *parent_sd,
	ino_t ino, struct sysfs_dirent *pos)
{
	if (pos) {
		int valid = !(pos->s_flags & SYSFS_FLAG_REMOVED) &&
			pos->s_parent == parent_sd &&
			ino == pos->s_ino;
		sysfs_put(pos);
		if (valid)
			return pos;
	}
	pos = NULL;
	if ((ino > 1) && (ino < INT_MAX)) {
		pos = parent_sd->s_dir.children;
		while (pos && (ino > pos->s_ino))
			pos = pos->s_sibling;
	}
	return pos;
}

static struct sysfs_dirent *sysfs_dir_next_pos(struct sysfs_dirent *parent_sd,
	ino_t ino, struct sysfs_dirent *pos)
{
	pos = sysfs_dir_pos(parent_sd, ino, pos);
	if (pos)
		pos = pos->s_sibling;
	return pos;
}

static int sysfs_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
	struct dentry *dentry = filp->f_path.dentry;
	struct sysfs_dirent * parent_sd = dentry->d_fsdata;
	struct sysfs_dirent *pos = filp->private_data;
	ino_t ino;

	if (filp->f_pos == 0) {
		ino = parent_sd->s_ino;
		if (filldir(dirent, ".", 1, filp->f_pos, ino, DT_DIR) == 0)
			filp->f_pos++;
	}
	if (filp->f_pos == 1) {
		if (parent_sd->s_parent)
			ino = parent_sd->s_parent->s_ino;
		else
			ino = parent_sd->s_ino;
		if (filldir(dirent, "..", 2, filp->f_pos, ino, DT_DIR) == 0)
			filp->f_pos++;
	}
	mutex_lock(&sysfs_mutex);
	for (pos = sysfs_dir_pos(parent_sd, filp->f_pos, pos);
	     pos;
	     pos = sysfs_dir_next_pos(parent_sd, filp->f_pos, pos)) {
		const char * name;
		unsigned int type;
		int len, ret;

		name = pos->s_name;
		len = strlen(name);
		ino = pos->s_ino;
		type = dt_type(pos);
		filp->f_pos = ino;
		filp->private_data = sysfs_get(pos);

		mutex_unlock(&sysfs_mutex);
		ret = filldir(dirent, name, len, filp->f_pos, ino, type);
		mutex_lock(&sysfs_mutex);
		if (ret < 0)
			break;
	}
	mutex_unlock(&sysfs_mutex);
	if ((filp->f_pos > 1) && !pos) { /* EOF */
		filp->f_pos = INT_MAX;
		filp->private_data = NULL;
	}
	return 0;
}


const struct file_operations sysfs_dir_operations = {
	.read		= generic_read_dir,
	.readdir	= sysfs_readdir,
	.release	= sysfs_dir_release,
	.llseek		= generic_file_llseek,
};
