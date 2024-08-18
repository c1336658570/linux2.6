/*
 *  linux/fs/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * Some corrections by tytso.
 */

/* [Feb 1997 T. Schoebel-Theuer] Complete rewrite of the pathname
 * lookup logic.
 */
/* [Feb-Apr 2000, AV] Rewrite to the new namespace architecture.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/fsnotify.h>
#include <linux/personality.h>
#include <linux/security.h>
#include <linux/ima.h>
#include <linux/syscalls.h>
#include <linux/mount.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/device_cgroup.h>
#include <linux/fs_struct.h>
#include <asm/uaccess.h>

#include "internal.h"

/* [Feb-1997 T. Schoebel-Theuer]
 * Fundamental changes in the pathname lookup mechanisms (namei)
 * were necessary because of omirr.  The reason is that omirr needs
 * to know the _real_ pathname, not the user-supplied one, in case
 * of symlinks (and also when transname replacements occur).
 *
 * The new code replaces the old recursive symlink resolution with
 * an iterative one (in case of non-nested symlink chains).  It does
 * this with calls to <fs>_follow_link().
 * As a side effect, dir_namei(), _namei() and follow_link() are now 
 * replaced with a single function lookup_dentry() that can handle all 
 * the special cases of the former code.
 *
 * With the new dcache, the pathname is stored at each inode, at least as
 * long as the refcount of the inode is positive.  As a side effect, the
 * size of the dcache depends on the inode cache and thus is dynamic.
 *
 * [29-Apr-1998 C. Scott Ananian] Updated above description of symlink
 * resolution to correspond with current state of the code.
 *
 * Note that the symlink resolution is not *completely* iterative.
 * There is still a significant amount of tail- and mid- recursion in
 * the algorithm.  Also, note that <fs>_readlink() is not used in
 * lookup_dentry(): lookup_dentry() on the result of <fs>_readlink()
 * may return different results than <fs>_follow_link().  Many virtual
 * filesystems (including /proc) exhibit this behavior.
 */

/* [24-Feb-97 T. Schoebel-Theuer] Side effects caused by new implementation:
 * New symlink semantics: when open() is called with flags O_CREAT | O_EXCL
 * and the name already exists in form of a symlink, try to create the new
 * name indicated by the symlink. The old code always complained that the
 * name already exists, due to not following the symlink even if its target
 * is nonexistent.  The new semantics affects also mknod() and link() when
 * the name is a symlink pointing to a non-existant name.
 *
 * I don't know which semantics is the right one, since I have no access
 * to standards. But I found by trial that HP-UX 9.0 has the full "new"
 * semantics implemented, while SunOS 4.1.1 and Solaris (SunOS 5.4) have the
 * "old" one. Personally, I think the new semantics is much more logical.
 * Note that "ln old new" where "new" is a symlink pointing to a non-existing
 * file does succeed in both HP-UX and SunOs, but not in Solaris
 * and in the old Linux semantics.
 */

/* [16-Dec-97 Kevin Buhr] For security reasons, we change some symlink
 * semantics.  See the comments in "open_namei" and "do_link" below.
 *
 * [10-Sep-98 Alan Modra] Another symlink change.
 */

/* [Feb-Apr 2000 AV] Complete rewrite. Rules for symlinks:
 *	inside the path - always follow.
 *	in the last component in creation/removal/renaming - never follow.
 *	if LOOKUP_FOLLOW passed - follow.
 *	if the pathname has trailing slashes - follow.
 *	otherwise - don't follow.
 * (applied in that order).
 *
 * [Jun 2000 AV] Inconsistent behaviour of open() in case if flags==O_CREAT
 * restored for 2.4. This is the last surviving part of old 4.2BSD bug.
 * During the 2.4 we need to fix the userland stuff depending on it -
 * hopefully we will be able to get rid of that wart in 2.5. So far only
 * XEmacs seems to be relying on it...
 */
/*
 * [Sep 2001 AV] Single-semaphore locking scheme (kudos to David Holland)
 * implemented.  Let's see if raised priority of ->s_vfs_rename_mutex gives
 * any extra contention...
 */

/* In order to reduce some races, while at the same time doing additional
 * checking and hopefully speeding things up, we copy filenames to the
 * kernel data space before using them..
 *
 * POSIX.1 2.4: an empty pathname is invalid (ENOENT).
 * PATH_MAX includes the nul terminator --RR.
 */
/*
 * do_getname - 从用户空间获取文件名并拷贝到内核空间
 * @filename: 用户空间中的文件名指针
 * @page: 内核空间的缓冲区，用于存储拷贝的文件名
 * 返回值: 成功时返回 0，失败时返回负的错误码
 *
 * 此函数尝试将用户空间的文件名安全地拷贝到内核指定的缓冲区中。
 */
static int do_getname(const char __user *filename, char *page)
{
	int retval;
	unsigned long len = PATH_MAX;  // 定义最大路径长度

	if (!segment_eq(get_fs(), KERNEL_DS)) {  // 检查当前的地址限制，确保不是内核空间
		if ((unsigned long) filename >= TASK_SIZE)  // 如果文件名指针超出了用户空间的限制
			return -EFAULT;  // 返回错误码，表示无法访问的内存区域
		if (TASK_SIZE - (unsigned long) filename < PATH_MAX)  // 如果文件名指针到任务空间上限的距离小于最大路径长度
			len = TASK_SIZE - (unsigned long) filename;  // 调整长度以避免越界
	}

	retval = strncpy_from_user(page, filename, len);  // 从用户空间拷贝字符串到内核空间
	if (retval > 0) {  // 如果拷贝成功并且拷贝的字符数大于0
		if (retval < len)
			return 0;  // 如果拷贝的长度小于最大长度，表示成功拷贝了整个字符串
		return -ENAMETOOLONG;  // 如果拷贝的长度等于最大长度，可能没有拷贝完全
	} else if (!retval)
		retval = -ENOENT;  // 如果没有拷贝任何字符，可能是因为文件名不存在

	return retval;  // 返回拷贝过程中遇到的错误码
}

/*
 * getname - 从用户空间获取文件名
 * @filename: 指向用户空间中文件名的指针
 * 返回值: 成功时返回指向内核空间中文件名的指针，失败时返回错误指针
 *
 * 此函数用于从用户空间获取一个文件名字符串，并确保该字符串被安全地复制到内核空间。
 */
char * getname(const char __user * filename)
{
	char *tmp, *result;

	result = ERR_PTR(-ENOMEM);  // 预设结果为内存不足的错误指针

	tmp = __getname();  // 从名字缓存获取一个临时缓冲区
	if (tmp)  {  // 如果成功获取到缓冲区
		int retval = do_getname(filename, tmp);  // 尝试从用户空间获取文件名并拷贝到缓冲区

		result = tmp;  // 预设结果为缓冲区的地址
		if (retval < 0) {  // 如果获取文件名失败
			__putname(tmp);  // 释放缓冲区
			result = ERR_PTR(retval);  // 设置错误指针为相应的错误码
		}
	}
	audit_getname(result);  // 审计文件名获取操作
	return result;  // 返回结果
}

#ifdef CONFIG_AUDITSYSCALL  // 如果配置了系统调用审计
/*
 * putname - 释放由 getname 获取的文件名字符串
 * @name: 指向内核空间中文件名的指针
 *
 * 这个函数用于释放由 getname 函数获取的字符串，并根据审计配置决定如何进行释放。
 */
void putname(const char *name)
{
	if (unlikely(!audit_dummy_context())) // 如果当前不是一个“假”的审计上下文
		audit_putname(name);              // 使用审计系统的释放函数
	else
		__putname(name);                  // 否则使用标准释放函数
}
EXPORT_SYMBOL(putname);  // 导出 putname 函数，使其可以被其他内核模块调用
#endif  // 结束 ifdef CONFIG_AUDITSYSCALL

/*
 * This does basic POSIX ACL permission checking
 */
/*
 * 这个函数执行基本的 POSIX ACL 权限检查
 * acl_permission_check - 执行 POSIX 访问控制列表（ACL）权限检查
 * @inode: 被检查的 inode
 * @mask: 请求的权限掩码（MAY_READ, MAY_WRITE, MAY_EXEC）
 * @check_acl: 自定义的 ACL 检查函数
 * 返回值: 成功时返回 0，失败时返回 -EACCES 表示拒绝访问
 *
 * 这个函数用于检查请求的文件操作权限是否被授权。
 */
static int acl_permission_check(struct inode *inode, int mask,
		int (*check_acl)(struct inode *inode, int mask))
{
	umode_t			mode = inode->i_mode; // 获取 inode 的模式

	mask &= MAY_READ | MAY_WRITE | MAY_EXEC; // 确保掩码只包含有效的权限请求

	if (current_fsuid() == inode->i_uid) // 如果当前用户是文件所有者
		mode >>= 6; // 只考虑所有者权限位
	else {
		if (IS_POSIXACL(inode) && (mode & S_IRWXG) && check_acl) { // 如果 inode 有 POSIX ACL 且定义了检查函数
			int error = check_acl(inode, mask); // 调用自定义的 ACL 检查函数
			if (error != -EAGAIN) // 如果检查函数返回的不是 -EAGAIN，即需要立即决定
				return error; // 返回检查结果
		}

		if (in_group_p(inode->i_gid)) // 如果当前用户属于文件所在组
			mode >>= 3; // 只考虑组权限位
	}

	/*
	 * If the DACs are ok we don't need any capability check.
	 */
	/*
	 * 如果 DAC（自主访问控制）检查通过，我们不需要进行任何能力检查。
	 */
	if ((mask & ~mode) == 0) // 如果请求的权限完全被 DAC 授权
		return 0; // 返回成功
	return -EACCES; // 否则返回访问被拒绝的错误码
}

/**
 * generic_permission  -  check for access rights on a Posix-like filesystem
 * @inode:	inode to check access rights for
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC)
 * @check_acl:	optional callback to check for Posix ACLs
 *
 * Used to check for read/write/execute permissions on a file.
 * We use "fsuid" for this, letting us set arbitrary permissions
 * for filesystem access without changing the "normal" uids which
 * are used for other things..
 */
/**
 * generic_permission - 检查 Posix 类文件系统上的访问权限
 * @inode: 需要检查访问权限的 inode
 * @mask: 要检查的权限 (%MAY_READ, %MAY_WRITE, %MAY_EXEC)
 * @check_acl: 可选的回调函数，用于检查 Posix ACLs
 *
 * 用于检查文件的读/写/执行权限。
 * 这里使用 "fsuid" 进行权限检查，允许我们设置文件系统访问的任意权限，
 * 而不改变用于其他用途的 "正常" uid。
 */
int generic_permission(struct inode *inode, int mask,
		int (*check_acl)(struct inode *inode, int mask))
{
	int ret;

	/*
	 * Do the basic POSIX ACL permission checks.
	 */
	/*
	 * 执行基本的 POSIX ACL 权限检查。
	 */
	ret = acl_permission_check(inode, mask, check_acl); // 先进行 ACL 权限检查
	if (ret != -EACCES) // 如果结果不是 "拒绝访问"
		return ret; // 直接返回结果

	/*
	 * Read/write DACs are always overridable.
	 * Executable DACs are overridable if at least one exec bit is set.
	 */
	/*
	 * 读/写 DAC 总是可以被覆盖。
	 * 如果设置了至少一个执行位，可执行 DAC 也可以被覆盖。
	 */
	if (!(mask & MAY_EXEC) || execute_ok(inode)) // 如果不是执行权限检查，或者执行权限检查通过
		if (capable(CAP_DAC_OVERRIDE)) // 如果有足够的权限来覆盖 DAC
			return 0; // 返回允许访问

	/*
	 * Searching includes executable on directories, else just read.
	 */
	/*
	 * 搜索操作包括对目录的可执行权限，否则只需要读权限。
	 */
	mask &= MAY_READ | MAY_WRITE | MAY_EXEC; // 确保掩码只包含有效的权限请求
	if (mask == MAY_READ || (S_ISDIR(inode->i_mode) && !(mask & MAY_WRITE))) // 如果是读权限请求，或是对目录的搜索操作（不包括写）
		if (capable(CAP_DAC_READ_SEARCH)) // 如果有搜索权限
			return 0; // 返回允许访问

	return -EACCES; // 默认返回 "拒绝访问"
}

/**
 * inode_permission  -  check for access rights to a given inode
 * @inode:	inode to check permission on
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC)
 *
 * Used to check for read/write/execute permissions on an inode.
 * We use "fsuid" for this, letting us set arbitrary permissions
 * for filesystem access without changing the "normal" uids which
 * are used for other things.
 */
/**
 * inode_permission - 检查对给定 inode 的访问权限
 * @inode: 需要检查权限的 inode
 * @mask: 要检查的权限 (%MAY_READ, %MAY_WRITE, %MAY_EXEC)
 *
 * 用于检查 inode 的读/写/执行权限。
 * 我们使用 "fsuid" 来设置文件系统访问的任意权限，而不改变用于其他用途的 "正常" uid。
 */
int inode_permission(struct inode *inode, int mask)
{
	int retval;

	if (mask & MAY_WRITE) {  // 如果检查写权限
		umode_t mode = inode->i_mode;  // 获取 inode 的模式

		/*
		 * Nobody gets write access to a read-only fs.
		 */
		/*
		 * 无人可以写入只读文件系统。
		 */
		if (IS_RDONLY(inode) &&  // 如果文件系统是只读的
		    (S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode)))  // 并且 inode 是常规文件、目录或符号链接
			return -EROFS;  // 返回只读文件系统错误

		/*
		 * Nobody gets write access to an immutable file.
		 */
		/*
		 * 无人可以写入不可变文件。
		 */
		if (IS_IMMUTABLE(inode))  // 如果 inode 是不可变的
			return -EACCES;  // 返回访问被拒绝错误
	}

	if (inode->i_op->permission)  // 如果定义了特定的权限检查函数
		retval = inode->i_op->permission(inode, mask);  // 调用该函数进行权限检查
	else
		retval = generic_permission(inode, mask, inode->i_op->check_acl);  // 否则调用通用权限检查函数

	if (retval)  // 如果权限检查失败
		return retval;  // 返回错误码

	retval = devcgroup_inode_permission(inode, mask);  // 检查设备控制组对 inode 的权限
	if (retval)  // 如果检查失败
		return retval;  // 返回错误码

	// 调用安全模块进行最终的权限检查
	return security_inode_permission(inode, mask & (MAY_READ|MAY_WRITE|MAY_EXEC|MAY_APPEND));
}

/**
 * file_permission  -  check for additional access rights to a given file
 * @file:	file to check access rights for
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC)
 *
 * Used to check for read/write/execute permissions on an already opened
 * file.
 *
 * Note:
 *	Do not use this function in new code.  All access checks should
 *	be done using inode_permission().
 */
/**
 * file_permission - 检查对已打开文件的额外访问权限
 * @file: 需要检查访问权限的文件
 * @mask: 需要检查的权限 (%MAY_READ, %MAY_WRITE, %MAY_EXEC)
 *
 * 用于检查已打开文件的读/写/执行权限。
 *
 * 注意：
 * 不要在新代码中使用这个函数。所有的访问检查应该使用 inode_permission() 来完成。
 */
int file_permission(struct file *file, int mask)
{
	return inode_permission(file->f_path.dentry->d_inode, mask);  // 直接调用 inode_permission 函数检查权限
}

/*
 * get_write_access() gets write permission for a file.
 * put_write_access() releases this write permission.
 * This is used for regular files.
 * We cannot support write (and maybe mmap read-write shared) accesses and
 * MAP_DENYWRITE mmappings simultaneously. The i_writecount field of an inode
 * can have the following values:
 * 0: no writers, no VM_DENYWRITE mappings
 * < 0: (-i_writecount) vm_area_structs with VM_DENYWRITE set exist
 * > 0: (i_writecount) users are writing to the file.
 *
 * Normally we operate on that counter with atomic_{inc,dec} and it's safe
 * except for the cases where we don't hold i_writecount yet. Then we need to
 * use {get,deny}_write_access() - these functions check the sign and refuse
 * to do the change if sign is wrong. Exclusion between them is provided by
 * the inode->i_lock spinlock.
 */
/*
 * get_write_access() 获取文件的写权限。
 * put_write_access() 释放这个写权限。
 * 这用于常规文件。
 * 我们不能同时支持写入（可能还包括 mmap 读写共享）访问和 MAP_DENYWRITE 内存映射。
 * inode 的 i_writecount 字段可以有以下值：
 * 0: 没有写者，没有 VM_DENYWRITE 映射
 * < 0: 存在带有 VM_DENYWRITE 设置的 vm_area_struct 数量的负数
 * > 0: 正在写入文件的用户数
 *
 * 通常我们使用 atomic_{inc,dec} 操作该计数器，这是安全的，
 * 除非我们还没有持有 i_writecount。然后我们需要使用 {get,deny}_write_access() -
 * 这些函数检查符号并在符号错误时拒绝改变。它们之间的排他性由 inode->i_lock 自旋锁提供。
 */

int get_write_access(struct inode * inode)
{
	spin_lock(&inode->i_lock);  // 锁定 inode，防止其他线程同时修改 i_writecount
	if (atomic_read(&inode->i_writecount) < 0) {  // 如果 i_writecount 是负数
		spin_unlock(&inode->i_lock);  // 解锁
		return -ETXTBSY;  // 返回错误，表示文件正被用于不允许写入的映射
	}
	atomic_inc(&inode->i_writecount);  // 增加 i_writecount，表示增加一个写入者
	spin_unlock(&inode->i_lock);  // 解锁

	return 0;  // 返回 0，表示成功获取写权限
}

/*
 * deny_write_access - 拒绝对文件的写入访问
 * @file: 需要检查的文件
 * 返回值: 成功时返回 0，如果有活动的写入者则返回 -ETXTBSY
 *
 * 该函数用于设置一个文件的状态，使其不能被写入。它检查文件的 inode 的写计数器（i_writecount），
 * 如果存在活动的写入者（i_writecount > 0），则拒绝设置并返回错误。
 */
int deny_write_access(struct file * file)
{
	struct inode *inode = file->f_path.dentry->d_inode;  // 获取文件的 inode

	spin_lock(&inode->i_lock);  // 锁定 inode 以同步对 i_writecount 的访问
	if (atomic_read(&inode->i_writecount) > 0) {  // 如果 i_writecount 大于 0，表示有活动的写入者
		spin_unlock(&inode->i_lock);  // 解锁 inode
		return -ETXTBSY;  // 返回错误，表示文件正忙
	}
	atomic_dec(&inode->i_writecount);  // 将 i_writecount 减 1，设置为不允许写入状态
	spin_unlock(&inode->i_lock);  // 解锁 inode

	return 0;  // 返回 0，表示成功设置文件为不允许写入状态
}

/**
 * path_get - get a reference to a path
 * @path: path to get the reference to
 *
 * Given a path increment the reference count to the dentry and the vfsmount.
 */
/**
 * path_get - 获取对路径的引用
 * @path: 需要获取引用的路径
 *
 * 对给定的路径增加目录项和虚拟文件系统挂载点的引用计数。
 */
void path_get(struct path *path)
{
	mntget(path->mnt);  // 增加虚拟文件系统挂载点的引用计数
	dget(path->dentry); // 增加目录项的引用计数
}
EXPORT_SYMBOL(path_get);

/**
 * path_put - put a reference to a path
 * @path: path to put the reference to
 *
 * Given a path decrement the reference count to the dentry and the vfsmount.
 */
/**
 * path_put - 释放对路径的引用
 * @path: 需要释放引用的路径
 *
 * 对给定的路径减少目录项和虚拟文件系统挂载点的引用计数。
 */
void path_put(struct path *path)
{
	dput(path->dentry);  // 减少目录项的引用计数
	mntput(path->mnt);   // 减少虚拟文件系统挂载点的引用计数
}
EXPORT_SYMBOL(path_put);

/**
 * release_open_intent - free up open intent resources
 * @nd: pointer to nameidata
 */
/**
 * release_open_intent - 释放打开意图相关的资源
 * @nd: 指向 nameidata 的指针
 *
 * 这个函数用于在不再需要时释放与文件打开意图相关的资源。
 */
void release_open_intent(struct nameidata *nd)
{
	if (nd->intent.open.file->f_path.dentry == NULL)  // 如果打开的文件的目录项为 NULL
		put_filp(nd->intent.open.file);  // 释放文件结构，这通常用于没有成功打开文件的情况
	else
		fput(nd->intent.open.file);  // 释放文件的引用，适用于已成功打开文件的情况
}

/**
 * do_revalidate - 重新验证目录项
 * @dentry: 需要验证的目录项
 * @nd: 相关的 nameidata 结构体，通常包含路径解析和查找操作的上下文
 *
 * 如果目录项需要重新验证（比如在网络文件系统中经常需要），这个函数会调用注册的 d_revalidate 方法。
 * 返回值：返回更新后的目录项；如果验证失败，可能返回 NULL 或错误指针。
 */
static inline struct dentry *
do_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	int status = dentry->d_op->d_revalidate(dentry, nd);  // 调用目录项的重新验证方法
	if (unlikely(status <= 0)) {  // 如果验证失败或有特定的失败状态
		/*
		 * The dentry failed validation.
		 * If d_revalidate returned 0 attempt to invalidate
		 * the dentry otherwise d_revalidate is asking us
		 * to return a fail status.
		 */
		/*
		 * 目录项验证失败。
		 * 如果 d_revalidate 返回 0 尝试使目录项无效，
		 * 否则 d_revalidate 要求我们返回一个失败状态。
		 */
		if (!status) {  // 如果 d_revalidate 返回 0
			if (!d_invalidate(dentry)) {  // 尝试使目录项无效
				dput(dentry);  // 减少目录项的引用计数
				dentry = NULL;  // 将目录项指针设置为 NULL
			}
		} else {  // 如果 d_revalidate 返回了一个负值（错误状态）
			dput(dentry);  // 减少目录项的引用计数
			dentry = ERR_PTR(status);  // 返回错误指针
		}
	}
	return dentry;  // 返回可能已更新的目录项
}

/*
 * force_reval_path - force revalidation of a dentry
 *
 * In some situations the path walking code will trust dentries without
 * revalidating them. This causes problems for filesystems that depend on
 * d_revalidate to handle file opens (e.g. NFSv4). When FS_REVAL_DOT is set
 * (which indicates that it's possible for the dentry to go stale), force
 * a d_revalidate call before proceeding.
 *
 * Returns 0 if the revalidation was successful. If the revalidation fails,
 * either return the error returned by d_revalidate or -ESTALE if the
 * revalidation it just returned 0. If d_revalidate returns 0, we attempt to
 * invalidate the dentry. It's up to the caller to handle putting references
 * to the path if necessary.
 */
/*
 * force_reval_path - 强制重新验证一个目录项
 *
 * 在某些情况下，路径遍历代码会信任目录项而不重新验证它们。这对于依赖于
 * d_revalidate 来处理文件打开的文件系统（例如 NFSv4）会导致问题。当设置了
 * FS_REVAL_DOT（这表示目录项可能变得陈旧），在继续之前强制调用 d_revalidate。
 *
 * 如果重新验证成功，则返回 0。如果重新验证失败，要么返回 d_revalidate 返回的错误，
 * 要么如果重新验证只返回 0 则返回 -ESTALE。如果 d_revalidate 返回 0，我们尝试
 * 使目录项无效。如果有必要，由调用者负责处理路径的引用。
 */
static int
force_reval_path(struct path *path, struct nameidata *nd)
{
	int status;
	struct dentry *dentry = path->dentry;  // 获取路径中的目录项

	/*
	 * only check on filesystems where it's possible for the dentry to
	 * become stale. It's assumed that if this flag is set then the
	 * d_revalidate op will also be defined.
	 */
	/*
	 * 仅在文件系统上检查，其中目录项可能变得陈旧。假设如果设置了这个标志，
	 * 那么 d_revalidate 操作也将被定义。
	 */
	if (!(dentry->d_sb->s_type->fs_flags & FS_REVAL_DOT))  // 如果文件系统不支持重新验证标志
		return 0;  // 直接返回 0，表示不需要重新验证

	status = dentry->d_op->d_revalidate(dentry, nd);  // 调用目录项的重新验证操作
	if (status > 0)
		return 0;  // 如果重新验证返回正值，表示成功，返回 0

	if (!status) {  // 如果重新验证返回 0，表示目录项可能已陈旧
		d_invalidate(dentry);  // 尝试使目录项无效
		status = -ESTALE;  // 设置返回状态为 -ESTALE 表示目录项陈旧
	}
	return status;  // 返回重新验证的结果
}

/*
 * Short-cut version of permission(), for calling on directories
 * during pathname resolution.  Combines parts of permission()
 * and generic_permission(), and tests ONLY for MAY_EXEC permission.
 *
 * If appropriate, check DAC only.  If not appropriate, or
 * short-cut DAC fails, then call ->permission() to do more
 * complete permission check.
 */
/*
 * exec_permission - 在路径名解析过程中，用于检查目录的执行权限的快捷版本。
 * 在路径名解析期间调用，主要用于目录，并且只检查 MAY_EXEC 权限。
 *
 * 如果适用，只检查 DAC (自主访问控制)。如果不适用，或者快捷 DAC 检查失败，
 * 则调用 ->permission() 来进行更完整的权限检查。
 */
static int exec_permission(struct inode *inode)
{
	int ret;

	if (inode->i_op->permission) {  // 如果定义了 inode 的 permission 操作
		ret = inode->i_op->permission(inode, MAY_EXEC);  // 调用这个操作进行权限检查
		if (!ret)  // 如果权限检查通过
			goto ok;
		return ret;  // 如果权限检查失败，返回错误代码
	}
	ret = acl_permission_check(inode, MAY_EXEC, inode->i_op->check_acl);  // 进行 ACL 权限检查
	if (!ret)  // 如果 ACL 权限检查通过
		goto ok;

	if (capable(CAP_DAC_OVERRIDE) || capable(CAP_DAC_READ_SEARCH))  // 检查进程是否有覆盖 DAC 权限的能力
		goto ok;  // 如果有，权限检查通过

	return ret;  // 返回 ACL 权限检查的结果
ok:
	return security_inode_permission(inode, MAY_EXEC);  // 进行安全模块的权限检查，并返回结果
}

/*
 * set_root - 设置 nameidata 结构中的根路径
 * @nd: 指向 nameidata 结构的指针
 *
 * 如果 nameidata 结构中的根路径未设置（即 nd->root.mnt 为空），此函数会从当前
 * 进程的文件系统结构中复制根路径到 nameidata，并增加路径的引用计数。
 * 这保证了 nameidata 在使用期间根路径有效且稳定。
 */
static __always_inline void set_root(struct nameidata *nd)
{
	if (!nd->root.mnt) {  // 检查 nameidata 的根挂载点是否已经设置
		struct fs_struct *fs = current->fs;  // 获取当前进程的文件系统结构
		read_lock(&fs->lock);  // 对文件系统结构加读锁，防止在复制过程中数据被改变
		nd->root = fs->root;  // 将进程的根路径复制到 nameidata 的根路径
		path_get(&nd->root);  // 增加复制得到的路径的引用计数
		read_unlock(&fs->lock);  // 释放读锁
	}
}

static int link_path_walk(const char *, struct nameidata *);

/*
 * __vfs_follow_link - 处理符号链接的解析
 * @nd: 包含当前路径状态的 nameidata 结构
 * @link: 指向符号链接目标的字符串
 *
 * 当解析路径遇到符号链接时调用此函数。该函数根据符号链接的内容更新 nameidata 结构，
 * 以便继续路径遍历。
 * 返回值：操作的结果状态，成功为 0，失败为错误代码。
 */
static __always_inline int __vfs_follow_link(struct nameidata *nd, const char *link)
{
	if (IS_ERR(link))  // 如果 link 是一个错误指针
		goto fail;  // 跳转到错误处理

	if (*link == '/') {  // 如果符号链接是绝对路径
		set_root(nd);  // 设置 nd 的根路径
		path_put(&nd->path);  // 释放当前路径的引用
		nd->path = nd->root;  // 将当前路径设置为根路径
		path_get(&nd->root);  // 增加根路径的引用计数
	}

	return link_path_walk(link, nd);  // 继续对符号链接目标进行路径遍历

fail:
	path_put(&nd->path);  // 在失败的情况下释放当前路径的引用
	return PTR_ERR(link);  // 返回 link 指针中的错误代码
}

/*
 * path_put_conditional - 有条件地释放路径的引用
 * @path: 指向要释放的路径结构的指针
 * @nd: 指向nameidata结构的指针，它持有当前路径状态
 *
 * 此函数释放指定路径的目录项引用。如果该路径的挂载点与nameidata中当前路径的挂载点不同，
 * 则也释放该挂载点的引用。这用于在路径遍历或处理过程中确保不再需要的路径资源被适当地释放。
 */
static void path_put_conditional(struct path *path, struct nameidata *nd)
{
	dput(path->dentry);  // 释放路径的目录项引用
	if (path->mnt != nd->path.mnt)  // 检查当前操作的路径的挂载点是否与nameidata中的挂载点不同
		mntput(path->mnt);  // 如果不同，释放该挂载点的引用
}

/*
 * path_to_nameidata - 将一个路径复制到nameidata结构
 * @path: 指向源路径结构的指针
 * @nd: 指向目标nameidata结构的指针
 *
 * 这个函数用于将一个路径结构复制到nameidata结构中，更新nameidata以反映新路径。
 * 这通常用于路径解析过程中，当路径组件需要更新或改变时。
 */
static inline void path_to_nameidata(struct path *path, struct nameidata *nd)
{
	dput(nd->path.dentry);  // 释放nameidata中当前路径的目录项引用
	if (nd->path.mnt != path->mnt)  // 如果nameidata中的挂载点与新路径的挂载点不同
		mntput(nd->path.mnt);  // 释放nameidata中的挂载点引用
	nd->path.mnt = path->mnt;  // 更新nameidata结构中的挂载点指针为新路径的挂载点
	nd->path.dentry = path->dentry;  // 更新nameidata结构中的目录项指针为新路径的目录项
}

/*
 * __do_follow_link - 跟随符号链接并处理相应的文件系统操作
 * @path: 包含目录项和挂载点信息的路径结构
 * @nd: nameidata结构，用于路径解析和状态跟踪
 * @p: 用于存储由follow_link操作返回的指针
 *
 * 此函数用于处理符号链接解析的核心步骤。它调用特定文件系统的follow_link方法，并
 * 管理路径和链接解析过程中的状态更新。
 * 返回值：0表示成功，非0表示错误代码。
 */
static __always_inline int
__do_follow_link(struct path *path, struct nameidata *nd, void **p)
{
	int error;
	struct dentry *dentry = path->dentry;

	touch_atime(path->mnt, dentry);  // 更新访问时间
	nd_set_link(nd, NULL);  // 初始化nameidata中的链接指针

	if (path->mnt != nd->path.mnt) {  // 如果挂载点发生变化
		path_to_nameidata(path, nd);  // 更新nd中的路径信息
		dget(dentry);  // 增加目录项的引用计数
	}
	mntget(path->mnt);  // 增加挂载点的引用计数
	nd->last_type = LAST_BIND;  // 设置链接类型为LAST_BIND
	*p = dentry->d_inode->i_op->follow_link(dentry, nd);  // 调用follow_link方法
	error = PTR_ERR(*p);  // 从指针中提取错误代码
	if (!IS_ERR(*p)) {  // 如果没有错误
		char *s = nd_get_link(nd);  // 获取解析后的链接路径
		error = 0;
		if (s)
			error = __vfs_follow_link(nd, s);  // 继续跟随链接
		else if (nd->last_type == LAST_BIND) {
			error = force_reval_path(&nd->path, nd);  // 强制重新验证路径
			if (error)
				path_put(&nd->path);	// 如果有错误，释放路径
		}
	}
	return error;	// 返回操作结果
}

/*
 * This limits recursive symlink follows to 8, while
 * limiting consecutive symlinks to 40.
 *
 * Without that kind of total limit, nasty chains of consecutive
 * symlinks can cause almost arbitrarily long lookups. 
 */
/*
 * do_follow_link - 处理符号链接的跟随操作
 * @path: 包含目录项和挂载点的路径结构
 * @nd: nameidata结构，用于保存路径解析过程中的状态
 *
 * 此函数用于处理符号链接的解析，实现对递归和连续符号链接的数量限制。
 * 递归符号链接的最大允许跟随次数限制为8次，连续符号链接的最大允许次数限制为40次。
 * 这样的限制是必需的，因为没有这种限制，连续的符号链接可能导致几乎任意长的查找。
 * 返回值：成功返回0，失败返回错误代码。
 */
static inline int do_follow_link(struct path *path, struct nameidata *nd)
{
	void *cookie;
	int err = -ELOOP;  // 默认错误设置为 -ELOOP，表示过多的符号链接
	if (current->link_count >= MAX_NESTED_LINKS)  // 检查当前递归符号链接的数量是否超过限制
		goto loop;
	if (current->total_link_count >= 40)  // 检查连续符号链接的数量是否超过限制
		goto loop;
	BUG_ON(nd->depth >= MAX_NESTED_LINKS);  // 断言，确保 nameidata 的深度没有超过限制
	cond_resched();  // 在需要时让出 CPU，允许其他任务运行
	err = security_inode_follow_link(path->dentry, nd);  // 安全模块检查链接跟随操作
	if (err)  // 如果检查失败
		goto loop;
	current->link_count++;  // 增加递归符号链接计数
	current->total_link_count++;  // 增加总符号链接计数
	nd->depth++;  // 增加 nameidata 的深度
	err = __do_follow_link(path, nd, &cookie);  // 实际进行链接跟随
	if (!IS_ERR(cookie) && path->dentry->d_inode->i_op->put_link)
		path->dentry->d_inode->i_op->put_link(path->dentry, nd, cookie);  // 使用链接的结束操作，如果有的话
	path_put(path);  // 释放路径
	current->link_count--;  // 减少递归符号链接计数
	nd->depth--;  // 减少 nameidata 的深度
	return err;  // 返回操作结果
loop:
	path_put_conditional(path, nd);  // 有条件地释放路径
	path_put(&nd->path);  // 释放 nameidata 中的路径
	return err;  // 返回错误代码
}

/*
 * follow_up - 向上遍历文件系统的挂载点
 * @path: 包含当前目录项和挂载点的路径结构
 *
 * 此函数用于向上遍历文件系统的挂载点，以找到当前路径所在挂载点的父挂载点。
 * 如果当前路径已经是最顶层挂载点，函数返回0。否则，更新路径以指向父挂载点，并返回1。
 */
int follow_up(struct path *path)
{
	struct vfsmount *parent;       // 父挂载点
	struct dentry *mountpoint;     // 挂载点的目录项
	spin_lock(&vfsmount_lock);     // 锁定全局的挂载点锁，确保挂载结构不被并发修改
	parent = path->mnt->mnt_parent;  // 获取当前路径挂载点的父挂载点
	if (parent == path->mnt) {       // 如果父挂载点与当前挂载点相同，表示已经到达顶层
		spin_unlock(&vfsmount_lock); // 解锁
		return 0;                    // 返回0，表示没有更上层的挂载点
	}
	mntget(parent);                  // 增加父挂载点的引用计数
	mountpoint = dget(path->mnt->mnt_mountpoint); // 获取当前挂载点的挂载目录项并增加其引用计数
	spin_unlock(&vfsmount_lock);     // 解锁
	dput(path->dentry);              // 释放原路径的目录项引用
	path->dentry = mountpoint;       // 更新路径的目录项为挂载点目录项
	mntput(path->mnt);               // 释放原路径的挂载点引用
	path->mnt = parent;              // 更新路径的挂载点为父挂载点

	return 1;                        // 返回1，表示成功向上遍历到父挂载点
}

/* no need for dcache_lock, as serialization is taken care in
 * namespace.c
 */
/*
 * __follow_mount - 跟随挂载点到实际的挂载目标
 * @path: 包含目录项和挂载点的路径结构
 *
 * 此函数检查给定的路径是否是一个挂载点，并且如果是，更新路径以指向挂载的目标。
 * 这个过程可能会多次迭代，如果存在多级挂载。
 * 由于 namespace.c 中已经处理了序列化，因此不需要 dcache_lock。
 * 返回值: 如果路径被更新到挂载的根目录，则返回 1，否则返回 0。
 */
static int __follow_mount(struct path *path)
{
	int res = 0;  // 结果初始化为 0，表示默认情况下路径没有更新
	while (d_mountpoint(path->dentry)) {  // 检查当前路径的目录项是否是一个挂载点
		struct vfsmount *mounted = lookup_mnt(path);  // 查找与当前挂载点对应的挂载结构
		if (!mounted)  // 如果没有找到对应的挂载结构
			break;  // 退出循环
		dput(path->dentry);  // 释放当前目录项的引用
		if (res)  // 如果之前已经进行了挂载点更新
			mntput(path->mnt);  // 释放前一个挂载点的引用
		path->mnt = mounted;  // 更新路径的挂载点为新找到的挂载点
		path->dentry = dget(mounted->mnt_root);  // 更新路径的目录项为新挂载点的根目录项，并增加其引用计数
		res = 1;  // 设置返回结果为 1，表示路径已更新
	}
	return res;  // 返回操作结果
}

/*
 * follow_mount - 跟随挂载点直到实际挂载的文件系统
 * @path: 包含目录项和挂载点的路径结构
 *
 * 此函数遍历路径中的挂载点，如果目录项是一个挂载点，它会更新路径以指向挂载的文件系统的根目录。
 * 这是处理文件系统挂载和联合文件系统时常见的操作，确保路径引用的是文件系统的实际位置而非挂载点的抽象层。
 */
static void follow_mount(struct path *path)
{
	while (d_mountpoint(path->dentry)) {  // 检查当前目录项是否是一个挂载点
		struct vfsmount *mounted = lookup_mnt(path);  // 查找当前挂载点的挂载信息
		if (!mounted)  // 如果没有找到挂载信息
			break;  // 结束循环
		dput(path->dentry);  // 释放当前目录项的引用
		mntput(path->mnt);  // 释放当前挂载点的引用
		path->mnt = mounted;  // 更新路径的挂载点为新找到的挂载点
		path->dentry = dget(mounted->mnt_root);  // 更新路径的目录项为新挂载点的根目录项，并增加其引用计数
	}
}

/* no need for dcache_lock, as serialization is taken care in
 * namespace.c
 */
/*
 * follow_down - 向下跟随到最低的挂载点
 * @path: 包含目录项和挂载点的路径结构
 *
 * 此函数用于寻找并更新路径到最底层的挂载点，如果当前路径是一个挂载点的话。
 * 这有助于确保操作指向文件系统的实际存放位置，而非挂载点的抽象层。
 * 由于 namespace.c 中已经处理了序列化，因此无需担心 dcache_lock。
 * 返回值：如果路径被更新到新的挂载点，则返回 1；如果路径未更新（即当前路径不是挂载点），返回 0。
 */
int follow_down(struct path *path)
{
	struct vfsmount *mounted;  // 指向挂载点的指针

	mounted = lookup_mnt(path);  // 查找当前路径的挂载点信息
	if (mounted) {  // 如果找到了挂载点
		dput(path->dentry);  // 释放当前路径目录项的引用
		mntput(path->mnt);  // 释放当前挂载点的引用
		path->mnt = mounted;  // 更新路径的挂载点为新的挂载点
		path->dentry = dget(mounted->mnt_root);  // 更新路径的目录项为新挂载点的根目录项，并增加其引用计数
		return 1;  // 返回 1，表示路径已更新到新的挂载点
	}
	return 0;  // 返回 0，表示当前路径不是挂载点，没有进行更新
}

/*
 * follow_dotdot - 在文件系统中向上遍历到父目录
 * @nd: nameidata结构，用于保存路径解析过程中的状态
 *
 * 此函数用于处理路径中的“..”（上级目录），即跟随路径向上至其父目录。
 * 它会检查当前路径是否已经是根目录或者挂载点的根目录。如果是，不再继续向上；
 * 否则，会继续向上至找到父目录或者到达根目录或挂载点的根。
 * 最终，这个函数也会跟随任何找到的挂载点，以确保路径正确指向实际文件系统位置。
 */
static __always_inline void follow_dotdot(struct nameidata *nd)
{
	set_root(nd);  // 设置或确认根路径，确保nd的根是当前有效的根路径

	while (1) {
		struct dentry *old = nd->path.dentry;  // 保存当前目录项

		// 检查是否已经位于根目录
		if (nd->path.dentry == nd->root.dentry &&
		    nd->path.mnt == nd->root.mnt) {
			break;  // 如果当前目录项和挂载点都是根目录，则停止循环
		}

		// 如果当前目录项不是其挂载点的根目录项，则获取其父目录项
		if (nd->path.dentry != nd->path.mnt->mnt_root) {
			/* rare case of legitimate dget_parent()... */
			// 正常情况下获取父目录项
			nd->path.dentry = dget_parent(nd->path.dentry);
			dput(old);  // 释放原来的目录项
			break;  // 跳出循环，因为已经移动到父目录项
		}

		// 如果当前目录项是挂载点的根目录项，尝试向上跟随到挂载点的父级
		if (!follow_up(&nd->path))
			break;  // 如果没有更上层的挂载点或父目录项，则停止循环
	}

	follow_mount(&nd->path);  // 跟随挂载点，确保路径正确指向实际文件系统位置
}

/*
 *  It's more convoluted than I'd like it to be, but... it's still fairly
 *  small and for now I'd prefer to have fast path as straight as possible.
 *  It _is_ time-critical.
 */
/*
 * do_lookup - 执行文件名查找
 * @nd: nameidata结构，包含当前的查找状态和路径
 * @name: 要查找的文件名
 * @path: 结果路径，查找成功时填充
 *
 * 此函数负责在文件系统中查找给定的文件名。它首先检查文件系统是否提供自定义的哈希函数，
 * 如果有，则使用该哈希函数。然后尝试在目录项缓存中查找目标文件。如果找到且无需重新验证，
 * 则直接返回。如果未找到或需要进一步的操作（如重新验证或从硬盘查找），则执行相应的步骤。
 * 返回值：0表示成功，负值表示错误代码。
 */
static int do_lookup(struct nameidata *nd, struct qstr *name,
		     struct path *path)
{
	struct vfsmount *mnt = nd->path.mnt;  // 保存当前路径的挂载点
	struct dentry *dentry, *parent;
	struct inode *dir;

	/*
	 * See if the low-level filesystem might want
	 * to use its own hash..
	 */
	// 检查文件系统是否定义了自己的哈希函数，如果是，使用它来计算哈希值
	if (nd->path.dentry->d_op && nd->path.dentry->d_op->d_hash) {
		int err = nd->path.dentry->d_op->d_hash(nd->path.dentry, name);
		if (err < 0)
			return err;  // 哈希函数错误，直接返回错误代码
	}

	// 在目录项缓存中查找文件
	dentry = __d_lookup(nd->path.dentry, name);
	if (!dentry)
		goto need_lookup;  // 没有找到，需要进一步查找
	if (dentry->d_op && dentry->d_op->d_revalidate)
		goto need_revalidate;  // 如果找到的目录项需要重新验证，执行重新验证
done:
	path->mnt = mnt;  // 设置返回路径的挂载点
	path->dentry = dentry;  // 设置返回路径的目录项
	__follow_mount(path);  // 跟踪可能的挂载点变化，确保返回最终有效的挂载点
	return 0;  // 返回成功

need_lookup:
	parent = nd->path.dentry;  // 当前目录项
	dir = parent->d_inode;  // 当前目录项的inode

	mutex_lock(&dir->i_mutex);  // 加锁，防止并发修改
	/*
	 * First re-do the cached lookup just in case it was created
	 * while we waited for the directory semaphore..
	 *
	 * FIXME! This could use version numbering or similar to
	 * avoid unnecessary cache lookups.
	 *
	 * The "dcache_lock" is purely to protect the RCU list walker
	 * from concurrent renames at this point (we mustn't get false
	 * negatives from the RCU list walk here, unlike the optimistic
	 * fast walk).
	 *
	 * so doing d_lookup() (with seqlock), instead of lockfree __d_lookup
	 */
	/*
   * 再次尝试缓存查找，以防在我们等待目录信号量时，所需的目录项已被创建。
   * FIXME! 这里可以使用版本号或类似机制来避免不必要的缓存查找。
   *
   * "dcache_lock" 主要用于保护 RCU 列表遍历免受并发重命名的影响。
   * 与乐观的快速遍历不同，这里的 RCU 列表遍历必须避免假阴性。
   *
   * 因此使用带 seqlock 的 d_lookup()，而不是无锁的 __d_lookup。
   */
	// 再次尝试从缓存中查找目录项，防止在等待锁的过程中目录项被创建
	dentry = d_lookup(parent, name);
	if (!dentry) {	// 如果仍未找到
		struct dentry *new;
		/* Don't create child dentry for a dead directory. */
		/* 不为已死的目录创建子目录项。 */
		// 检查目录是否已经死亡
		dentry = ERR_PTR(-ENOENT);
		// 如果目录已死，则不应继续创建子目录项
		if (IS_DEADDIR(dir))
			goto out_unlock;

		new = d_alloc(parent, name);  // 为该名称分配一个新的目录项
		dentry = ERR_PTR(-ENOMEM);
		if (new) {
			// 通过文件系统特定的 lookup 方法尝试查找或创建目录项
			dentry = dir->i_op->lookup(dir, new, nd);  // 文件系统查找文件
			if (dentry)
				dput(new);  // 如果 lookup() 返回已存在的目录项，释放新创建的目录项
			else
				dentry = new;  // 使用新创建的目录项
		}
out_unlock:
		mutex_unlock(&dir->i_mutex);	// 解锁
		if (IS_ERR(dentry))
			goto fail;	// 如果返回错误指针，则处理失败逻辑
		goto done;		// 完成处理，继续后续流程
	}

	/*
	 * Uhhuh! Nasty case: the cache was re-populated while
	 * we waited on the semaphore. Need to revalidate.
	 */
	/*
	 * 这一段处理了在获取目录信号量期间，缓存被重新填充的情况。需要重新验证目录项的有效性。
	 */
	// 解锁并根据需要重新验证目录项
	mutex_unlock(&dir->i_mutex);	// 解锁父目录的 inode
	if (dentry->d_op && dentry->d_op->d_revalidate) {	// 如果存在需要重新验证的操作
		dentry = do_revalidate(dentry, nd);	// 执行重新验证
		if (!dentry)	// 如果验证失败并且没有返回目录项
			dentry = ERR_PTR(-ENOENT);	// 创建一个错误指针，表示无法找到文件或目录
	}
	if (IS_ERR(dentry))	// 如果目录项是一个错误指针
		goto fail;	// 跳转到失败处理
	goto done;	// 完成处理，继续执行

/*
 * 如果在普通缓存查找后确定需要重新验证，执行以下代码。
 */
need_revalidate:
	dentry = do_revalidate(dentry, nd);	// 对目录项进行重新验证
	if (!dentry)	// 如果验证后没有返回有效的目录项
		goto need_lookup;  // 如果重新验证失败，需要重新查找
	if (IS_ERR(dentry))	// 如果返回的是错误指针
		goto fail;	// 跳转到失败处理
	goto done;	// 完成验证，继续执行

fail:
	return PTR_ERR(dentry);  // 返回错误代码
}

/*
 * This is a temporary kludge to deal with "automount" symlinks; proper
 * solution is to trigger them on follow_mount(), so that do_lookup()
 * would DTRT.  To be killed before 2.6.34-final.
 */
/*
 * 这是一个临时的权宜之计，用来处理“自动挂载”符号链接；正确的解决方案是在 follow_mount() 中触发它们，
 * 这样 do_lookup() 就会做正确的事情。这将在 2.6.34-final 版本之前被移除。
 */
static inline int follow_on_final(struct inode *inode, unsigned lookup_flags)
{
	// 首先检查 inode 是否存在
	return inode && 
		// 检查 inode 的操作是否包括 follow_link，这通常表示这是一个符号链接
		unlikely(inode->i_op->follow_link) &&
		// 确保如果设置了 LOOKUP_FOLLOW 标志或者 inode 代表一个目录，该函数返回 true
		((lookup_flags & LOOKUP_FOLLOW) || S_ISDIR(inode->i_mode));
}

/*
 * Name resolution.
 * This is the basic name resolution function, turning a pathname into
 * the final dentry. We expect 'base' to be positive and a directory.
 *
 * Returns 0 and nd will have valid dentry and mnt on success.
 * Returns error and drops reference to input namei data on failure.
 */
/*
 * link_path_walk - 解析文件路径名，将路径名转换为最终的目录项(dentry)
 * @name: 路径名字符串
 * @nd: 包含当前查找状态的nameidata结构
 *
 * 这是基本的名称解析函数，它将路径名转换为最终的目录项。
 * 我们期望'base'是有效的且为目录。
 * 成功时返回0，nd将包含有效的目录项和挂载点。
 * 失败时返回错误，并释放输入的namei数据的引用。
 */
static int link_path_walk(const char *name, struct nameidata *nd)
{
	struct path next;  // 用于存储下一步解析得到的路径
	struct inode *inode;  // 指向当前目录项的inode
	int err;  // 错误代码
	unsigned int lookup_flags = nd->flags;  // 查找标志

	// 跳过路径开始的所有斜杠，定位到第一个有效的路径组件
	while (*name == '/')
		name++;
	if (!*name)	// 如果处理完斜杠后没有其他字符，返回重验证标记
		goto return_reval;  // 如果路径为空，执行重验证

	inode = nd->path.dentry->d_inode;  // 获取当前路径的inode
	if (nd->depth)	// 如果处于递归查找中，修改查找标志
		lookup_flags = LOOKUP_FOLLOW | (nd->flags & LOOKUP_CONTINUE);  // 如果在深度搜索中，更新查找标志

	/* At this point we know we have a real path component. */
	// 主循环：解析路径的每一部分
	for(;;) {
		unsigned long hash;	// 用于存储路径组件的哈希值
		struct qstr this; 	// 当前处理的路径组件
		unsigned int c;			// 当前字符

		nd->flags |= LOOKUP_CONTINUE;  // 设置继续查找标志
		err = exec_permission(inode);  // 检查对当前inode的执行权限
 		if (err)
			break;

		this.name = name;  // 设置当前组件的起始位置
		c = *(const unsigned char *)name;  // 当前字符

		hash = init_name_hash();  // 初始化名称哈希
		do {
			name++;  // 移动到下一个字符
			hash = partial_name_hash(c, hash);  // 更新哈希值
			c = *(const unsigned char *)name;  // 获取新的当前字符
		} while (c && (c != '/'));  // 继续直到路径分隔符或字符串结束
		this.len = name - (const char *)this.name;	// 计算当前路径组件的长度
		this.hash = end_name_hash(hash);  // 完成当前路径组件的哈希计算

		/* remove trailing slashes? */
		/* 是否移除尾随的斜杠 */
		// 处理路径中的尾随斜杠
		if (!c)
			goto last_component;	// 如果当前字符为空，则跳转到处理最后一个组件
		while (*++name == '/');	// 跳过连续的斜杠
		if (!*name)
			goto last_with_slashes;	// 如果之后没有更多字符，处理最后一个带斜杠的组件

		/*
		 * "." and ".." are special - ".." especially so because it has
		 * to be able to know about the current root directory and
		 * parent relationships.
		 */
		/*
		 * 处理特殊的目录项 "." 和 ".."，其中 ".." 特别重要，因为它需要处理当前的根目录和父目录关系
		 */
		// 处理"."和".."，这些是特殊的目录项
		if (this.name[0] == '.') switch (this.len) {
			default:
				break;	// 默认情况不处理
			case 2:	
				if (this.name[1] != '.')
					break;	// 如果不是 ".."，则不处理
				follow_dotdot(nd);	 // 处理 ".."，即跟随到父目录
				inode = nd->path.dentry->d_inode;	// 更新inode为当前目录项的inode
				/* fallthrough */
			case 1:
				continue;	// 如果是 "."，则继续下一轮循环
		}
		/* This does the actual lookups.. */
		/* 执行实际的目录项查找 */
		err = do_lookup(nd, &this, &next);  // 使用当前组件执行查找
		if (err)
			break;  // 如果查找出错，中断循环

		err = -ENOENT;  // 默认设置错误码为 "没有该文件或目录"
		inode = next.dentry->d_inode;  // 获取查找结果的inode
		if (!inode)
			goto out_dput;  // 如果inode为空，即目录项未关联inode，处理释放

		if (inode->i_op->follow_link) {	// 如果inode操作中包含 follow_link，即需要处理符号链接
			err = do_follow_link(&next, nd);	// 如果是符号链接，执行跟随
			if (err)
				goto return_err;	// 如果处理出错，跳转到错误处理
			err = -ENOENT;	// 重设错误码
			inode = nd->path.dentry->d_inode;	// 更新inode为符号链接处理后的inode
			if (!inode)
				break;	// 如果inode为空，中断循环
		} else
			path_to_nameidata(&next, nd);	// 如果不是符号链接，更新nameidata的路径为查找结果
		err = -ENOTDIR;	// 设置错误代码为 "不是目录"
		if (!inode->i_op->lookup)
			break;	// 如果inode没有lookup操作，终止循环
		continue;	// 继续处理下一个路径组件
		/* here ends the main loop */
		/* 这里结束主循环 */

last_with_slashes:
		lookup_flags |= LOOKUP_FOLLOW | LOOKUP_DIRECTORY;	// 设置查找标志，指示跟随链接并期望目录
last_component:
		/* Clear LOOKUP_CONTINUE iff it was previously unset */
		/* 只在未设置LOOKUP_CONTINUE时清除该标志 */
		nd->flags &= lookup_flags | ~LOOKUP_CONTINUE;	// 调整查找标志，去除LOOKUP_CONTINUE，除非之前已设置
		if (lookup_flags & LOOKUP_PARENT)
			goto lookup_parent;	// 如果设置了LOOKUP_PARENT，跳转到处理父路径的部分
		if (this.name[0] == '.') switch (this.len) {
			default:
				break;	// 默认情况，不处理
			case 2:	
				if (this.name[1] != '.')
					break;	// 如果不是 ".."，不处理
				follow_dotdot(nd);	// 处理 ".."，回到父目录
				inode = nd->path.dentry->d_inode;	// 更新inode
				/* fallthrough */	
			case 1:
				goto return_reval;	// 如果是 "."，跳转到重验证步骤
		}
		err = do_lookup(nd, &this, &next);	// 对当前路径组件执行查找
		if (err)
			break;	// 如果查找出错，中断循环
		inode = next.dentry->d_inode;	// 获取查找到的目录项的inode
		if (follow_on_final(inode, lookup_flags)) {  // 检查是否需要跟随符号链接
			err = do_follow_link(&next, nd);  // 执行符号链接跟随
			if (err)
				goto return_err;  // 如果跟随链接出错，跳转到错误返回步骤
			inode = nd->path.dentry->d_inode;  // 更新inode
		} else
			path_to_nameidata(&next, nd);  // 更新nd以反映查找到的新路径
		
		err = -ENOENT;  // 设置错误代码为“文件或目录不存在”
		if (!inode)
			break;  // 如果inode为空，退出循环
		if (lookup_flags & LOOKUP_DIRECTORY) {
			err = -ENOTDIR;  // 设置错误代码为“不是目录”
			if (!inode->i_op->lookup)
				break;  // 如果inode没有lookup操作，退出循环
		}
		goto return_base;  // 跳转到返回基础路径处理

lookup_parent:
		nd->last = this;  // 设置nd的last为当前组件
		nd->last_type = LAST_NORM;  // 设置上一个类型为普通类型
		if (this.name[0] != '.')  // 如果当前组件不是'.'或'..'
			goto return_base;  // 返回基础路径处理
		if (this.len == 1)
			nd->last_type = LAST_DOT;  // 设置上一个类型为'.'
		else if (this.len == 2 && this.name[1] == '.')
			nd->last_type = LAST_DOTDOT;  // 设置上一个类型为'..'
		else
			goto return_base;  // 返回基础路径处理

return_reval:
		/*
		 * We bypassed the ordinary revalidation routines.
		 * We may need to check the cached dentry for staleness.
		 */
		/*
		 * 我们绕过了常规的重新验证程序。
		 * 我们可能需要检查缓存的目录项是否过时。
		 */
		if (nd->path.dentry && nd->path.dentry->d_sb &&
		    (nd->path.dentry->d_sb->s_type->fs_flags & FS_REVAL_DOT)) {
			err = -ESTALE;	// 设置错误代码为“陈旧”
			/* Note: we do not d_invalidate() */
			/* 注意: 我们不使用 d_invalidate() */
			if (!nd->path.dentry->d_op->d_revalidate(
					nd->path.dentry, nd))
				break;	// 如果重新验证失败，退出循环
		}
return_base:
		return 0;	// 返回0，表示成功处理
out_dput:
		path_put_conditional(&next, nd);	// 条件性地释放路径
		break;	// 退出循环
	}
	path_put(&nd->path);	// 释放nd的路径
return_err:
	return err;	// 返回错误代码
}

/*
 * path_walk - 路径解析的主函数，将用户提供的路径字符串转换为内核理解的路径结构
 * @name: 用户提供的路径字符串
 * @nd: 保存路径解析状态的nameidata结构
 *
 * 这个函数负责将一个字符串形式的路径名解析成内核能够操作的结构化路径。
 * 它通过调用 link_path_walk 来完成实际的路径解析，并处理特定的错误情况，如路径失效。
 */
static int path_walk(const char *name, struct nameidata *nd)
{
	struct path save = nd->path;  // 保存当前路径，以便恢复
	int result;  // 存储函数返回结果

	current->total_link_count = 0;  // 初始化当前进程的链接跟随计数器

	/* make sure the stuff we saved doesn't go away */
	/* 确保我们保存的路径不会消失 */
	path_get(&save);  // 增加保存路径的引用计数，防止在使用过程中被释放

	result = link_path_walk(name, nd);  // 调用 link_path_walk 来解析路径
	if (result == -ESTALE) {  // 如果返回结果表示路径失效
		/* nd->path had been dropped */
		/* nd->path 已被丢弃 */
		current->total_link_count = 0;  // 重置链接跟随计数器
		nd->path = save;  // 恢复原来的路径
		path_get(&nd->path);  // 增加路径的引用计数
		nd->flags |= LOOKUP_REVAL;  // 设置标志位，表示需要重新验证路径
		result = link_path_walk(name, nd);  // 再次尝试解析路径
	}

	path_put(&save);  // 释放先前保存的路径的引用

	return result;  // 返回解析结果
}

/*
 * path_init - 初始化文件名查找过程的起始路径
 * @dfd: 目录文件描述符，用于相对路径查找
 * @name: 要查找的路径名
 * @flags: 查找标志
 * @nd: 存储路径查找状态的nameidata结构
 *
 * 这个函数设置了nameidata结构的初始状态，基于给定的路径名和目录文件描述符。
 * 它处理绝对和相对路径的初始化，并确保起始点是有效的目录项。
 * 返回值为0表示成功，非0值表示错误代码。
 */
static int path_init(int dfd, const char *name, unsigned int flags, struct nameidata *nd)
{
	int retval = 0;  // 默认返回值为成功
	int fput_needed;  // 标记是否需要释放文件指针
	struct file *file;  // 用于存储从文件描述符获取的文件结构

	/* if there are only slashes... */
	nd->last_type = LAST_ROOT; /* 如果路径只有斜杠 */
	nd->flags = flags;  // 设置查找标志
	nd->depth = 0;  // 设置查找深度为0
	nd->root.mnt = NULL;  // 初始化根挂载点为空

	if (*name == '/') {  // 如果是绝对路径
		set_root(nd);  // 设置根目录
		nd->path = nd->root;  // 初始化路径为根目录
		path_get(&nd->root);  // 增加根目录的引用计数
	} else if (dfd == AT_FDCWD) {  // 如果是相对于当前工作目录的路径
		struct fs_struct *fs = current->fs;  // 获取当前进程的文件系统结构
		read_lock(&fs->lock);  // 获取读锁
		nd->path = fs->pwd;  // 设置路径为当前工作目录
		path_get(&fs->pwd);  // 增加当前工作目录的引用计数
		read_unlock(&fs->lock);  // 释放读锁
	} else {  // 如果是相对于特定文件描述符的路径
		struct dentry *dentry;

		file = fget_light(dfd, &fput_needed);  // 获取文件描述符指向的文件
		retval = -EBADF;  // 假设错误为“无效的文件描述符”
		if (!file)
			goto out_fail;  // 如果文件获取失败，跳到错误处理

		dentry = file->f_path.dentry;  // 获取文件对应的目录项

		retval = -ENOTDIR;  // 假设错误为“不是目录”
		if (!S_ISDIR(dentry->d_inode->i_mode))
			goto fput_fail;  // 如果文件不是目录，跳到文件释放处理

		retval = file_permission(file, MAY_EXEC);  // 检查执行权限
		if (retval)
			goto fput_fail;  // 如果没有执行权限，跳到文件释放处理

		nd->path = file->f_path;  // 设置路径为文件的路径
		path_get(&file->f_path);  // 增加文件路径的引用计数

		fput_light(file, fput_needed);  // 根据需要释放文件
	}
	return 0;  // 返回成功

fput_fail:
	fput_light(file, fput_needed);  // 释放文件
out_fail:
	return retval;  // 返回错误代码
}

/* Returns 0 and nd will be valid on success; Retuns error, otherwise. */
/*
 * do_path_lookup - 执行路径查找
 * @dfd: 目录文件描述符，用于相对路径查找
 * @name: 要查找的路径名
 * @flags: 查找标志
 * @nd: 存储路径查找状态的nameidata结构
 *
 * 这个函数整合了路径初始化和路径遍历的步骤，用于将路径名解析为文件系统中的具体节点。
 * 成功返回0，nd将包含有效的路径信息；失败则返回错误代码。
 */
static int do_path_lookup(int dfd, const char *name,
				unsigned int flags, struct nameidata *nd)
{
	int retval = path_init(dfd, name, flags, nd);  // 初始化路径查找，设置起始点
	if (!retval)  // 如果路径初始化成功
		retval = path_walk(name, nd);  // 执行路径遍历，解析整个路径

	// 如果路径遍历成功，且当前上下文不是一个空的审计上下文
	if (unlikely(!retval && !audit_dummy_context() && nd->path.dentry &&
				nd->path.dentry->d_inode))
		audit_inode(name, nd->path.dentry);  // 审计节点，记录访问信息

	if (nd->root.mnt) {  // 如果存在根挂载点
		path_put(&nd->root);  // 释放根路径的引用
		nd->root.mnt = NULL;  // 清除根挂载点信息
	}
	return retval;  // 返回结果，0表示成功，非0表示错误代码
}

/*
 * path_lookup - 对外公开的路径查找函数
 * @name: 要查找的路径字符串
 * @flags: 查找标志
 * @nd: 存储路径查找结果的nameidata结构
 *
 * 该函数是进行路径查找的简单封装，它调用 do_path_lookup 来处理实际的路径查找工作。
 * 返回值为 do_path_lookup 的返回结果，0表示成功，非0表示错误。
 */
int path_lookup(const char *name, unsigned int flags,
			struct nameidata *nd)
{
	return do_path_lookup(AT_FDCWD, name, flags, nd);
}

/*
 * kern_path - 用于内核模块进行路径查找的函数
 * @name: 要查找的路径字符串
 * @flags: 查找标志
 * @path: 存储找到的路径信息的结构
 *
 * 此函数是为内核模块或内部函数调用设计的路径查找接口，使用当前工作目录作为相对路径的基点。
 * 如果路径成功找到，路径信息被存储在提供的 'path' 结构中。
 * 返回值为 do_path_lookup 的返回结果，0表示成功，非0表示错误。
 */
int kern_path(const char *name, unsigned int flags, struct path *path)
{
	struct nameidata nd;  // 创建nameidata结构以存储查找过程中的数据
	int res = do_path_lookup(AT_FDCWD, name, flags, &nd);  // 调用 do_path_lookup 进行路径查找
	if (!res)
		*path = nd.path;  // 如果查找成功，将找到的路径信息复制到path参数指向的结构中
	return res;  // 返回查找结果
}

/**
 * vfs_path_lookup - lookup a file path relative to a dentry-vfsmount pair
 * @dentry:  pointer to dentry of the base directory
 * @mnt: pointer to vfs mount of the base directory
 * @name: pointer to file name
 * @flags: lookup flags
 * @nd: pointer to nameidata
 */
/**
 * vfs_path_lookup - 根据给定的目录项-虚拟文件系统挂载对查找文件路径
 * @dentry: 基目录的目录项指针
 * @mnt: 基目录的虚拟文件系统挂载点指针
 * @name: 文件名指针
 * @flags: 查找标志
 * @nd: nameidata结构的指针
 *
 * 该函数用于从指定的目录开始，根据给定的文件名进行路径查找。
 */
int vfs_path_lookup(struct dentry *dentry, struct vfsmount *mnt,
		    const char *name, unsigned int flags,
		    struct nameidata *nd)
{
	int retval;  // 用于存储函数返回值

	/* same as do_path_lookup */
	// 初始化nameidata结构
	nd->last_type = LAST_ROOT;  // 设置最后一部分类型为根部分
	nd->flags = flags;  // 设置查找标志
	nd->depth = 0;  // 设置查找深度为0

	nd->path.dentry = dentry;  // 设置当前路径的目录项为传入的dentry
	nd->path.mnt = mnt;  // 设置当前路径的挂载点为传入的mnt
	path_get(&nd->path);  // 增加当前路径的引用计数
	nd->root = nd->path;  // 将根路径设置为当前路径
	path_get(&nd->root);  // 增加根路径的引用计数

	retval = path_walk(name, nd);  // 执行路径遍历
	// 如果路径遍历成功，并且当前审计上下文不是哑元，且路径的目录项及其inode有效
	if (unlikely(!retval && !audit_dummy_context() && nd->path.dentry &&
				nd->path.dentry->d_inode))
		audit_inode(name, nd->path.dentry);  // 执行审计

	path_put(&nd->root);  // 释放根路径的引用计数
	nd->root.mnt = NULL;  // 清除根路径的挂载点信息

	return retval;  // 返回路径查找的结果
}

/*
 * __lookup_hash - 在特定的基目录下查找给定名称的目录项
 * @name: 查找的目录项名称
 * @base: 基目录项
 * @nd: nameidata结构，包含查找状态信息
 *
 * 此函数用于在文件系统中基于给定的基目录项查找一个名称对应的目录项。
 * 它尝试使用低层文件系统可能提供的哈希函数来优化查找过程。
 * 返回值为找到的目录项，或者错误指针。
 */
static struct dentry *__lookup_hash(struct qstr *name,
		struct dentry *base, struct nameidata *nd)
{
	struct dentry *dentry;  // 用于存储找到的目录项
	struct inode *inode;  // 基目录项的inode
	int err;  // 错误代码

	inode = base->d_inode;  // 获取基目录项的inode

	/*
	 * See if the low-level filesystem might want
	 * to use its own hash..
	 */
	/*
	 * 查看底层文件系统是否想使用自己的哈希函数。
	 */
	if (base->d_op && base->d_op->d_hash) {  // 如果定义了哈希函数
		err = base->d_op->d_hash(base, name);  // 计算哈希
		dentry = ERR_PTR(err);  // 将错误代码转换为错误指针
		if (err < 0)
			goto out;  // 如果有错误，退出
	}

	dentry = __d_lookup(base, name);  // 尝试无锁查找目录项

	/* lockess __d_lookup may fail due to concurrent d_move()
	 * in some unrelated directory, so try with d_lookup
	 */
	/* 由于并发的d_move()操作，无锁的__d_lookup可能会失败，
	   所以在一些无关的目录中尝试加锁的d_lookup */
	if (!dentry)
		dentry = d_lookup(base, name);  // 如果无锁查找失败，尝试加锁查找

	if (dentry && dentry->d_op && dentry->d_op->d_revalidate)  // 如果需要重新验证目录项
		dentry = do_revalidate(dentry, nd);  // 进行重新验证

	if (!dentry) {  // 如果目录项仍然未找到
		struct dentry *new;

		/* Don't create child dentry for a dead directory. */
		/* 对于已经标记为死亡的目录，不创建子目录项。 */
		dentry = ERR_PTR(-ENOENT);  // 设置错误指针为“不存在”
		if (IS_DEADDIR(inode))
			goto out;  // 如果目录已死，退出

		new = d_alloc(base, name);  // 为新名称分配一个目录项
		dentry = ERR_PTR(-ENOMEM);  // 设置错误指针为“内存不足”
		if (!new)
			goto out;  // 如果分配失败，退出
		dentry = inode->i_op->lookup(inode, new, nd);  // 在inode上执行查找操作
		if (!dentry)
			dentry = new;  // 如果没有错误，使用新目录项
		else
			dput(new);  // 如果返回了有效目录项，释放新分配的目录项
	}
out:
	return dentry;  // 返回找到的或错误的目录项
}

/*
 * Restricted form of lookup. Doesn't follow links, single-component only,
 * needs parent already locked. Doesn't follow mounts.
 * SMP-safe.
 */
/*
 * lookup_hash - 对目录项的查找进行限制。不跟随链接，只处理单个组件，
 *               需要父目录已被锁定。不跟随挂载点。
 *               在多处理器环境中是安全的。
 *
 * 这个函数是一个受限的目录项查找功能，仅用于特定的场景，如内核操作，
 * 其中父目录已经被锁定，不需要处理链接和挂载点的跟随。
 * @nd: 包含当前查找路径和状态的nameidata结构
 *
 * 返回值: 成功时返回目录项的指针，失败时返回错误指针。
 */
static struct dentry *lookup_hash(struct nameidata *nd)
{
	int err;  // 错误码

	// 检查执行权限
	err = exec_permission(nd->path.dentry->d_inode);  // 检查对当前目录项的执行权限
	if (err)
		return ERR_PTR(err);  // 如果权限检查失败，返回相应的错误指针

	// 调用 __lookup_hash 进行目录项查找
	return __lookup_hash(&nd->last, nd->path.dentry, nd);  // 调用更底层的查找函数处理具体的查找操作
}

/*
 * __lookup_one_len - 对单个名称组件的长度和内容进行处理，准备用于目录项查找
 * @name: 要查找的名称
 * @this: 结构体qstr，用于存储处理后的名称信息
 * @base: 基目录的目录项
 * @len: 名称的长度
 *
 * 这个函数处理一个给定长度的名称字符串，计算其哈希值，准备用于后续的目录项查找。
 * 名称中不允许包含 '/' 或 '\0'，这些字符会导致函数返回访问错误。
 * 返回0表示成功处理名称，非0值表示有错误发生。
 */
static int __lookup_one_len(const char *name, struct qstr *this,
		struct dentry *base, int len)
{
	unsigned long hash;  // 存储名称的哈希值
	unsigned int c;  // 存储当前处理的字符

	this->name = name;  // 设置qstr的name字段指向输入的名称
	this->len = len;  // 设置qstr的len字段为输入的长度
	if (!len)
		return -EACCES;  // 如果长度为0，返回访问错误

	hash = init_name_hash();  // 初始化哈希计算
	while (len--) {  // 对每个字符进行处理
		c = *(const unsigned char *)name++;  // 获取当前字符并移动指针
		if (c == '/' || c == '\0')  // 如果字符是'/'或'\0'
			return -EACCES;  // 返回访问错误，因为这些字符在名称中是不允许的
		hash = partial_name_hash(c, hash);  // 更新哈希值
	}
	this->hash = end_name_hash(hash);  // 完成哈希计算
	return 0;  // 返回成功
}

/**
 * lookup_one_len - filesystem helper to lookup single pathname component
 * @name:	pathname component to lookup
 * @base:	base directory to lookup from
 * @len:	maximum length @len should be interpreted to
 *
 * Note that this routine is purely a helper for filesystem usage and should
 * not be called by generic code.  Also note that by using this function the
 * nameidata argument is passed to the filesystem methods and a filesystem
 * using this helper needs to be prepared for that.
 */
/**
 * lookup_one_len - 文件系统辅助函数，用于查找单个路径名组件
 * @name: 需要查找的路径名组件
 * @base: 从哪个基目录开始查找
 * @len: 应该解释的最大长度 @len
 *
 * 注意，这个函数纯粹是为文件系统使用而设计的辅助工具，不应由通用代码调用。
 * 还要注意，使用这个函数时，nameidata参数会传递给文件系统方法，使用这个辅助工具的文件系统
 * 需要为此做好准备。
 */
struct dentry *lookup_one_len(const char *name, struct dentry *base, int len)
{
	int err;  // 错误码
	struct qstr this;  // 结构体，用于存储处理后的名称信息

	// 警告：仅当基目录的inode互斥锁已经被锁定时才应调用此函数
	WARN_ON_ONCE(!mutex_is_locked(&base->d_inode->i_mutex));

	// 处理输入的名称，检查长度和字符的有效性
	err = __lookup_one_len(name, &this, base, len);
	if (err)
		return ERR_PTR(err);  // 如果处理出错，返回错误指针

	// 检查对基目录的执行权限
	err = exec_permission(base->d_inode);
	if (err)
		return ERR_PTR(err);  // 如果没有执行权限，返回错误指针

	// 调用 __lookup_hash 查找处理后的名称在基目录中的目录项
	return __lookup_hash(&this, base, NULL);
}

/*
 * user_path_at - 从用户空间获取路径名，并进行路径查找
 * @dfd: 目录文件描述符，用于相对路径查找
 * @name: 用户空间的路径名指针
 * @flags: 查找标志
 * @path: 存储找到的路径信息的结构体
 *
 * 该函数接受一个用户空间的路径名，并在指定的文件描述符或当前工作目录下进行路径查找。
 * 返回0表示成功，返回错误码表示失败。
 */
int user_path_at(int dfd, const char __user *name, unsigned flags,
		 struct path *path)
{
	struct nameidata nd;  // nameidata结构，用于存储路径查找过程中的状态
	char *tmp = getname(name);  // 从用户空间获取路径名
	int err = PTR_ERR(tmp);  // 初始化错误码

	if (!IS_ERR(tmp)) {  // 如果获取路径名成功

		BUG_ON(flags & LOOKUP_PARENT);  // 确保不使用LOOKUP_PARENT标志，因为它不适用于此函数

		err = do_path_lookup(dfd, tmp, flags, &nd);  // 执行路径查找
		putname(tmp);  // 释放从用户空间获取的路径名

		if (!err)  // 如果路径查找成功
			*path = nd.path;  // 将找到的路径信息保存到path参数中
	}
	return err;  // 返回错误码或成功码
}

/*
 * user_path_parent - 从用户空间获取路径，并查找其父目录
 * @dfd: 目录文件描述符，用于相对路径查找
 * @path: 用户空间中的路径字符串
 * @nd: nameidata结构，存储查找的结果
 * @name: 输出参数，返回从用户空间获取的原始路径名
 *
 * 该函数从用户空间获取路径，并尝试查找该路径的父目录。
 * 返回0表示成功，否则返回错误码。
 */
static int user_path_parent(int dfd, const char __user *path,
			struct nameidata *nd, char **name)
{
	char *s = getname(path);  // 从用户空间获取路径字符串
	int error;

	if (IS_ERR(s))
		return PTR_ERR(s);  // 如果获取路径失败，返回对应的错误码

	error = do_path_lookup(dfd, s, LOOKUP_PARENT, nd);  // 执行路径查找，查找父目录
	if (error)
		putname(s);  // 如果查找失败，释放获取的路径字符串
	else
		*name = s;  // 如果查找成功，将路径字符串传回给调用者

	return error;  // 返回查找结果，成功或失败的错误码
}

/*
 * It's inline, so penalty for filesystems that don't use sticky bit is
 * minimal.
 */
/*
 * check_sticky - 检查粘滞位的权限
 * @dir: 目录的inode
 * @inode: 要操作的文件的inode
 *
 * 这个内联函数用于检查文件系统中粘滞位的权限问题。粘滞位主要用于特定目录（如/tmp），
 * 在这些目录中，只有文件的所有者或超级用户可以删除或重命名文件。
 * 如果文件系统不使用粘滞位，这个函数的开销很小。
 *
 * 返回0表示没有权限限制，返回非0表示没有删除或重命名的权限。
 */
static inline int check_sticky(struct inode *dir, struct inode *inode)
{
	uid_t fsuid = current_fsuid();  // 获取当前文件系统用户的UID

	if (!(dir->i_mode & S_ISVTX))  // 如果目录的模式中没有设置粘滞位
		return 0;  // 返回0，没有权限限制

	if (inode->i_uid == fsuid)  // 如果文件的所有者是当前用户
		return 0;  // 返回0，表示可以删除或重命名

	if (dir->i_uid == fsuid)  // 如果目录的所有者是当前用户
		return 0;  // 返回0，表示可以删除或重命名

	return !capable(CAP_FOWNER);  // 检查当前进程是否有CAP_FOWNER能力
	// 如果有CAP_FOWNER能力，返回0（无限制），否则返回1（有限制）
}

/*
 *	Check whether we can remove a link victim from directory dir, check
 *  whether the type of victim is right.
 *  1. We can't do it if dir is read-only (done in permission())
 *  2. We should have write and exec permissions on dir
 *  3. We can't remove anything from append-only dir
 *  4. We can't do anything with immutable dir (done in permission())
 *  5. If the sticky bit on dir is set we should either
 *	a. be owner of dir, or
 *	b. be owner of victim, or
 *	c. have CAP_FOWNER capability
 *  6. If the victim is append-only or immutable we can't do antyhing with
 *     links pointing to it.
 *  7. If we were asked to remove a directory and victim isn't one - ENOTDIR.
 *  8. If we were asked to remove a non-directory and victim isn't one - EISDIR.
 *  9. We can't remove a root or mountpoint.
 * 10. We don't allow removal of NFS sillyrenamed files; it's handled by
 *     nfs_async_unlink().
 */
/*
 * 检查是否可以从目录 dir 中移除一个链接受害者（victim），检查受害者的类型是否正确。
 *  1. 如果 dir 是只读的，我们不能进行删除操作（在 permission() 中完成）。
 *  2. 我们应该拥有 dir 的写和执行权限。
 *  3. 我们不能从仅追加的目录中移除任何东西。
 *  4. 我们不能对不可变目录执行任何操作（在 permission() 中完成）。
 *  5. 如果 dir 上设置了粘滞位，我们应该：
 *    a. 是 dir 的所有者，或
 *    b. 是受害者的所有者，或
 *    c. 拥有 CAP_FOWNER 能力。
 *  6. 如果受害者是仅追加或不可变的，我们不能对指向它的链接做任何事情。
 *  7. 如果我们被要求移除一个目录而受害者不是目录 - 返回 ENOTDIR。
 *  8. 如果我们被要求移除一个非目录而受害者是一个目录 - 返回 EISDIR。
 *  9. 我们不能移除根目录或挂载点。
 * 10. 我们不允许移除 NFS 傻傻重命名的文件；这是由 nfs_async_unlink() 处理的。
 */
/*
 * may_delete - 检查是否可以从目录dir中删除一个链接或目标victim
 * @dir: 目录的inode
 * @victim: 要删除的目标的dentry
 * @isdir: 指示删除请求是否针对目录
 *
 * 这个函数执行一系列检查，以确定是否可以删除目录中的文件或目录。
 * 返回0表示可以删除，否则返回相应的错误码。
 */
static int may_delete(struct inode *dir, struct dentry *victim, int isdir)
{
	int error;  // 错误码变量

	if (!victim->d_inode)  // 如果victim没有关联的inode，表示不存在
		return -ENOENT;  // 返回“不存在”错误

	// 确保victim的父目录项的inode与dir一致
	BUG_ON(victim->d_parent->d_inode != dir);
	audit_inode_child(victim, dir);  // 审计victim和dir的关系

	// 检查对dir的写和执行权限
	error = inode_permission(dir, MAY_WRITE | MAY_EXEC);
	if (error)  // 如果没有权限
		return error;
	if (IS_APPEND(dir))  // 如果dir是只追加目录
		return -EPERM;  // 返回“操作不允许”错误
	if (check_sticky(dir, victim->d_inode) || IS_APPEND(victim->d_inode) ||
	    IS_IMMUTABLE(victim->d_inode) || IS_SWAPFILE(victim->d_inode))
		return -EPERM;  // 如果有粘滞位等限制，返回“操作不允许”
	if (isdir) {  // 如果请求删除的是目录
		if (!S_ISDIR(victim->d_inode->i_mode))  // 如果victim不是目录
			return -ENOTDIR;  // 返回“不是目录”错误
		if (IS_ROOT(victim))  // 如果victim是根目录
			return -EBUSY;  // 返回“设备或资源忙”错误
	} else if (S_ISDIR(victim->d_inode->i_mode))  // 如果请求删除的不是目录，但victim是目录
		return -EISDIR;  // 返回“是目录”错误
	if (IS_DEADDIR(dir))  // 如果dir是死目录
		return -ENOENT;  // 返回“不存在”错误
	if (victim->d_flags & DCACHE_NFSFS_RENAMED)  // 如果victim是NFS临时重命名的文件
		return -EBUSY;  // 返回“设备或资源忙”错误
	return 0;  // 如果所有检查都通过，返回0表示可以删除
}

/*	Check whether we can create an object with dentry child in directory
 *  dir.
 *  1. We can't do it if child already exists (open has special treatment for
 *     this case, but since we are inlined it's OK)
 *  2. We can't do it if dir is read-only (done in permission())
 *  3. We should have write and exec permissions on dir
 *  4. We can't do it if dir is immutable (done in permission())
 */
/*
 * may_create - 检查是否可以在目录 dir 中创建带有目录项 child 的对象。
 * @dir: 操作所在的目录的 inode。
 * @child: 准备创建的目录项。
 *
 * 此函数执行一系列权限和状态检查，以确定是否可以在指定目录中创建新对象。
 * 返回0表示可以创建，非0表示错误代码。
 *
 *  1. 如果 child 已经存在（对于 open 操作有特殊处理，但由于此处是内联的，可以接受）
 *  2. 如果 dir 是只读的（在 permission() 中完成检查）
 *  3. 我们应该拥有 dir 的写和执行权限
 *  4. 如果 dir 是不可变的（在 permission() 中完成检查）
 */
static inline int may_create(struct inode *dir, struct dentry *child)
{
	if (child->d_inode)  // 如果 child 已存在
		return -EEXIST;  // 返回“已存在”错误
	if (IS_DEADDIR(dir))  // 如果 dir 是一个已废弃的目录
		return -ENOENT;  // 返回“不存在”错误
	return inode_permission(dir, MAY_WRITE | MAY_EXEC);  // 检查对 dir 的写和执行权限
}

/*
 * p1 and p2 should be directories on the same fs.
 */
/*
 * lock_rename - 锁定两个目录进行重命名操作
 * @p1: 第一个目录的目录项
 * @p2: 第二个目录的目录项
 *
 * p1 和 p2 应该是同一个文件系统上的目录。
 * 这个函数用于在进行重命名操作前锁定相关的目录项，确保操作的原子性和线程安全。
 */
struct dentry *lock_rename(struct dentry *p1, struct dentry *p2)
{
	struct dentry *p;

	if (p1 == p2) {  // 如果两个目录项相同
		mutex_lock_nested(&p1->d_inode->i_mutex, I_MUTEX_PARENT);  // 锁定一个互斥锁，防止死锁
		return NULL;  // 返回空，因为不需要锁定两次
	}

	mutex_lock(&p1->d_inode->i_sb->s_vfs_rename_mutex);  // 锁定重命名互斥锁

	p = d_ancestor(p2, p1);  // 检查p2是否是p1的祖先
	if (p) {  // 如果p2是p1的祖先
		mutex_lock_nested(&p2->d_inode->i_mutex, I_MUTEX_PARENT);  // 先锁p2
		mutex_lock_nested(&p1->d_inode->i_mutex, I_MUTEX_CHILD);  // 再锁p1
		return p;  // 返回p2的目录项
	}

	p = d_ancestor(p1, p2);  // 检查p1是否是p2的祖先
	if (p) {  // 如果p1是p2的祖先
		mutex_lock_nested(&p1->d_inode->i_mutex, I_MUTEX_PARENT);  // 先锁p1
		mutex_lock_nested(&p2->d_inode->i_mutex, I_MUTEX_CHILD);  // 再锁p2
		return p;  // 返回p1的目录项
	}

	mutex_lock_nested(&p1->d_inode->i_mutex, I_MUTEX_PARENT);  // 锁定p1
	mutex_lock_nested(&p2->d_inode->i_mutex, I_MUTEX_CHILD);  // 锁定p2
	return NULL;  // 如果没有祖先关系，返回空
}

/*
 * unlock_rename - 解锁两个目录项的重命名操作
 * @p1: 第一个目录的目录项
 * @p2: 第二个目录的目录项
 *
 * 该函数用于在完成重命名操作后解锁之前由 lock_rename 函数锁定的目录项。
 * 它确保所有之前获得的锁都被适当释放。
 */
void unlock_rename(struct dentry *p1, struct dentry *p2)
{
	mutex_unlock(&p1->d_inode->i_mutex);  // 解锁第一个目录的互斥锁
	if (p1 != p2) {  // 如果两个目录不是同一个
		mutex_unlock(&p2->d_inode->i_mutex);  // 解锁第二个目录的互斥锁
		mutex_unlock(&p1->d_inode->i_sb->s_vfs_rename_mutex);  // 解锁文件系统级的重命名互斥锁
	}
}

/*
 * vfs_create - 在虚拟文件系统中创建一个新的文件
 * @dir: 包含文件的目录的inode
 * @dentry: 用于新文件的目录项
 * @mode: 文件的模式和权限
 * @nd: 调用中使用的nameidata结构，可能包含相关数据
 *
 * 此函数用于在文件系统中创建新文件。它首先检查是否有权限在指定目录中创建文件，
 * 然后调用特定文件系统的create操作来实际创建文件。
 * 返回0表示成功，非0表示有错误发生。
 */
int vfs_create(struct inode *dir, struct dentry *dentry, int mode,
		struct nameidata *nd)
{
	int error = may_create(dir, dentry);  // 检查是否允许在目录中创建文件

	if (error)  // 如果检查发现错误
		return error;  // 返回错误码

	if (!dir->i_op->create)  // 如果目录的inode操作中没有create方法
		/* shouldn't it be ENOSYS? */
		return -EACCES;	// 返回“拒绝访问”错误（可能应该返回ENOSYS，“不支持的操作”）

	mode &= S_IALLUGO;  // 清除mode中除基本权限位外的所有位
	mode |= S_IFREG;  // 设置文件类型为普通文件
	error = security_inode_create(dir, dentry, mode);  // 安全模块检查创建操作
	if (error)  // 如果安全模块报告错误
		return error;  // 返回错误码

	error = dir->i_op->create(dir, dentry, mode, nd);  // 调用文件系统的create方法创建文件
	if (!error)  // 如果文件创建成功
		fsnotify_create(dir, dentry);  // 发送文件系统通知

	return error;  // 返回操作结果，成功为0，失败为相应错误码
}

/*
 * may_open - 检查文件是否可以被打开
 * @path: 包含待打开文件的目录项和inode的路径
 * @acc_mode: 请求的访问模式（读、写、执行）
 * @flag: 打开文件时使用的标志（例如 O_RDONLY，O_WRONLY）
 *
 * 该函数对文件执行访问前的各种检查，以确定是否允许打开文件。
 * 返回0表示成功，非0为错误代码。
 */
int may_open(struct path *path, int acc_mode, int flag)
{
	struct dentry *dentry = path->dentry;  // 文件的目录项
	struct inode *inode = dentry->d_inode;  // 文件的inode
	int error;  // 错误代码

	if (!inode)  // 如果inode不存在
		return -ENOENT;  // 返回“不存在”错误

	// 根据文件类型进行检查
	switch (inode->i_mode & S_IFMT) {  // 检查文件类型
	case S_IFLNK:  // 符号链接
		return -ELOOP;  // 返回“太多层次的符号链接”错误
	case S_IFDIR:  // 目录
		if (acc_mode & MAY_WRITE)  // 如果请求写权限
			return -EISDIR;  // 返回“是目录”错误
		break;
	case S_IFBLK:  // 块设备
	case S_IFCHR:  // 字符设备
		if (path->mnt->mnt_flags & MNT_NODEV)  // 如果挂载时设置了禁止设备访问
			return -EACCES;  // 返回“拒绝访问”错误
		/*FALLTHRU*/
	case S_IFIFO:  // FIFO
	case S_IFSOCK:  // 套接字
		flag &= ~O_TRUNC;  // 忽略O_TRUNC标志，因为这些类型不支持截断操作
		break;
	}

	error = inode_permission(inode, acc_mode);  // 检查inode的访问权限
	if (error)
		return error;

	/*
	 * An append-only file must be opened in append mode for writing.
	 */
	/*
	 * 只追加文件必须在追加模式下打开才能写。
	 */
	if (IS_APPEND(inode)) {
		if ((flag & O_ACCMODE) != O_RDONLY && !(flag & O_APPEND))
			return -EPERM;  // 如果文件是只追加的，但没有以追加模式打开，返回“操作不允许”
		if (flag & O_TRUNC)
			return -EPERM;  // 如果尝试截断只追加文件，返回“操作不允许”
	}

	/* O_NOATIME can only be set by the owner or superuser */
	/* 只有文件所有者或超级用户才能设置O_NOATIME */
	if (flag & O_NOATIME && !is_owner_or_cap(inode))
		return -EPERM;  // 如果尝试设置O_NOATIME但权限不足，返回“操作不允许”

	/*
	 * Ensure there are no outstanding leases on the file.
	 */
	/*
	 * 确保没有文件的租约问题。
	 */
	return break_lease(inode, flag);  // 检查并可能中断文件的租约
}

/*
 * handle_truncate - 处理文件截断
 * @path: 包含待截断文件的目录项和inode的路径
 *
 * 该函数用于截断文件到指定长度（这里为0，即完全清空文件内容）。
 * 返回0表示成功，非0表示错误代码。
 */
static int handle_truncate(struct path *path)
{
	struct inode *inode = path->dentry->d_inode;  // 获取文件的inode
	int error = get_write_access(inode);  // 获取写入权限
	if (error)
		return error;  // 如果获取写入权限失败，直接返回错误

	/*
	 * Refuse to truncate files with mandatory locks held on them.
	 */
	/*
	 * 拒绝截断在其上持有强制锁的文件。
	 */
	error = locks_verify_locked(inode);  // 验证文件是否有持有的锁
	if (!error)  // 如果没有锁，继续执行
		error = security_path_truncate(path, 0,
				       ATTR_MTIME|ATTR_CTIME|ATTR_OPEN);  // 安全模块检查截断操作
	if (!error) {  // 如果安全检查通过
		error = do_truncate(path->dentry, 0,  // 执行截断操作
				    ATTR_MTIME|ATTR_CTIME|ATTR_OPEN,
				    NULL);
	}
	put_write_access(inode);  // 释放之前获得的写入权限
	return error;  // 返回执行结果
}

/*
 * Be careful about ever adding any more callers of this
 * function.  Its flags must be in the namei format, not
 * what get passed to sys_open().
 */
/*
 * 注意添加这个函数的调用者。它的标志必须是namei格式的，而不是
 * 传递给sys_open()的那种。
 */
/*
 * __open_namei_create - 用于处理文件的创建逻辑
 * @nd: nameidata结构，包含关于查找文件的信息
 * @path: 包含待创建文件的dentry和inode的路径
 * @open_flag: 用于文件打开的标志
 * @mode: 创建文件时的权限模式
 *
 * 这个函数专门用于创建文件。在调用此函数前，必须确保传递的标志符合namei的格式，
 * 而不是直接传递给sys_open()的标志。谨慎添加这个函数的更多调用者。
 */
static int __open_namei_create(struct nameidata *nd, struct path *path,
				int open_flag, int mode)
{
	int error;  // 错误码
	struct dentry *dir = nd->path.dentry;  // 获取目录的dentry

	if (!IS_POSIXACL(dir->d_inode))  // 如果不支持POSIX ACL
		mode &= ~current_umask();  // 应用umask调整文件模式
	error = security_path_mknod(&nd->path, path->dentry, mode, 0);  // 安全检查：尝试创建节点
	if (error)
		goto out_unlock;  // 如果有错误，跳到解锁部分

	error = vfs_create(dir->d_inode, path->dentry, mode, nd);  // 在VFS层创建文件
out_unlock:
	mutex_unlock(&dir->d_inode->i_mutex);  // 解锁目录的互斥锁
	dput(nd->path.dentry);  // 减少dentry的引用计数
	nd->path.dentry = path->dentry;  // 更新nameidata结构的dentry指向新创建的文件
	if (error)
		return error;  // 如果创建过程中出现错误，返回错误码

	/* Don't check for write permission, don't truncate */
	// 检查打开文件的权限，但不截断文件
	return may_open(&nd->path, 0, open_flag & ~O_TRUNC);
}

/*
 * Note that while the flag value (low two bits) for sys_open means:
 *	00 - read-only
 *	01 - write-only
 *	10 - read-write
 *	11 - special
 * it is changed into
 *	00 - no permissions needed
 *	01 - read-permission
 *	10 - write-permission
 *	11 - read-write
 * for the internal routines (ie open_namei()/follow_link() etc)
 * This is more logical, and also allows the 00 "no perm needed"
 * to be used for symlinks (where the permissions are checked
 * later).
 *
*/
/*
 * 注意，虽然 sys_open 的标志值（低两位）意味着：
 *	00 - 只读
 *	01 - 只写
 *	10 - 读写
 *	11 - 特殊
 * 但在内部例程中（例如 open_namei()/follow_link() 等）它被转换为
 *	00 - 不需要权限
 *	01 - 需要读权限
 *	10 - 需要写权限
 *	11 - 需要读写权限
 * 这种转换更符合逻辑，也允许使用 00 “不需要权限”
 * 用于符号链接（权限检查将在稍后进行）。
 */
static inline int open_to_namei_flags(int flag)
{
	// 如果标志位不仅仅是读写模式（即包括其他权限或选项），则递增flag值
	if ((flag+1) & O_ACCMODE)
		flag++;
	return flag;  // 返回转换后的标志
}

/*
 * open_will_truncate - 检查打开文件操作是否会截断文件
 * @flag: 打开文件时使用的标志
 * @inode: 文件的inode
 *
 * 此函数用于确定基于给定标志和文件类型，文件打开操作是否会导致文件被截断。
 * 返回1表示将会截断文件，0表示不会。
 */
static int open_will_truncate(int flag, struct inode *inode)
{
	/*
	 * We'll never write to the fs underlying
	 * a device file.
	 */
	/*
	 * 我们永远不会写入设备文件下的文件系统。
	 */
	if (special_file(inode->i_mode))  // 如果inode代表一个特殊文件（如字符设备、块设备等）
		return 0;  // 返回0，因为我们不会截断设备文件
	return (flag & O_TRUNC);  // 检查打开标志中是否包含O_TRUNC，如果包含则返回非0（通常为1），表示会截断文件
}

/*
 * finish_open - 完成文件打开过程
 * @nd: nameidata 结构，包含有关文件路径和状态的信息
 * @open_flag: 打开文件时使用的标志
 * @acc_mode: 请求的访问模式（例如读、写）
 *
 * 这个函数用于完成文件打开过程，包括检查是否需要截断文件、处理挂载点写权限、
 * 验证打开权限以及创建文件对象。如果成功，返回文件对象；如果失败，返回错误。
 */
static struct file *finish_open(struct nameidata *nd,
				int open_flag, int acc_mode)
{
	struct file *filp;
	int will_truncate;
	int error;

	// 检查是否需要根据打开标志截断文件
	will_truncate = open_will_truncate(open_flag, nd->path.dentry->d_inode);
	if (will_truncate) {
        // 如果需要截断，则获取挂载点写权限
		error = mnt_want_write(nd->path.mnt);
		if (error)
			goto exit;  // 如果获取写权限失败，跳到退出处理
	}
	// 检查是否允许打开文件
	error = may_open(&nd->path, acc_mode, open_flag);
	if (error) {
		if (will_truncate)
			mnt_drop_write(nd->path.mnt);  // 如果打开失败且之前获取了写权限，释放写权限
		goto exit;  // 跳到退出处理
	}
	// 从nameidata结构创建文件对象
	filp = nameidata_to_filp(nd);
	if (!IS_ERR(filp)) {
		// 对文件执行 IMA 安全检查
		error = ima_file_check(filp, acc_mode);
		if (error) {
			fput(filp);  // 如果安全检查失败，释放文件对象
			filp = ERR_PTR(error);  // 设置错误
		}
	}
	if (!IS_ERR(filp)) {
		if (will_truncate) {
			// 处理文件截断
			error = handle_truncate(&nd->path);
			if (error) {
				fput(filp);  // 如果截断失败，释放文件对象
				filp = ERR_PTR(error);  // 设置错误
			}
		}
	}
	/*
	 * It is now safe to drop the mnt write
	 * because the filp has had a write taken
	 * on its behalf.
	 */
	/*
	 * 现在可以安全地释放挂载点的写权限，
	 * 因为已经为文件对象获取了写权限。
	 */
	if (will_truncate)
		mnt_drop_write(nd->path.mnt);
	return filp;  // 返回文件对象

exit:
	if (!IS_ERR(nd->intent.open.file))
		release_open_intent(nd);  // 释放打开意图
	path_put(&nd->path);  // 释放路径
	return ERR_PTR(error);  // 返回错误
}

/*
 * do_last - 完成文件打开或创建的最后步骤
 * @nd: 包含解析路径信息的nameidata结构
 * @path: 要打开或创建的文件路径
 * @open_flag: 打开文件的标志
 * @acc_mode: 请求的访问模式（读、写、执行）
 * @mode: 文件创建时的权限
 * @pathname: 文件的路径名
 *
 * 此函数负责处理文件打开或创建的最后阶段。根据不同的文件类型和请求操作，
 * 执行不同的逻辑，例如直接打开、创建、处理符号链接等。
 */
static struct file *do_last(struct nameidata *nd, struct path *path,
			    int open_flag, int acc_mode,
			    int mode, const char *pathname)
{
	struct dentry *dir = nd->path.dentry; // 当前目录的目录项
	struct file *filp; // 最终要返回的文件对象
	int error = -EISDIR; // 初始错误设置为“是一个目录”

	// 根据最后一个组件的类型进行不同的处理
	switch (nd->last_type) {
	case LAST_DOTDOT:	// 处理“..”，即上一级目录
		follow_dotdot(nd);	// 更新nameidata结构到上一级目录
		dir = nd->path.dentry;	// 重新获取当前的目录项
		// 如果文件系统要求对目录项进行重新验证
		if (nd->path.mnt->mnt_sb->s_type->fs_flags & FS_REVAL_DOT) {
			if (!dir->d_op->d_revalidate(dir, nd)) {
				error = -ESTALE; // 重新验证失败
				goto exit;
			}
		}
		/* fallthrough */
	case LAST_DOT:	// 处理“.”
	case LAST_ROOT: // 处理根目录
		if (open_flag & O_CREAT)	// 如果指定了创建标志但位置是特殊目录，则退出
			goto exit;
		/* fallthrough */
	case LAST_BIND: // 处理绑定点
		audit_inode(pathname, dir); // 审计文件
		goto ok;	// 处理成功
	}

	/* trailing slashes? */
	// 处理路径末尾的斜线（如果有）
	if (nd->last.name[nd->last.len]) {
		if (open_flag & O_CREAT)	// 如果指定了创建且路径以斜线结束，则退出
			goto exit;
		nd->flags |= LOOKUP_DIRECTORY | LOOKUP_FOLLOW;	// 设置查找标志为目录且跟随链接
	}

	/* just plain open? */
	/* 仅仅是普通打开？ */
	// 纯粹的打开，不创建
	if (!(open_flag & O_CREAT)) {  // 如果没有指定创建标志
		error = do_lookup(nd, &nd->last, path);  // 执行查找
		if (error)
			goto exit;  // 如果查找失败，跳到退出处理
		error = -ENOENT;  // 设置文件不存在的错误
		if (!path->dentry->d_inode)
			goto exit_dput;  // 如果找不到inode，跳到释放dentry并退出的处理
		if (path->dentry->d_inode->i_op->follow_link)
			return NULL;  // 如果是符号链接，直接返回NULL
		error = -ENOTDIR;  // 设置不是目录的错误
		if (nd->flags & LOOKUP_DIRECTORY) {  // 如果需要目录查找
			if (!path->dentry->d_inode->i_op->lookup)
				goto exit_dput;  // 如果没有查找操作，跳到释放dentry并退出的处理
		}
		path_to_nameidata(path, nd);  // 更新nameidata结构
		audit_inode(pathname, nd->path.dentry);  // 审计inode
		goto ok;  // 处理成功
	}


	/* OK, it's O_CREAT */
	/* 好的，这是一个创建操作 */
	// 创建文件
	mutex_lock(&dir->d_inode->i_mutex);  // 锁定目录的互斥锁

	path->dentry = lookup_hash(nd);  // 通过hash查找获取dentry
	path->mnt = nd->path.mnt;  // 设置挂载点

	error = PTR_ERR(path->dentry);  // 获取PTR错误
	if (IS_ERR(path->dentry)) {  // 如果dentry有错误
		mutex_unlock(&dir->d_inode->i_mutex);  // 解锁互斥锁
		goto exit;  // 跳到退出处理
	}

	if (IS_ERR(nd->intent.open.file)) {  // 如果打开意图中的文件有错误
		error = PTR_ERR(nd->intent.open.file);  // 获取文件错误
		goto exit_mutex_unlock;  // 跳到解锁并退出的处理
	}

	/* Negative dentry, just create the file */
	/* 如果dentry是负的，即文件不存在，那么直接创建文件 */
	// 如果dentry为负，即文件不存在，创建文件
	if (!path->dentry->d_inode) {	// 检查dentry是否有关联的inode，如果没有则表示文件不存在
		/*
		 * This write is needed to ensure that a
		 * ro->rw transition does not occur between
		 * the time when the file is created and when
		 * a permanent write count is taken through
		 * the 'struct file' in nameidata_to_filp().
		 */
		/*
		 * 这次写操作是必需的，以确保在文件创建和通过 nameidata_to_filp() 中的
		 * 'struct file' 进行永久写计数期间，不会发生只读到可写的转换。
		 */
		error = mnt_want_write(nd->path.mnt);  // 请求写权限，以准备写入文件系统
		if (error)
			goto exit_mutex_unlock;  // 如果获取写权限失败，跳转到解锁并退出处理
		error = __open_namei_create(nd, path, open_flag, mode);  // 尝试创建文件
		if (error) {  // 如果创建失败
			mnt_drop_write(nd->path.mnt);  // 释放写权限
			goto exit;  // 跳到退出处理
		}
		filp = nameidata_to_filp(nd);  // 从nameidata结构转换为文件结构
		mnt_drop_write(nd->path.mnt);  // 创建成功后，释放写权限
		if (!IS_ERR(filp)) {  // 如果文件结构有效
			error = ima_file_check(filp, acc_mode);  // 进行IMA安全检查
			if (error) {  // 如果安全检查失败
				fput(filp);  // 释放文件
				filp = ERR_PTR(error);  // 设置错误
			}
		}
		return filp;  // 返回文件结构
	}

	/*
	 * It already exists.
	 */
	// 文件已存在
	mutex_unlock(&dir->d_inode->i_mutex);  // 解锁目录的互斥锁
	audit_inode(pathname, path->dentry);  // 审计inode

	error = -EEXIST;  // 设置文件已存在的错误码
	if (open_flag & O_EXCL)  // 如果打开标志包含排他性打开
		goto exit_dput;  // 跳到释放dentry并退出的处理

	if (__follow_mount(path)) {  // 如果路径是一个挂载点，尝试跟随它
		error = -ELOOP;  // 设置错误为“太多的符号链接”
		if (open_flag & O_NOFOLLOW)  // 如果标志指定不跟随符号链接
			goto exit_dput;  // 跳到释放dentry并退出的处理
	}

	error = -ENOENT;  // 设置文件不存在的错误
	if (!path->dentry->d_inode)  // 如果dentry没有关联的inode
		goto exit_dput;  // 跳到释放dentry并退出的处理

	if (path->dentry->d_inode->i_op->follow_link)  // 如果inode有跟随链接的操作
		return NULL;  // 直接返回NULL，外部逻辑将处理链接

	path_to_nameidata(path, nd);  // 更新nameidata结构以反映新路径
	error = -EISDIR;  // 设置错误为“是一个目录”
	if (S_ISDIR(path->dentry->d_inode->i_mode))  // 如果inode是一个目录
		goto exit;  // 跳到退出处理
ok:
	filp = finish_open(nd, open_flag, acc_mode);  // 完成文件的打开
	return filp;  // 返回文件指针

exit_mutex_unlock:
	mutex_unlock(&dir->d_inode->i_mutex);  // 解锁目录的互斥锁
exit_dput:
	path_put_conditional(path, nd);  // 条件释放路径
exit:
	if (!IS_ERR(nd->intent.open.file))  // 如果打开意图中的文件有效
		release_open_intent(nd);  // 释放打开意图
	path_put(&nd->path);  // 释放路径
	return ERR_PTR(error);  // 返回错误指针
}

/*
 * Note that the low bits of the passed in "open_flag"
 * are not the same as in the local variable "flag". See
 * open_to_namei_flags() for more details.
 */
/* 请注意，传入的 "open_flag" 的低位并不与局部变量 "flag" 相同。有关详细信息，请参见 open_to_namei_flags()。 */
struct file *do_filp_open(int dfd, const char *pathname,
		int open_flag, int mode, int acc_mode)
{
	struct file *filp;
	struct nameidata nd;
	int error;
	struct path path;
	int count = 0;
	int flag = open_to_namei_flags(open_flag);	// 转换open_flag为内部使用的标志
	int force_reval = 0;

	if (!(open_flag & O_CREAT))
		mode = 0;	// 如果不是创建操作，将模式设为0

	/*
	 * O_SYNC is implemented as __O_SYNC|O_DSYNC.  As many places only
	 * check for O_DSYNC if the need any syncing at all we enforce it's
	 * always set instead of having to deal with possibly weird behaviour
	 * for malicious applications setting only __O_SYNC.
	 */
	/*
	 * O_SYNC 是实现为 __O_SYNC|O_DSYNC。由于许多地方仅在需要任何同步时检查 O_DSYNC，
	 * 我们强制始终设置它，而不是让可能存在恶意应用程序仅设置 __O_SYNC 导致的可能怪异行为。
	 */
	if (open_flag & __O_SYNC)
		open_flag |= O_DSYNC;	// 强制开启 O_DSYNC 以确保数据同步

	if (!acc_mode)
		acc_mode = MAY_OPEN | ACC_MODE(open_flag);	// 设置访问模式

	/* O_TRUNC implies we need access checks for write permissions */
	/* O_TRUNC 表示我们需要对写权限进行访问检查 */
	if (open_flag & O_TRUNC)
		acc_mode |= MAY_WRITE;	// 加入写权限需求

	/* Allow the LSM permission hook to distinguish append 
	   access from general write access. */
	/* 允许 LSM 权限挂钩区分附加访问与一般写访问 */
	if (open_flag & O_APPEND)
		acc_mode |= MAY_APPEND;	// 加入追加权限需求

	/* find the parent */
	/* 寻找父目录 */
reval:
	error = path_init(dfd, pathname, LOOKUP_PARENT, &nd);	// 初始化路径，如果出错则返回错误指针
	if (error)
		return ERR_PTR(error);	// 错误处理
	if (force_reval)
		nd.flags |= LOOKUP_REVAL;	// 如果需要强制重新验证，则设置相应标志

	current->total_link_count = 0;	// 重置链接计数
	error = link_path_walk(pathname, &nd);	// 遍历路径
	if (error) {
		filp = ERR_PTR(error);	// 进行路径遍历，如果出错则返回错误指针
		goto out;
	}
	if (unlikely(!audit_dummy_context()) && (open_flag & O_CREAT))
		audit_inode(pathname, nd.path.dentry);	// 如果进行创建操作，进行审计

	/*
	 * We have the parent and last component.
	 */
	/*
	 * 我们已经有了父目录和最后一个组件。
	 */

	error = -ENFILE;	// 设置文件打开错误码
	filp = get_empty_filp();  // 获取一个空闲的文件结构
	if (filp == NULL)
		goto exit_parent;  // 如果无法获取，处理退出
	nd.intent.open.file = filp;  // 设置意图为打开文件
	filp->f_flags = open_flag;  // 设置文件标志
	nd.intent.open.flags = flag;  // 设置打开标志
	nd.intent.open.create_mode = mode;  // 设置创建模式
	nd.flags &= ~LOOKUP_PARENT;	// 清除查找父目录的标志
	nd.flags |= LOOKUP_OPEN;  // 设置为打开文件的查找模式
	if (open_flag & O_CREAT) {
		nd.flags |= LOOKUP_CREATE;  // 如果是创建操作，设置创建标志
		if (open_flag & O_EXCL)
			nd.flags |= LOOKUP_EXCL;  // 如果是独占创建，设置独占标志
	}
	if (open_flag & O_DIRECTORY)
		nd.flags |= LOOKUP_DIRECTORY;  // 如果是打开目录，设置目录标志
	if (!(open_flag & O_NOFOLLOW))
		nd.flags |= LOOKUP_FOLLOW;	// 如果不是禁止链接跟踪，设置跟踪标志
	filp = do_last(&nd, &path, open_flag, acc_mode, mode, pathname);  // 执行最后的打开操作
	/* 处理尾随符号链接 */
	while (unlikely(!filp)) { /* trailing symlink */
		struct path holder;
		struct inode *inode = path.dentry->d_inode;
		void *cookie;
		error = -ELOOP;
		/* S_ISDIR part is a temporary automount kludge */
		/* S_ISDIR 部分是临时的自动挂载解决方案 */
		/* 链接层次太深，错误处理 */
		if (!(nd.flags & LOOKUP_FOLLOW) && !S_ISDIR(inode->i_mode))
			goto exit_dput;	/* 如果不跟踪链接且非目录，则退出 */
		if (count++ == 32)
			goto exit_dput;	/* 防止符号链接无限循环，限制循环次数 */
		/*
		 * This is subtle. Instead of calling do_follow_link() we do
		 * the thing by hands. The reason is that this way we have zero
		 * link_count and path_walk() (called from ->follow_link)
		 * honoring LOOKUP_PARENT.  After that we have the parent and
		 * last component, i.e. we are in the same situation as after
		 * the first path_walk().  Well, almost - if the last component
		 * is normal we get its copy stored in nd->last.name and we will
		 * have to putname() it when we are done. Procfs-like symlinks
		 * just set LAST_BIND.
		 */
		/*
		 * 这是微妙之处。与其调用 do_follow_link()，我们更倾向于手动处理。
		 * 原因是这样我们可以保持 link_count 为零，并且 path_walk()
		 * （从 ->follow_link 调用）会考虑 LOOKUP_PARENT。之后我们有了父目录
		 * 和最后一个组件，即我们处于 path_walk() 第一次之后的相同情况。
		 * 好吧，几乎一样 - 如果最后一个组件是正常的，我们会得到它的副本存储在 nd->last.name 中，
		 * 并且在完成后我们需要 putname() 它。类似 procfs 的符号链接只设置 LAST_BIND。
		 */
		nd.flags |= LOOKUP_PARENT;
		error = security_inode_follow_link(path.dentry, &nd);	// 安全检查跟随链接
		if (error)
			goto exit_dput;
		error = __do_follow_link(&path, &nd, &cookie);	// 执行跟随链接
		if (unlikely(error)) {
			/* nd.path had been dropped */
			/* nd.path 已被释放 */
			if (!IS_ERR(cookie) && inode->i_op->put_link)
				inode->i_op->put_link(path.dentry, &nd, cookie);
			/* 清理资源 */
			path_put(&path);
			release_open_intent(&nd);
			filp = ERR_PTR(error);
			goto out;
		}
		holder = path;
		nd.flags &= ~LOOKUP_PARENT;
		 /* 继续处理最后一个组件 */
		filp = do_last(&nd, &path, open_flag, acc_mode, mode, pathname);	// 再次处理最后一个组件
		if (inode->i_op->put_link)
			/* 如果有必要，调用put_link */
			inode->i_op->put_link(holder.dentry, &nd, cookie);
		path_put(&holder);	 /* 释放持有的路径 */
	}
out:
	if (nd.root.mnt)
		path_put(&nd.root);	/* 如果使用了根路径，则释放根路径引用。 */
	if (filp == ERR_PTR(-ESTALE) && !force_reval) {
		force_reval = 1;
		goto reval;	/* 如果文件描述符为-ESTALE且没有强制重验证，设置重验证标志并跳转到重验证步骤。 */
	}
	return filp;	/* 返回文件描述符或错误指针。 */

exit_dput:
	path_put_conditional(&path, &nd);	/* 如果有条件的释放路径，这通常发生在成功获取路径但后续操作失败的情况下。 */
	if (!IS_ERR(nd.intent.open.file))
		release_open_intent(&nd);	/* 如果打开意图没有错误，则释放打开意图资源。 */
exit_parent:
	path_put(&nd.path);	/* 释放获取的父路径资源。 */
	filp = ERR_PTR(error);	/* 设置错误指针，将错误码转换为错误指针形式返回。 */
	goto out;	/* 跳转到出口处理代码，执行一些清理工作。 */
}

/**
 * filp_open - open file and return file pointer
 *
 * @filename:	path to open
 * @flags:	open flags as per the open(2) second argument
 * @mode:	mode for the new file if O_CREAT is set, else ignored
 *
 * This is the helper to open a file from kernelspace if you really
 * have to.  But in generally you should not do this, so please move
 * along, nothing to see here..
 */
/**
 * filp_open - open file and return file pointer
 * 打开文件并返回文件指针
 *
 * @filename: path to open
 * @filename: 要打开的路径
 * @flags: open flags as per the open(2) second argument
 * @flags: 打开标志，与open(2)系统调用的第二个参数相同
 * @mode: mode for the new file if O_CREAT is set, else ignored
 * @mode: 如果设置了O_CREAT，则为新文件指定的模式，否则忽略
 *
 * This is the helper to open a file from kernelspace if you really
 * have to. But in generally you should not do this, so please move
 * along, nothing to see here..
 * 这是一个从内核空间打开文件的辅助函数，如果你确实需要的话可以使用它。但通常你不应该这么做，请继续前行，这里没有什么可看的。
 */
struct file *filp_open(const char *filename, int flags, int mode)
{
	// 调用 do_filp_open 函数实际执行打开文件操作，使用当前工作目录作为相对路径的基点。
	return do_filp_open(AT_FDCWD, filename, flags, mode, 0);
}
EXPORT_SYMBOL(filp_open);

/**
 * lookup_create - lookup a dentry, creating it if it doesn't exist
 * @nd: nameidata info
 * @is_dir: directory flag
 *
 * Simple function to lookup and return a dentry and create it
 * if it doesn't exist.  Is SMP-safe.
 *
 * Returns with nd->path.dentry->d_inode->i_mutex locked.
 */
/**
 * lookup_create - lookup a dentry, creating it if it doesn't exist
 * 查找一个目录项，如果不存在则创建它
 * @nd: nameidata info
 * @nd: nameidata信息
 * @is_dir: directory flag
 * @is_dir: 是否为目录的标志
 *
 * Simple function to lookup and return a dentry and create it
 * if it doesn't exist. Is SMP-safe.
 * 这是一个简单的函数，用于查找并返回一个目录项，如果不存在则创建它。该函数是SMP安全的。
 *
 * Returns with nd->path.dentry->d_inode->i_mutex locked.
 * 返回时，nd->path.dentry->d_inode->i_mutex将被锁定。
 */
struct dentry *lookup_create(struct nameidata *nd, int is_dir)
{
	struct dentry *dentry = ERR_PTR(-EEXIST);

	// 锁定当前目录项的互斥锁，以便安全地创建或查找目录项。
	mutex_lock_nested(&nd->path.dentry->d_inode->i_mutex, I_MUTEX_PARENT);
	/*
	 * Yucky last component or no last component at all?
	 * (foo/., foo/.., /////)
	 */
	/*
	 * 最后一个组件是不是不合法或者不存在？
	 * （例如 foo/., foo/.., ///// 等）
	 */
	if (nd->last_type != LAST_NORM)
		goto fail;	// 如果最后一个组件的类型不是普通类型，则跳转到失败处理。
	// 设置 nameidata 的标志以创建新的目录项，并排他地打开它。
	nd->flags &= ~LOOKUP_PARENT;
	nd->flags |= LOOKUP_CREATE | LOOKUP_EXCL;
	nd->intent.open.flags = O_EXCL;

	/*
	 * Do the final lookup.
	 */
	/*
	 * 执行最终的查找。
	 */
	dentry = lookup_hash(nd);	// 查找目录项。
	if (IS_ERR(dentry))
		goto fail;	// 如果查找失败，则跳转到失败处理。

	if (dentry->d_inode)
		goto eexist;	// 如果目录项已存在，则跳转到已存在处理。
	/*
	 * Special case - lookup gave negative, but... we had foo/bar/
	 * From the vfs_mknod() POV we just have a negative dentry -
	 * all is fine. Let's be bastards - you had / on the end, you've
	 * been asking for (non-existent) directory. -ENOENT for you.
	 */
	/*
	 * 特殊情况 - 查找返回了不存在的结果，但是... 我们有 foo/bar/
	 * 从 vfs_mknod() 的角度看，我们只是得到了一个不存在的目录项 -
	 * 一切正常。让我们严格一点 - 你在末尾加了 /，你在请求一个不存在的目录。返回 -ENOENT。
	 */
	if (unlikely(!is_dir && nd->last.name[nd->last.len])) {
		dput(dentry);
		dentry = ERR_PTR(-ENOENT);	// 如果不是目录但路径以 / 结尾，返回错误 -ENOENT。
	}
	return dentry;
eexist:
	// 处理存在错误或失败的情况。
	dput(dentry);
	dentry = ERR_PTR(-EEXIST);
fail:
	return dentry;
}
EXPORT_SYMBOL_GPL(lookup_create);

/**
 * vfs_mknod - 创建一个设备文件
 * @dir: 父目录的 inode
 * @dentry: 目标文件的 dentry
 * @mode: 文件模式和类型
 * @dev: 设备的设备号
 *
 * 返回：成功时返回0，失败时返回错误代码
 */

int vfs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
	int error = may_create(dir, dentry);	// 检查是否有权限在指定目录下创建文件

	if (error)
		return error;	// 如果没有权限，返回错误代码

	if ((S_ISCHR(mode) || S_ISBLK(mode)) && !capable(CAP_MKNOD))
		return -EPERM;	// 如果文件是字符或块设备，但调用者没有CAP_MKNOD权限，返回权限错误

	if (!dir->i_op->mknod)
		return -EPERM;	// 如果父目录的 inode 没有 mknod 操作，返回权限错误

	error = devcgroup_inode_mknod(mode, dev);	// 设备控制组对设备创建的额外权限检查
	if (error)
		return error;	// 如果检查失败，返回错误代码

	error = security_inode_mknod(dir, dentry, mode, dev);	// 安全模块进行 mknod 操作的安全检查
	if (error)
		return error;	// 如果检查失败，返回错误代码

	error = dir->i_op->mknod(dir, dentry, mode, dev);	// 调用具体的 mknod 操作，创建设备文件
	if (!error)
		fsnotify_create(dir, dentry);	// 如果创建成功，发送文件系统通知
	return error;	// 返回操作结果，0为成功，非0为错误代码
}

/**
 * may_mknod - 检查是否可以创建特定类型的文件节点
 * @mode: 欲创建的文件类型和权限
 *
 * 返回：如果允许创建，则返回0；如果不允许创建或类型无效，则返回负值错误代码。
 */
static int may_mknod(mode_t mode)
{
	switch (mode & S_IFMT) {  // 检查文件类型位
	case S_IFREG:  // 常规文件
	case S_IFCHR:  // 字符设备
	case S_IFBLK:  // 块设备
	case S_IFIFO:  // FIFO（命名管道）
	case S_IFSOCK: // 套接字
	// 如果mode为0，视为常规文件
	case 0: /* zero mode translates to S_IFREG */
		return 0;  // 这些文件类型是允许创建的，返回0表示成功
	case S_IFDIR:  // 目录
		return -EPERM;  // 不允许通过 mknod 创建目录，返回权限错误
	default:  // 任何其他非法的文件类型
		return -EINVAL;  // 返回无效参数错误
	}
}

/**
 * mknodat - 在指定目录下创建文件系统节点
 * @dfd: 目录文件描述符
 * @filename: 用户空间提供的文件名
 * @mode: 指定创建节点的类型和权限
 * @dev: 设备号（对于设备文件）
 *
 * 根据提供的参数创建一个文件系统节点。
 * 返回：成功时返回0，失败时返回负值错误代码。
 */
SYSCALL_DEFINE4(mknodat, int, dfd, const char __user *, filename, int, mode,
		unsigned, dev)
{
	int error;  // 错误码
	char *tmp;  // 临时文件名存储
	struct dentry *dentry;  // 目录项
	struct nameidata nd;  // 名字解析数据结构

	if (S_ISDIR(mode))  // 检查是否尝试创建目录，mknodat不允许创建目录
		return -EPERM;  // 返回错误：操作不允许

	error = user_path_parent(dfd, filename, &nd, &tmp);  // 获取文件的父目录路径
	if (error)
		return error;  // 如果出错，直接返回错误码

	dentry = lookup_create(&nd, 0);  // 在获取的路径上查找或创建一个新的目录项
	if (IS_ERR(dentry)) {  // 检查目录项是否有误
		error = PTR_ERR(dentry);  // 获取错误码
		goto out_unlock;  // 跳到解锁步骤
	}
	if (!IS_POSIXACL(nd.path.dentry->d_inode))
		mode &= ~current_umask();  // 如果不支持POSIX ACL，应用umask
	error = may_mknod(mode);  // 检查是否允许创建此类型的文件节点
	if (error)
		goto out_dput;  // 出错则进行清理
	error = mnt_want_write(nd.path.mnt);  // 尝试获取写权限
	if (error)
		goto out_dput;  // 获取写权限失败，进行清理
	error = security_path_mknod(&nd.path, dentry, mode, dev);  // 安全检查
	if (error)
		goto out_drop_write;  // 安全检查失败，释放写权限
	switch (mode & S_IFMT) {  // 根据文件类型进行不同的操作
		case 0: case S_IFREG:
			error = vfs_create(nd.path.dentry->d_inode, dentry, mode, &nd);  // 创建普通文件
			break;
		case S_IFCHR: case S_IFBLK:
			error = vfs_mknod(nd.path.dentry->d_inode, dentry, mode,
					new_decode_dev(dev));  // 创建字符或块设备文件
			break;
		case S_IFIFO: case S_IFSOCK:
			error = vfs_mknod(nd.path.dentry->d_inode, dentry, mode, 0);  // 创建FIFO或套接字文件
			break;
	}
out_drop_write:
	mnt_drop_write(nd.path.mnt);  // 释放写权限
out_dput:
	dput(dentry);  // 释放目录项
out_unlock:
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);  // 解锁inode
	path_put(&nd.path);  // 释放路径
	putname(tmp);  // 释放临时文件名资源

	return error;  // 返回错误码
}

/**
 * mknod - 创建设备文件、FIFO或套接字
 * @filename: 用户空间传入的文件名
 * @mode: 创建节点的类型和权限
 * @dev: 设备号（对于设备文件）
 *
 * 该系统调用在指定路径下创建一个特殊文件（设备文件、FIFO、套接字等）。
 * 返回：成功时返回0，失败时返回负值错误代码。
 */
SYSCALL_DEFINE3(mknod, const char __user *, filename, int, mode, unsigned, dev)
{
	return sys_mknodat(AT_FDCWD, filename, mode, dev);  // 调用 mknodat 实现具体功能，使用当前工作目录作为相对路径的基础
}

/**
 * vfs_mkdir - 在文件系统中创建目录
 * @dir: 父目录的inode
 * @dentry: 目标目录项
 * @mode: 新目录的权限
 *
 * 创建一个新目录。该函数首先检查是否有创建目录的权限，然后调用文件系统特定的 mkdir 操作。
 * 返回：成功时返回0，失败时返回负值错误代码。
 */
int vfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	int error = may_create(dir, dentry);  // 检查是否有权限在指定目录下创建新目录

	if (error)
		return error;  // 如果检查失败，返回错误码

	if (!dir->i_op->mkdir)
		return -EPERM;  // 如果文件系统不支持创建目录操作，返回错误

	mode &= (S_IRWXUGO|S_ISVTX);  // 应用掩码，保留有效的权限位和粘滞位
	error = security_inode_mkdir(dir, dentry, mode);  // 进行安全模块检查
	if (error)
		return error;  // 安全检查失败，返回错误

	error = dir->i_op->mkdir(dir, dentry, mode);  // 调用文件系统特定的mkdir操作
	if (!error)
		fsnotify_mkdir(dir, dentry);  // 如果创建成功，发送目录创建事件通知
	return error;  // 返回操作结果
}

/**
 * mkdirat - 在指定的目录文件描述符下创建一个新目录
 * @dfd: 目录文件描述符，用于相对路径查找
 * @pathname: 要创建的目录的路径
 * @mode: 新目录的权限设置
 *
 * 返回：成功返回0，失败返回负值错误码。
 */
SYSCALL_DEFINE3(mkdirat, int, dfd, const char __user *, pathname, int, mode)
{
	int error = 0;  // 初始化错误变量
	char *tmp;  // 用于保存用户空间传来的路径名
	struct dentry *dentry;  // 目录项结构
	struct nameidata nd;  // 解析路径时用的nameidata结构

	error = user_path_parent(dfd, pathname, &nd, &tmp);  // 获取路径名的父目录
	if (error)
		goto out_err;  // 错误处理

	dentry = lookup_create(&nd, 1);  // 在父目录中查找或创建新的目录项
	error = PTR_ERR(dentry);
	if (IS_ERR(dentry))
		goto out_unlock;  // 如果出错，进行错误处理

	if (!IS_POSIXACL(nd.path.dentry->d_inode))
		mode &= ~current_umask();  // 如果文件系统不支持POSIX ACL，应用umask
	error = mnt_want_write(nd.path.mnt);  // 请求写权限
	if (error)
		goto out_dput;  // 错误处理
	error = security_path_mkdir(&nd.path, dentry, mode);  // 安全模块检查
	if (error)
		goto out_drop_write;  // 错误处理
	error = vfs_mkdir(nd.path.dentry->d_inode, dentry, mode);  // 调用具体的mkdir实现函数
out_drop_write:
	mnt_drop_write(nd.path.mnt);  // 释放写权限
out_dput:
	dput(dentry);  // 释放目录项
out_unlock:
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);  // 解锁inode
	path_put(&nd.path);  // 释放路径
	putname(tmp);  // 释放临时路径名
out_err:
	return error;  // 返回错误码
}

/**
 * mkdir - 在当前工作目录下创建一个新目录
 * @pathname: 要创建的目录的路径
 * @mode: 新目录的权限设置
 *
 * 返回：成功返回0，失败返回负值错误码。
 */
SYSCALL_DEFINE2(mkdir, const char __user *, pathname, int, mode)
{
	return sys_mkdirat(AT_FDCWD, pathname, mode);  // 调用 mkdirat 实现，使用当前工作目录作为基准
}

/*
 * We try to drop the dentry early: we should have
 * a usage count of 2 if we're the only user of this
 * dentry, and if that is true (possibly after pruning
 * the dcache), then we drop the dentry now.
 *
 * A low-level filesystem can, if it choses, legally
 * do a
 *
 *	if (!d_unhashed(dentry))
 *		return -EBUSY;
 *
 * if it cannot handle the case of removing a directory
 * that is still in use by something else..
 */
/**
 * 尝试提前删除目录项：如果我们是这个目录项的唯一用户，
 * 那么它的使用计数应为2，如果真是这样（可能在修剪目录缓存后），
 * 那么我们现在就删除这个目录项。
 *
 * 低级文件系统可以，如果它选择，合法地执行
 *
 *	if (!d_unhashed(dentry))
 *		return -EBUSY;
 *
 * 如果它不能处理删除一个仍然被其他东西使用的目录的情况。
 */
void dentry_unhash(struct dentry *dentry)
{
	dget(dentry);  // 增加目录项的引用计数
	shrink_dcache_parent(dentry);  // 尝试减少父目录项的缓存大小
	spin_lock(&dcache_lock);  // 锁定目录项缓存
	spin_lock(&dentry->d_lock);  // 锁定目录项
	if (atomic_read(&dentry->d_count) == 2)  // 如果引用计数为2
		__d_drop(dentry);  // 从目录项缓存中移除目录项
	spin_unlock(&dentry->d_lock);  // 解锁目录项
	spin_unlock(&dcache_lock);  // 解锁目录项缓存
}

// 用于从虚拟文件系统中删除一个目录。
int vfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int error = may_delete(dir, dentry, 1);  // 检查是否有权限删除目录

	if (error)
		return error;  // 如果没有权限，返回错误

	if (!dir->i_op->rmdir)
		return -EPERM;  // 如果文件系统不支持rmdir操作，返回错误

	mutex_lock(&dentry->d_inode->i_mutex);  // 锁定目录项
	dentry_unhash(dentry);  // 尝试从目录缓存中移除目录项
	if (d_mountpoint(dentry))
		error = -EBUSY;  // 如果目录项是一个挂载点，返回忙碌
	else {
		error = security_inode_rmdir(dir, dentry);  // 安全模块检查
		if (!error) {
			error = dir->i_op->rmdir(dir, dentry);  // 调用具体文件系统的rmdir操作
			if (!error) {
				dentry->d_inode->i_flags |= S_DEAD;  // 标记inode为已删除
				dont_mount(dentry);  // 阻止在此dentry上挂载
			}
		}
	}
	mutex_unlock(&dentry->d_inode->i_mutex);  // 解锁目录项
	if (!error) {
		d_delete(dentry);  // 从目录项缓存中删除目录项
	}
	dput(dentry);  // 释放目录项引用

	return error;  // 返回操作结果
}

// 用于删除一个目录
static long do_rmdir(int dfd, const char __user *pathname)
{
	int error = 0;
	char * name;
	struct dentry *dentry;
	struct nameidata nd;

	error = user_path_parent(dfd, pathname, &nd, &name);  // 获取路径名的父目录
	if (error)
		return error;

	switch(nd.last_type) {  // 检查最后一个路径组件的类型
	case LAST_DOTDOT:  // ".."
		error = -ENOTEMPTY;  // 不允许删除“..”
		goto exit1;
	case LAST_DOT:  // "."
		error = -EINVAL;  // 不允许删除“.”
		goto exit1;
	case LAST_ROOT:  // "/"
		error = -EBUSY;  // 不允许删除根目录
		goto exit1;
	}

	nd.flags &= ~LOOKUP_PARENT;

	mutex_lock_nested(&nd.path.dentry->d_inode->i_mutex, I_MUTEX_PARENT);  // 锁定父目录的互斥锁
	dentry = lookup_hash(&nd);  // 查找目录项
	error = PTR_ERR(dentry);
	if (IS_ERR(dentry))
		goto exit2;
	error = mnt_want_write(nd.path.mnt);  // 检查是否可以写入挂载点
	if (error)
		goto exit3;
	error = security_path_rmdir(&nd.path, dentry);  // 安全模块检查是否可以删除目录
	if (error)
		goto exit4;
	error = vfs_rmdir(nd.path.dentry->d_inode, dentry);  // 调用 VFS rmdir
exit4:
	mnt_drop_write(nd.path.mnt);  // 释放挂载点的写权限
exit3:
	dput(dentry);  // 释放目录项的引用
exit2:
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);  // 解锁互斥锁
exit1:
	path_put(&nd.path);  // 释放路径
	putname(name);  // 释放路径名
	return error;
}

SYSCALL_DEFINE1(rmdir, const char __user *, pathname)
{
	return do_rmdir(AT_FDCWD, pathname);  // 执行实际的 rmdir 操作
}

// 用于从文件系统中删除一个文件链接
int vfs_unlink(struct inode *dir, struct dentry *dentry)
{
	int error = may_delete(dir, dentry, 0);  // 检查是否允许删除这个文件

	if (error)
		return error;  // 如果不允许，返回错误

	if (!dir->i_op->unlink)
		return -EPERM;  // 如果文件系统不支持 unlink 操作，返回错误

	mutex_lock(&dentry->d_inode->i_mutex);  // 锁定文件的 inode，防止并发访问
	if (d_mountpoint(dentry))
		error = -EBUSY;  // 如果该 dentry 是一个挂载点，则返回忙碌错误
	else {
		error = security_inode_unlink(dir, dentry);  // 安全模块检查是否允许 unlink 操作
		if (!error) {
			error = dir->i_op->unlink(dir, dentry);  // 调用文件系统的 unlink 操作
			if (!error)
				dont_mount(dentry);  // 如果删除成功，设置 dentry 为不可挂载
		}
	}
	mutex_unlock(&dentry->d_inode->i_mutex);  // 解锁 inode

	/* We don't d_delete() NFS sillyrenamed files--they still exist. */
	// 如果文件成功删除，并且文件不是 NFS 的 sillyrenamed 文件，那么删除 dentry
	if (!error && !(dentry->d_flags & DCACHE_NFSFS_RENAMED)) {
		fsnotify_link_count(dentry->d_inode);  // 通知系统文件链接数已更改
		d_delete(dentry);  // 从 dcache 中删除 dentry
	}

	return error;  // 返回操作结果
}

/*
 * Make sure that the actual truncation of the file will occur outside its
 * directory's i_mutex.  Truncate can take a long time if there is a lot of
 * writeout happening, and we don't want to prevent access to the directory
 * while waiting on the I/O.
 */
/*
 * 确保文件的实际截断发生在其目录的 i_mutex 外部。如果有大量的写出操作，截断可能需要很长时间，
 * 我们不希望在等待 I/O 时阻止访问目录。
 */
static long do_unlinkat(int dfd, const char __user *pathname)
{
	int error; // 用于存储错误码
	char *name; // 用于存储从用户空间解析的文件名
	struct dentry *dentry; // 目录项结构体，代表文件系统中的一个点
	struct nameidata nd; // 解析路径时使用的结构体
	struct inode *inode = NULL; // 文件的 inode 结构体

	// 解析用户提供的路径，获取其父目录和名称
	error = user_path_parent(dfd, pathname, &nd, &name);  // 解析文件的父目录
	if (error)
		return error;  // 如果出错，直接返回错误

	// 如果路径最后一个组件不是正常文件（例如尝试删除“..”）
	error = -EISDIR;  // 默认错误为目录错误，因为不允许对目录使用 unlink
	if (nd.last_type != LAST_NORM)  // 检查路径的最后一个组件是否正常
		goto exit1;	// 如果不是正常的文件名，跳到错误处理

	nd.flags &= ~LOOKUP_PARENT;  // 清除 LOOKUP_PARENT 标志，因为我们需要具体的文件

	mutex_lock_nested(&nd.path.dentry->d_inode->i_mutex, I_MUTEX_PARENT);  // 锁定父目录的互斥锁
	dentry = lookup_hash(&nd);  // 查找目录项
	error = PTR_ERR(dentry);  // 获取可能的错误码
	if (!IS_ERR(dentry)) {  // 如果没有错误
		/* Why not before? Because we want correct error value */
		// 检查是否有路径分隔符的错误
		if (nd.last.name[nd.last.len])	// 检查路径是否异常结束（如多余的斜杠）
			goto slashes;
		inode = dentry->d_inode;  // 获取目录项的 inode
		if (inode)
			atomic_inc(&inode->i_count);  // 增加 inode 的引用计数
		error = mnt_want_write(nd.path.mnt);  // 检查是否允许写操作
		if (error)
			goto exit2;
		error = security_path_unlink(&nd.path, dentry);  // 安全检查
		if (error)
			goto exit3;
		error = vfs_unlink(nd.path.dentry->d_inode, dentry);  // 执行 unlink 操作
exit3:
		mnt_drop_write(nd.path.mnt);  // 释放写权限
	exit2:
		dput(dentry);  // 释放目录项，减少dentry的引用计数
	}
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);  // 解锁父目录的互斥锁
	if (inode)	// 如果有inode，减少其引用计数
		/* truncate the inode here */
		iput(inode);  // 释放 inode，可能触发截断操作
exit1:
	path_put(&nd.path);  // 释放路径
	putname(name);  // 释放名称缓存
	return error;		// 返回错误码

slashes:	// 如果路径有异常斜杠
	error = !dentry->d_inode ? -ENOENT :
		S_ISDIR(dentry->d_inode->i_mode) ? -EISDIR : -ENOTDIR;  // 设置适当的错误码
	goto exit2;	// 跳到错误处理
}

/*
 * 定义 unlinkat 系统调用，用于从文件系统中删除文件或目录。
 * @dfd: 目录文件描述符，指明从哪个目录开始解析 pathname。
 * @pathname: 指向要删除的文件或目录的路径的用户空间指针。
 * @flag: 指定额外的标志来控制行为，例如 AT_REMOVEDIR。
 */
SYSCALL_DEFINE3(unlinkat, int, dfd, const char __user *, pathname, int, flag)
{
	// 验证 flag 参数是否只包含合法的标志，即 AT_REMOVEDIR。
	if ((flag & ~AT_REMOVEDIR) != 0)
		return -EINVAL;  // 如果包含其他标志，则返回无效参数错误。

	// 如果设置了 AT_REMOVEDIR 标志，执行目录删除操作。
	if (flag & AT_REMOVEDIR)
		return do_rmdir(dfd, pathname);

	// 如果未设置 AT_REMOVEDIR 标志，执行文件删除操作。
	return do_unlinkat(dfd, pathname);
}

/*
 * 定义 unlink 系统调用，用于从文件系统中删除文件。
 * @pathname: 指向要删除的文件的路径的用户空间指针。
 */
SYSCALL_DEFINE1(unlink, const char __user *, pathname)
{
	// 调用 do_unlinkat 函数执行删除操作，假定操作在当前工作目录进行。
	return do_unlinkat(AT_FDCWD, pathname);
}

/*
 * 创建一个指向 oldname 的符号链接，在目录 dir 中创建名为 dentry 的新条目。
 * @dir: 包含链接的目录的 inode。
 * @dentry: 符号链接的目录条目。
 * @oldname: 符号链接所指向的旧文件名。
 * 返回：成功返回0，失败返回负的错误码。
 */
int vfs_symlink(struct inode *dir, struct dentry *dentry, const char *oldname)
{
	// 检查是否允许在 dir 目录下创建新的目录项 dentry。
	int error = may_create(dir, dentry);

	if (error)
		return error; // 如果不允许，返回错误。

	// 如果目录的 inode 操作中不包含 symlink 函数，则不允许创建符号链接。
	if (!dir->i_op->symlink)
		return -EPERM; // 返回操作不允许的错误。

	// 调用安全模块的 symlink 钩子函数，进行额外的安全检查。
	error = security_inode_symlink(dir, dentry, oldname);
	if (error)
		return error; // 如果安全模块阻止操作，返回错误。

	// 调用文件系统特定的 symlink 操作，实际创建符号链接。
	error = dir->i_op->symlink(dir, dentry, oldname);
	if (!error)
		// 如果符号链接创建成功，通知文件系统事件监视器。
		fsnotify_create(dir, dentry);
	return error; // 返回操作结果，成功为0，失败为负的错误码。
}

/*
 * 创建一个符号链接。
 * @oldname: 链接指向的旧文件名。
 * @newdfd: 新文件目录的文件描述符。
 * @newname: 符号链接的新路径。
 * 返回：成功返回0，失败返回错误代码。
 */
SYSCALL_DEFINE3(symlinkat, const char __user *, oldname,
		int, newdfd, const char __user *, newname)
{
	int error; // 错误码。
	char *from; // 用户空间中的源文件路径。
	char *to; // 用户空间中的目标文件路径。
	struct dentry *dentry; // 目录条目。
	struct nameidata nd; // 路径查找时用到的结构。

	from = getname(oldname); // 从用户空间获取源文件名。
	if (IS_ERR(from)) // 检查是否有错误。
		return PTR_ERR(from);

	error = user_path_parent(newdfd, newname, &nd, &to); // 获取目标文件的父目录路径。
	if (error)
		goto out_putname; // 处理错误。

	dentry = lookup_create(&nd, 0); // 查找或创建目录项。
	error = PTR_ERR(dentry);
	if (IS_ERR(dentry))
		goto out_unlock; // 处理错误。

	error = mnt_want_write(nd.path.mnt); // 尝试获取写权限。
	if (error)
		goto out_dput; // 处理错误。
	error = security_path_symlink(&nd.path, dentry, from); // 安全检查。
	if (error)
		goto out_drop_write; // 处理错误。
	error = vfs_symlink(nd.path.dentry->d_inode, dentry, from); // 创建符号链接。
out_drop_write:
	mnt_drop_write(nd.path.mnt); // 释放写权限。
out_dput:
	dput(dentry); // 减少目录项的引用计数。
out_unlock:
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex); // 解锁。
	path_put(&nd.path); // 减少路径的引用计数。
	putname(to); // 释放获取的名称。
out_putname:
	putname(from); // 释放获取的名称。
	return error; // 返回错误码。
}

/*
 * 创建符号链接的简单版本，使用当前工作目录。
 * @oldname: 链接指向的旧文件名。
 * @newname: 符号链接的新路径。
 * 返回：成功返回0，失败返回错误代码。
 */
SYSCALL_DEFINE2(symlink, const char __user *, oldname, const char __user *, newname)
{
	return sys_symlinkat(oldname, AT_FDCWD, newname); // 调用 symlinkat 为当前工作目录创建符号链接。
}

/*
 * 创建硬链接。
 * @old_dentry: 指向要链接的旧目录项。
 * @dir: 新链接所在目录的inode。
 * @new_dentry: 指向新目录项。
 * 返回：成功返回0，失败返回错误代码。
 */
int vfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry)
{
	struct inode *inode = old_dentry->d_inode; // 获取旧目录项的inode。
	int error;

	if (!inode) // 如果inode为空，则返回“不存在该文件”错误。
		return -ENOENT;

	error = may_create(dir, new_dentry); // 检查是否有权限在目标目录创建新链接。
	if (error)
		return error;

	if (dir->i_sb != inode->i_sb) // 如果源文件和目标目录不在同一个文件系统上，则返回错误。
		return -EXDEV;

	/*
	 * A link to an append-only or immutable file cannot be created.
	 */
	/*
	 * 不能为只追加或不可变的文件创建链接。
	 */
	if (IS_APPEND(inode) || IS_IMMUTABLE(inode))
		return -EPERM;
	if (!dir->i_op->link) // 如果文件系统不支持link操作，则返回错误。
		return -EPERM;
	if (S_ISDIR(inode->i_mode)) // 如果源文件是一个目录，则返回错误，因为大多数文件系统不支持目录的硬链接。
		return -EPERM;

	error = security_inode_link(old_dentry, dir, new_dentry); // 安全模块检查链接操作。
	if (error)
		return error;

	mutex_lock(&inode->i_mutex); // 锁定源文件的inode，准备修改。
	error = dir->i_op->link(old_dentry, dir, new_dentry); // 调用具体的文件系统操作来创建链接。
	mutex_unlock(&inode->i_mutex); // 解锁。
	if (!error)
		fsnotify_link(dir, inode, new_dentry); // 如果链接创建成功，通知文件系统事件。
	return error;
}

/*
 * Hardlinks are often used in delicate situations.  We avoid
 * security-related surprises by not following symlinks on the
 * newname.  --KAB
 *
 * We don't follow them on the oldname either to be compatible
 * with linux 2.0, and to avoid hard-linking to directories
 * and other special files.  --ADM
 */
/*
 * 硬链接常用于敏感操作。为了避免安全相关的意外，我们不在新文件名上解析符号链接。--KAB
 * 我们也不在旧文件名上解析符号链接，以保持与Linux 2.0的兼容性，同时避免链接到目录和其他特殊文件。--ADM
 */
SYSCALL_DEFINE5(linkat, int, olddfd, const char __user *, oldname,
		int, newdfd, const char __user *, newname, int, flags)
{
	struct dentry *new_dentry;
	struct nameidata nd;
	struct path old_path;
	int error;
	char *to;

	if ((flags & ~AT_SYMLINK_FOLLOW) != 0)  // 检查flags，仅支持AT_SYMLINK_FOLLOW。
		return -EINVAL;

	error = user_path_at(olddfd, oldname,
			     flags & AT_SYMLINK_FOLLOW ? LOOKUP_FOLLOW : 0,
			     &old_path);  // 解析旧文件名路径。
	if (error)
		return error;

	error = user_path_parent(newdfd, newname, &nd, &to);  // 获取新文件名的父目录路径。
	if (error)
		goto out;
	error = -EXDEV;
	if (old_path.mnt != nd.path.mnt)  // 确保新旧路径在同一文件系统。
		goto out_release;
	new_dentry = lookup_create(&nd, 0);  // 查找或创建新的目录项。
	error = PTR_ERR(new_dentry);
	if (IS_ERR(new_dentry))
		goto out_unlock;
	error = mnt_want_write(nd.path.mnt);  // 尝试获取文件系统写权限。
	if (error)
		goto out_dput;
	error = security_path_link(old_path.dentry, &nd.path, new_dentry);  // 安全检查硬链接操作。
	if (error)
		goto out_drop_write;
	error = vfs_link(old_path.dentry, nd.path.dentry->d_inode, new_dentry);  // 执行硬链接操作。
out_drop_write:
	mnt_drop_write(nd.path.mnt);  // 释放写权限。
out_dput:
	dput(new_dentry);  // 释放新目录项。
out_unlock:
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);  // 解锁。
out_release:
	path_put(&nd.path);  // 释放路径。
	putname(to);  // 释放临时名称缓存。
out:
	path_put(&old_path);  // 释放旧路径。

	return error;
}

SYSCALL_DEFINE2(link, const char __user *, oldname, const char __user *, newname)
{
	return sys_linkat(AT_FDCWD, oldname, AT_FDCWD, newname, 0);  // 为link调用提供linkat功能。
}

/*
 * The worst of all namespace operations - renaming directory. "Perverted"
 * doesn't even start to describe it. Somebody in UCB had a heck of a trip...
 * Problems:
 *	a) we can get into loop creation. Check is done in is_subdir().
 *	b) race potential - two innocent renames can create a loop together.
 *	   That's where 4.4 screws up. Current fix: serialization on
 *	   sb->s_vfs_rename_mutex. We might be more accurate, but that's another
 *	   story.
 *	c) we have to lock _three_ objects - parents and victim (if it exists).
 *	   And that - after we got ->i_mutex on parents (until then we don't know
 *	   whether the target exists).  Solution: try to be smart with locking
 *	   order for inodes.  We rely on the fact that tree topology may change
 *	   only under ->s_vfs_rename_mutex _and_ that parent of the object we
 *	   move will be locked.  Thus we can rank directories by the tree
 *	   (ancestors first) and rank all non-directories after them.
 *	   That works since everybody except rename does "lock parent, lookup,
 *	   lock child" and rename is under ->s_vfs_rename_mutex.
 *	   HOWEVER, it relies on the assumption that any object with ->lookup()
 *	   has no more than 1 dentry.  If "hybrid" objects will ever appear,
 *	   we'd better make sure that there's no link(2) for them.
 *	d) some filesystems don't support opened-but-unlinked directories,
 *	   either because of layout or because they are not ready to deal with
 *	   all cases correctly. The latter will be fixed (taking this sort of
 *	   stuff into VFS), but the former is not going away. Solution: the same
 *	   trick as in rmdir().
 *	e) conversion from fhandle to dentry may come in the wrong moment - when
 *	   we are removing the target. Solution: we will have to grab ->i_mutex
 *	   in the fhandle_to_dentry code. [FIXME - current nfsfh.c relies on
 *	   ->i_mutex on parents, which works but leads to some truly excessive
 *	   locking].
 */
/*
 * 所有命名空间操作中最复杂的 - 重命名目录。"倒置的"甚至无法开始描述它。有人在UCB有一次奇异的体验...
 * 问题：
 *	a) 我们可能会造成循环。在 is_subdir() 中进行检查。
 *	b) 竞态潜能 - 两个无辜的重命名可以一起创建一个循环。这就是 4.4 版本出错的地方。当前修复方法：在
 *	   sb->s_vfs_rename_mutex 上序列化。我们可能可以做得更精确，但那是另一个故事。
 *	c) 我们需要锁定三个对象 - 父目录和目标对象（如果存在）。并且在我们得到父目录的 ->i_mutex 之前
 *	   我们不知道目标是否存在。解决方案：尝试智能地确定 inode 的锁定顺序。我们依赖于树形结构只能在 
 *	   ->s_vfs_rename_mutex 和父对象被锁定时改变的事实。因此我们可以按树（先祖先锁定）对目录进行排名，
 *	   并将所有非目录对象排在它们之后。这有效是因为除了重命名之外的所有人都执行“锁定父目录、查找、锁定子节点”，
 *	   并且重命名在 ->s_vfs_rename_mutex 下进行。
 *	   然而，这依赖于任何带有 ->lookup() 的对象至多有 1 个 dentry 的假设。如果“混合”对象出现，
 *	   我们最好确保它们没有 link(2)。
 *	d) 一些文件系统不支持打开但未链接的目录，要么因为布局原因，要么因为它们未准备好正确处理所有情况。
 *	   后者将会被修复（将此类事务纳入VFS），但前者不会消失。解决方案：与 rmdir() 中相同的技巧。
 *	e) 从 fhandle 转换为 dentry 可能在错误的时刻到来 - 当我们正在移除目标时。解决方案：我们将必须在 
 *	   fhandle_to_dentry 代码中获取 ->i_mutex。[FIXME - 当前 nfsfh.c 依赖于父目录的 ->i_mutex，
 *	   这可以工作但导致了一些确实过度的锁定]。
 */
static int vfs_rename_dir(struct inode *old_dir, struct dentry *old_dentry,
			  struct inode *new_dir, struct dentry *new_dentry)
{
	int error = 0;
	struct inode *target;

	/*
	 * If we are going to change the parent - check write permissions,
	 * we'll need to flip '..'.
	 */
	/*
	 * 如果我们要更改父目录 - 检查写权限，我们需要更改 '..'。
	 */
	if (new_dir != old_dir) {
		error = inode_permission(old_dentry->d_inode, MAY_WRITE);
		if (error)
			return error;
	}

	// 调用安全模块的重命名钩子以进行安全性检查。
	error = security_inode_rename(old_dir, old_dentry, new_dir, new_dentry);
	if (error)
		return error;

	// 获取新目录项的 inode。
	target = new_dentry->d_inode;
	if (target)
		mutex_lock(&target->i_mutex);	// 如果目标存在，锁定目标 inode。
	if (d_mountpoint(old_dentry)||d_mountpoint(new_dentry))
		error = -EBUSY; // 如果旧或新的目录项是挂载点，则返回忙碌错误。
	else {
		if (target)
			dentry_unhash(new_dentry);	// 如果目标存在，取消其目录项的哈希。
		error = old_dir->i_op->rename(old_dir, old_dentry, new_dir, new_dentry);	// 执行重命名操作。
	}
	if (target) {
		if (!error) {
			target->i_flags |= S_DEAD;	// 如果成功，标记目标 inode 为已删除。
			dont_mount(new_dentry);			// 防止在新 dentry 上挂载。
		}
		mutex_unlock(&target->i_mutex);	// 解锁目标 inode。
		if (d_unhashed(new_dentry))
			d_rehash(new_dentry);	// 如果目标 dentry 已取消哈希，重新哈希它。
		dput(new_dentry);	// 减少目标 dentry 的引用计数。
	}
	if (!error)
		if (!(old_dir->i_sb->s_type->fs_flags & FS_RENAME_DOES_D_MOVE))
			d_move(old_dentry,new_dentry);	// 如果文件系统不自动处理 d_move，则手动移动 dentry。
	return error;	// 返回操作结果。
}

/*
 * 函数 vfs_rename_other 用于重命名非目录项，比如文件或符号链接。
 * 这个函数首先执行安全检查，然后尝试执行重命名操作，并处理可能的错误情况。
 */
static int vfs_rename_other(struct inode *old_dir, struct dentry *old_dentry,
			    struct inode *new_dir, struct dentry *new_dentry)
{
	struct inode *target;
	int error;

	// 调用安全模块的重命名钩子进行安全性检查。
	error = security_inode_rename(old_dir, old_dentry, new_dir, new_dentry);
	if (error)
		return error;  // 如果安全检查未通过，则返回错误。

	dget(new_dentry);  // 增加新目录项的引用计数。
	target = new_dentry->d_inode;  // 获取目标目录项的 inode。
	if (target)
		mutex_lock(&target->i_mutex);  // 如果目标 inode 存在，则锁定它。

	// 检查是否试图在挂载点上进行操作，这是不允许的。
	if (d_mountpoint(old_dentry) || d_mountpoint(new_dentry))
		error = -EBUSY;
	else
		// 调用文件系统特定的 rename 方法尝试执行重命名。
		error = old_dir->i_op->rename(old_dir, old_dentry, new_dir, new_dentry);

	if (!error) {
		// 如果重命名操作成功且目标存在，则防止其被挂载。
		if (target)
			dont_mount(new_dentry);
		// 如果文件系统不自动处理 dentry 移动，则手动移动 dentry。
		if (!(old_dir->i_sb->s_type->fs_flags & FS_RENAME_DOES_D_MOVE))
			d_move(old_dentry, new_dentry);
	}

	if (target)
		mutex_unlock(&target->i_mutex);  // 解锁目标 inode。
	dput(new_dentry);  // 减少新目录项的引用计数。

	return error;  // 返回操作结果。
}

/*
 * 重命名文件或目录。
 * 参数:
 *  old_dir - 旧文件或目录的父目录的 inode。
 *  old_dentry - 旧文件或目录的 dentry。
 *  new_dir - 新位置的父目录的 inode。
 *  new_dentry - 新位置的 dentry。
 */
int vfs_rename(struct inode *old_dir, struct dentry *old_dentry,
	       struct inode *new_dir, struct dentry *new_dentry)
{
	int error;
	// 检测是否是目录。
	int is_dir = S_ISDIR(old_dentry->d_inode->i_mode);
	const char *old_name;

	// 如果新旧 dentry 是相同的，则无需操作。
	if (old_dentry->d_inode == new_dentry->d_inode)
 		return 0;
 
	// 检查是否有权限从旧目录删除。
	error = may_delete(old_dir, old_dentry, is_dir);
	if (error)
		return error;

	// 检查新位置是否存在节点，不存在则检查创建权限，存在则检查删除权限。
	if (!new_dentry->d_inode)
		error = may_create(new_dir, new_dentry);
	else
		error = may_delete(new_dir, new_dentry, is_dir);
	if (error)
		return error;

	// 检查文件系统是否支持重命名操作。
	if (!old_dir->i_op->rename)
		return -EPERM;

	// 初始化用于文件系统事件通知的旧名称。
	old_name = fsnotify_oldname_init(old_dentry->d_name.name);

	// 根据是否是目录选择使用的重命名函数。
	if (is_dir)
		error = vfs_rename_dir(old_dir, old_dentry, new_dir, new_dentry);
	else
		error = vfs_rename_other(old_dir, old_dentry, new_dir, new_dentry);

	// 如果重命名成功，发送文件系统事件。
	if (!error)
		fsnotify_move(old_dir, new_dir, old_name, is_dir,
			      new_dentry->d_inode, old_dentry);

	// 释放旧名称的内存。
	fsnotify_oldname_free(old_name);

	return error; // 返回操作结果。
}

/*
 * 重命名文件或目录，支持从一个目录到另一个目录。
 * 参数:
 *  olddfd - 旧文件的目录文件描述符。
 *  oldname - 旧文件的路径。
 *  newdfd - 新文件的目录文件描述符。
 *  newname - 新文件的路径。
 */
SYSCALL_DEFINE4(renameat, int, olddfd, const char __user *, oldname,
		int, newdfd, const char __user *, newname)
{
	struct dentry *old_dir, *new_dir;
	struct dentry *old_dentry, *new_dentry;
	struct dentry *trap;
	struct nameidata oldnd, newnd;
	char *from;
	char *to;
	int error;

	// 获取旧路径的父目录
	error = user_path_parent(olddfd, oldname, &oldnd, &from);
	if (error)
		goto exit;

	// 获取新路径的父目录
	error = user_path_parent(newdfd, newname, &newnd, &to);
	if (error)
		goto exit1;

	// 检查是否跨文件系统操作，确保源和目标在同一个文件系统
	// 确保旧路径和新路径位于同一个挂载点，否则返回 -EXDEV
	error = -EXDEV;
	if (oldnd.path.mnt != newnd.path.mnt)
		goto exit2;	// 如果旧路径不是普通文件/目录，报错并退出

	// 确保路径名的最后一个组件是正常的，即不是特殊目录（如"."或"..").
	old_dir = oldnd.path.dentry;
	error = -EBUSY;
	// 确保路径类型为普通路径（非特殊路径如 . 和 ..）
	if (oldnd.last_type != LAST_NORM)
		goto exit2;	// 如果新路径不是普通文件/目录，报错并退出

	new_dir = newnd.path.dentry;
	if (newnd.last_type != LAST_NORM)
		goto exit2;

	// 去除LOOKUP_PARENT标志，因为不再需要父路径
	oldnd.flags &= ~LOOKUP_PARENT;
	newnd.flags &= ~LOOKUP_PARENT;
	// 添加LOOKUP_RENAME_TARGET标志，指示这是一个重命名目标
	newnd.flags |= LOOKUP_RENAME_TARGET;

	// 锁定旧目录和新目录，以防重命名操作中的竞态条件
	trap = lock_rename(new_dir, old_dir);

	// 查找旧的目录项。
	old_dentry = lookup_hash(&oldnd);
	error = PTR_ERR(old_dentry);
	if (IS_ERR(old_dentry))
		goto exit3;	// 如果查找失败，处理错误
	/* source must exist */
	/* 源文件必须存在 */
	error = -ENOENT;
	if (!old_dentry->d_inode)
		goto exit4;	// 如果源目录项没有关联的inode，说明不存在，返回-ENOENT错误
	/* unless the source is a directory trailing slashes give -ENOTDIR */
	/* 除非源是一个目录，否则尾部的斜杠会导致 -ENOTDIR 错误 */
	if (!S_ISDIR(old_dentry->d_inode->i_mode)) {
		error = -ENOTDIR;
		if (oldnd.last.name[oldnd.last.len])
			goto exit4;	// 如果路径名在非目录文件上结束，返回-ENOTDIR错误
		if (newnd.last.name[newnd.last.len])
			goto exit4;	// 同上
	}
	/* source should not be ancestor of target */
	/* 源文件不能是目标的祖先 */
	// 源不能是目标的祖先，也就是说不能把父目录重命名为子目录。
	error = -EINVAL;
	if (old_dentry == trap)
		goto exit4;	// 如果旧目录项是锁定重命名操作中的临界点，返回-EINVAL错误
	// 查找新目录项
	new_dentry = lookup_hash(&newnd);
	error = PTR_ERR(new_dentry);
	if (IS_ERR(new_dentry))
		goto exit4;	// 查找新目录项，如果出错处理返回的错误
	/* target should not be an ancestor of source */
	/* 目标文件不能是源的祖先 */
	// 目标也不能是源的祖先。
	error = -ENOTEMPTY;
	if (new_dentry == trap)
		goto exit5;	// 如果新目录项是锁定重命名操作中的临界点，返回-ENOTEMPTY错误

	// 检查文件系统写权限
	error = mnt_want_write(oldnd.path.mnt);
	if (error)
		goto exit5;	// 请求写权限，如果失败处理错误
	// 安全检查
	error = security_path_rename(&oldnd.path, old_dentry,
				     &newnd.path, new_dentry);
	if (error)
		goto exit6;	// 检查安全模块对重命名操作的权限，如果有错误处理之
	// 调用 VFS 层的重命名操作。
	error = vfs_rename(old_dir->d_inode, old_dentry,
				   new_dir->d_inode, new_dentry);
exit6:
	mnt_drop_write(oldnd.path.mnt);	 // 释放之前获得的写权限
exit5:
	dput(new_dentry);	// 释放新目录项
exit4:
	dput(old_dentry);	// 释放旧目录项
exit3:
	unlock_rename(new_dir, old_dir);	// 解锁重命名操作
exit2:
	path_put(&newnd.path);	// 释放新路径
	putname(to);		// 释放获取的新路径名字符串
exit1:
	path_put(&oldnd.path);	// 释放旧路径
	putname(from);	// 释放获取的旧路径名字符串
exit:
	return error;	// 返回操作结果
}

// 系统调用：重命名文件或目录。这是一个简单的封装，直接调用 sys_renameat 。
SYSCALL_DEFINE2(rename, const char __user *, oldname, const char __user *, newname)
{
	return sys_renameat(AT_FDCWD, oldname, AT_FDCWD, newname); // 使用当前工作目录作为相对目录来执行重命名操作。
}

// vfs_readlink 函数用于读取符号链接的目标路径。
int vfs_readlink(struct dentry *dentry, char __user *buffer, int buflen, const char *link)
{
	int len;

	// 如果 link 是一个错误指针，获取错误并直接返回。
	len = PTR_ERR(link);
	if (IS_ERR(link))
		goto out; // 如果 link 是错误的，跳转到 out 标签。

	// 计算 link 字符串的长度，并确保不超过用户提供的缓冲区大小。
	len = strlen(link);
	if (len > (unsigned) buflen)
		len = buflen; // 如果链接长度大于缓冲区长度，截断它以适应缓冲区。

	// 将链接字符串复制到用户空间的缓冲区中。如果复制失败，返回错误 EFAULT。
	if (copy_to_user(buffer, link, len))
		len = -EFAULT; // 如果复制到用户空间失败，设置返回值为 -EFAULT。

out:
	return len; // 返回复制的字符数，或错误码。
}

/*
 * A helper for ->readlink().  This should be used *ONLY* for symlinks that
 * have ->follow_link() touching nd only in nd_set_link().  Using (or not
 * using) it for any given inode is up to filesystem.
 */
// 这是一个用于 ->readlink() 的辅助函数。这应该 *仅* 用于符号链接，这些符号链接的 ->follow_link() 只在 nd_set_link() 中触及 nd。对于任何给定的 inode 使用它或不使用它取决于文件系统。
int generic_readlink(struct dentry *dentry, char __user *buffer, int buflen)
{
	struct nameidata nd;  // nameidata 结构用于存储文件名解析过程中的状态。
	void *cookie;  // 通用指针，用于存储 follow_link 的状态。
	int res;  // 用于存储结果。

	nd.depth = 0;  // 初始化 nameidata 的深度。
	cookie = dentry->d_inode->i_op->follow_link(dentry, &nd);  // 调用 follow_link 来解析符号链接。
	if (IS_ERR(cookie))  // 检查 follow_link 的返回值是否指示错误。
		return PTR_ERR(cookie);  // 如果有错误，返回对应的错误码。

	res = vfs_readlink(dentry, buffer, buflen, nd_get_link(&nd));  // 使用解析得到的链接路径读取链接。
	if (dentry->d_inode->i_op->put_link)  // 如果存在 put_link 操作。
		dentry->d_inode->i_op->put_link(dentry, &nd, cookie);  // 调用 put_link 来清理 follow_link 可能分配的资源。
	return res;  // 返回读取结果。
}

// 简单封装调用 __vfs_follow_link 函数。
int vfs_follow_link(struct nameidata *nd, const char *link)
{
	return __vfs_follow_link(nd, link);  // 直接调用 __vfs_follow_link 来解析符号链接。
}

/* get the link contents into pagecache */
// 获取链接内容到页面缓存
static char *page_getlink(struct dentry * dentry, struct page **ppage)
{
	char *kaddr;  // 定义一个指针用于指向映射后的内核地址空间。
	struct page *page;  // 定义一个页面结构指针。
	struct address_space *mapping = dentry->d_inode->i_mapping;  // 从 dentry 的 inode 结构中获取地址空间映射。
	page = read_mapping_page(mapping, 0, NULL);  // 读取地址空间映射中的第一个页面。
	if (IS_ERR(page))  // 如果读取页面失败，检查返回值是否是错误指针。
		return (char*)page;  // 如果是错误，直接返回错误。
	*ppage = page;  // 将读取到的页面地址存储在 ppage 指向的位置。
	kaddr = kmap(page);  // 将页面映射到内核地址空间，并返回映射后的地址。
	nd_terminate_link(kaddr, dentry->d_inode->i_size, PAGE_SIZE - 1);  // 将映射后的地址空间中的数据以 null 结束符形式终止，确保字符串完整。
	return kaddr;  // 返回映射后的内核地址空间指针。
}

// 读取符号链接到用户空间
int page_readlink(struct dentry *dentry, char __user *buffer, int buflen)
{
	struct page *page = NULL;  // 初始化页面指针为 NULL，用来接收 page_getlink 函数返回的页面地址。
	char *s = page_getlink(dentry, &page);  // 调用 page_getlink 函数获取链接内容，并将页面地址存储在 page 变量中。
	int res = vfs_readlink(dentry, buffer, buflen, s);  // 使用 vfs_readlink 函数将链接内容复制到用户提供的缓冲区中。
	if (page) {  // 如果 page 不为 NULL，说明之前成功获取到了页面。
		kunmap(page);  // 取消页面映射，释放之前映射的内核地址空间。
		page_cache_release(page);  // 释放页面缓存，减少页面的引用计数。
	}
	return res;  // 返回操作结果，如果成功，返回复制到用户空间的字节数；如果失败，返回相应的错误码。
}

void *page_follow_link_light(struct dentry *dentry, struct nameidata *nd)
{
	struct page *page = NULL;  // 初始化一个页面指针为NULL，用来接收page_getlink函数返回的页面地址。
	nd_set_link(nd, page_getlink(dentry, &page));  // 调用page_getlink获取符号链接的内容，并将获取的结果设置到nd结构的链接字段。
	return page;  // 返回获取到的页面，此页面后续将用于释放。
}

void page_put_link(struct dentry *dentry, struct nameidata *nd, void *cookie)
{
	struct page *page = cookie;  // 将传入的cookie强制转换为页面指针，这里的cookie就是之前page_follow_link_light函数返回的页面。

	if (page) {  // 如果页面存在，
		kunmap(page);  // 取消页面的内核映射，释放映射所占用的内核地址空间。
		page_cache_release(page);  // 减少页面的引用计数，当引用计数为0时释放页面。
	}
}

/*
 * The nofs argument instructs pagecache_write_begin to pass AOP_FLAG_NOFS
 */
/*
 * nofs 参数指示 pagecache_write_begin 传递 AOP_FLAG_NOFS
 */
int __page_symlink(struct inode *inode, const char *symname, int len, int nofs)
{
	struct address_space *mapping = inode->i_mapping;  // 获取 inode 对应的地址映射。
	struct page *page;  // 用于存储分配的页面。
	void *fsdata;  // 文件系统的特定数据，用于 pagecache_write_begin 和 pagecache_write_end。
	int err;  // 错误码变量。
	char *kaddr;  // 用于映射页面地址。
	unsigned int flags = AOP_FLAG_UNINTERRUPTIBLE;  // 设置写操作标志为不可中断。
	if (nofs)
		flags |= AOP_FLAG_NOFS;  // 如果nofs参数为真，则添加AOP_FLAG_NOFS标志，表示在写操作中不进行文件系统的相关操作。

retry:
	err = pagecache_write_begin(NULL, mapping, 0, len-1,
				flags, &page, &fsdata);  // 开始一个页面缓存写操作。
	if (err)
		goto fail;  // 如果开始写操作失败，跳转到错误处理。

	kaddr = kmap_atomic(page, KM_USER0);  // 临时映射页面以便访问。
	memcpy(kaddr, symname, len-1);  // 将符号链接的名称复制到映射的地址。
	kunmap_atomic(kaddr, KM_USER0);  // 取消映射。

	err = pagecache_write_end(NULL, mapping, 0, len-1, len-1,
							page, fsdata);  // 结束页面缓存写操作。
	if (err < 0)
		goto fail;  // 如果写操作失败，处理错误。
	if (err < len-1)
		goto retry;  // 如果没有写完所有数据，重试写操作。

	mark_inode_dirty(inode);  // 标记inode为脏，需要将更改写回磁盘。
	return 0;  // 成功返回0。
fail:
	return err;  // 返回错误码。
}

int page_symlink(struct inode *inode, const char *symname, int len)
{
    // 调用__page_symlink函数来创建符号链接，传递inode, 符号链接名称，长度和一个标志。
    // 标志的设置取决于inode映射的GFP（内存分配）标志是否包括__GFP_FS，如果不包括，则传递true (1)。
	return __page_symlink(inode, symname, len,
			!(mapping_gfp_mask(inode->i_mapping) & __GFP_FS));
}

// 结构定义，包含与符号链接相关的inode操作函数
const struct inode_operations page_symlink_inode_operations = {
	.readlink	= generic_readlink, // 读取符号链接目标路径的通用函数
	.follow_link	= page_follow_link_light, // 轻量级的符号链接跟随函数
	.put_link	= page_put_link, // 用于清理符号链接跟随过程中的资源
};

EXPORT_SYMBOL(user_path_at);
EXPORT_SYMBOL(follow_down);
EXPORT_SYMBOL(follow_up);
EXPORT_SYMBOL(get_write_access); /* binfmt_aout */
EXPORT_SYMBOL(getname);
EXPORT_SYMBOL(lock_rename);
EXPORT_SYMBOL(lookup_one_len);
EXPORT_SYMBOL(page_follow_link_light);
EXPORT_SYMBOL(page_put_link);
EXPORT_SYMBOL(page_readlink);
EXPORT_SYMBOL(__page_symlink);
EXPORT_SYMBOL(page_symlink);
EXPORT_SYMBOL(page_symlink_inode_operations);
EXPORT_SYMBOL(path_lookup);
EXPORT_SYMBOL(kern_path);
EXPORT_SYMBOL(vfs_path_lookup);
EXPORT_SYMBOL(inode_permission);
EXPORT_SYMBOL(file_permission);
EXPORT_SYMBOL(unlock_rename);
EXPORT_SYMBOL(vfs_create);
EXPORT_SYMBOL(vfs_follow_link);
EXPORT_SYMBOL(vfs_link);
EXPORT_SYMBOL(vfs_mkdir);
EXPORT_SYMBOL(vfs_mknod);
EXPORT_SYMBOL(generic_permission);
EXPORT_SYMBOL(vfs_readlink);
EXPORT_SYMBOL(vfs_rename);
EXPORT_SYMBOL(vfs_rmdir);
EXPORT_SYMBOL(vfs_symlink);
EXPORT_SYMBOL(vfs_unlink);
EXPORT_SYMBOL(dentry_unhash);
EXPORT_SYMBOL(generic_readlink);
