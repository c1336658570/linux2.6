/*
 *  linux/fs/open.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/string.h>
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/fsnotify.h>
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/namei.h>
#include <linux/backing-dev.h>
#include <linux/capability.h>
#include <linux/securebits.h>
#include <linux/security.h>
#include <linux/mount.h>
#include <linux/vfs.h>
#include <linux/fcntl.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/fs.h>
#include <linux/personality.h>
#include <linux/pagemap.h>
#include <linux/syscalls.h>
#include <linux/rcupdate.h>
#include <linux/audit.h>
#include <linux/falloc.h>
#include <linux/fs_struct.h>
#include <linux/ima.h>

#include "internal.h"

/**
 * vfs_statfs - 获取文件系统的统计信息
 * @dentry: 目录项，表示需要获取统计信息的文件系统
 * @buf: 存储文件系统统计信息的结构体
 *
 * 根据提供的目录项dentry来获取其所在文件系统的统计信息，存储在buf中。
 * 返回0表示成功，否则返回错误代码。
 */
int vfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	int retval = -ENODEV;  // 如果没有提供有效的目录项，则预设返回设备未找到的错误

	if (dentry) {  // 如果提供了有效的目录项
		retval = -ENOSYS;  // 预设返回函数未实现的错误
		if (dentry->d_sb->s_op->statfs) {  // 如果文件系统操作中包含statfs方法
			memset(buf, 0, sizeof(*buf));  // 将统计信息结构体清零
			retval = security_sb_statfs(dentry);  // 执行安全模块相关的检查
			if (retval)
				return retval;  // 如果安全检查失败，则直接返回错误代码
			retval = dentry->d_sb->s_op->statfs(dentry, buf);  // 调用具体的statfs方法获取统计信息
			if (retval == 0 && buf->f_frsize == 0)  // 如果调用成功且f_frsize未设置
				buf->f_frsize = buf->f_bsize;  // 将f_frsize设置为块大小f_bsize
		}
	}
	return retval;  // 返回操作结果
}
EXPORT_SYMBOL(vfs_statfs);

/**
 * vfs_statfs_native - 从dentry获取文件系统统计信息，并转换为本地statfs结构
 * @dentry: 目录项，表示需要获取统计信息的文件系统
 * @buf: 本地文件系统统计结构
 *
 * 这个函数首先通过vfs_statfs函数获取文件系统的统计信息，然后将它转换为本地
 * 的statfs结构体格式。它还检查所有统计数据以确保它们可以适应较小的数据类型，
 * 特别是当使用32位整数时。
 *
 * 返回值是0代表成功，否则返回错误码。
 */
static int vfs_statfs_native(struct dentry *dentry, struct statfs *buf)
{
	struct kstatfs st;
	int retval;

	retval = vfs_statfs(dentry, &st);  // 调用 vfs_statfs 获取文件系统统计信息
	if (retval)
		return retval;  // 如果有错误，直接返回错误码

	if (sizeof(*buf) == sizeof(st))
		memcpy(buf, &st, sizeof(st));  // 如果大小匹配，直接复制整个结构
	else {
		if (sizeof buf->f_blocks == 4) {  // 如果字段使用32位整数
			// 检查统计数据中的任何字段是否有超出32位整数范围的值
			if ((st.f_blocks | st.f_bfree | st.f_bavail |
			     st.f_bsize | st.f_frsize) &
			    0xffffffff00000000ULL)
				return -EOVERFLOW;  // 如果有，返回溢出错误
			/*
			 * f_files and f_ffree may be -1; it's okay to stuff
			 * that into 32 bits
			 */
			// 对f_files和f_ffree做同样的检查
			// 检查文件数和空闲文件数是否溢出
			if (st.f_files != -1 &&
			    (st.f_files & 0xffffffff00000000ULL))
				return -EOVERFLOW;
			if (st.f_ffree != -1 &&
			    (st.f_ffree & 0xffffffff00000000ULL))
				return -EOVERFLOW;
		}

		// 逐个字段赋值到用户的结构体中
		buf->f_type = st.f_type;        // 文件系统类型
		buf->f_bsize = st.f_bsize;      // 块大小
		buf->f_blocks = st.f_blocks;    // 总块数
		buf->f_bfree = st.f_bfree;      // 空闲块数
		buf->f_bavail = st.f_bavail;    // 非超级用户可获取的空闲块数
		buf->f_files = st.f_files;      // 总文件数
		buf->f_ffree = st.f_ffree;      // 空闲文件数
		buf->f_fsid = st.f_fsid;        // 文件系统标识
		buf->f_namelen = st.f_namelen;  // 最大文件名长度
		buf->f_frsize = st.f_frsize;    // 片段大小
		memset(buf->f_spare, 0, sizeof(buf->f_spare));  // 清除保留字段
	}
	return 0;
}

/**
 * vfs_statfs64 - 从dentry获取文件系统的64位统计信息
 * @dentry: 目录项，表示需要获取统计信息的文件系统
 * @buf: 用户提供的64位文件系统统计结构
 *
 * 这个函数首先通过调用 vfs_statfs 来获取文件系统的统计信息，然后将这些信息转换
 * 成64位的 statfs64 结构体格式。如果结构体大小相匹配，则直接复制，否则逐个字段
 * 赋值。
 *
 * 返回值: 成功时返回0，失败时返回错误码。
 */
static int vfs_statfs64(struct dentry *dentry, struct statfs64 *buf)
{
	struct kstatfs st;
	int retval;

	retval = vfs_statfs(dentry, &st);  // 调用 vfs_statfs 获取文件系统统计信息
	if (retval)
		return retval;  // 如果获取信息失败，直接返回错误码

	if (sizeof(*buf) == sizeof(st))
		memcpy(buf, &st, sizeof(st));  // 如果结构体大小匹配，直接复制
	else {
		// 将 kstatfs 结构体的字段转换到 statfs64 结构体中
		buf->f_type = st.f_type;        // 文件系统类型
		buf->f_bsize = st.f_bsize;      // 块大小
		buf->f_blocks = st.f_blocks;    // 总块数
		buf->f_bfree = st.f_bfree;      // 空闲块数
		buf->f_bavail = st.f_bavail;    // 非超级用户可获取的空闲块数
		buf->f_files = st.f_files;      // 总文件数
		buf->f_ffree = st.f_ffree;      // 空闲文件数
		buf->f_fsid = st.f_fsid;        // 文件系统标识
		buf->f_namelen = st.f_namelen;  // 最大文件名长度
		buf->f_frsize = st.f_frsize;    // 片段大小
		memset(buf->f_spare, 0, sizeof(buf->f_spare));  // 清除保留字段
	}
	return 0;  // 操作成功
}

// 用于通过给定的路径名获取文件系统的状态信息
SYSCALL_DEFINE2(statfs, const char __user *, pathname, struct statfs __user *, buf)
{
	struct path path;  // 用来存储路径解析后的结果
	int error;         // 用于存放错误码

	error = user_path(pathname, &path);  // 解析用户空间提供的路径名，并存储结果到 path 结构体中
	if (!error) {  // 如果路径解析成功，error 为 0
		struct statfs tmp;  // 定义一个 statfs 结构体用于临时存放文件系统状态信息
		error = vfs_statfs_native(path.dentry, &tmp);  // 获取文件系统状态信息并存储到 tmp 中
		if (!error && copy_to_user(buf, &tmp, sizeof(tmp)))  // 如果获取信息成功且将信息从内核空间复制到用户空间出错
			error = -EFAULT;  // 设置错误码为 -EFAULT，表示内存访问出错
		path_put(&path);  // 释放通过 user_path 获取的路径资源
	}
	return error;  // 返回错误码
}

// 用于获取文件系统状态信息，支持 64 位的大小字段
SYSCALL_DEFINE3(statfs64, const char __user *, pathname, size_t, sz, struct statfs64 __user *, buf)
{
	struct path path;  // 用来存储解析后的路径信息
	long error;        // 用于存放错误码

	if (sz != sizeof(*buf))  // 检查传入的结构体大小是否正确
		return -EINVAL;  // 如果大小不匹配，返回无效参数错误
	error = user_path(pathname, &path);  // 解析用户空间提供的路径名，并存储结果到 path 结构体中
	if (!error) {  // 如果路径解析成功，error 为 0
		struct statfs64 tmp;  // 定义一个 statfs64 结构体用于临时存放文件系统状态信息
		error = vfs_statfs64(path.dentry, &tmp);  // 获取文件系统状态信息并存储到 tmp 中
		if (!error && copy_to_user(buf, &tmp, sizeof(tmp)))  // 如果获取信息成功且将信息从内核空间复制到用户空间出错
			error = -EFAULT;  // 设置错误码为 -EFAULT，表示内存访问出错
		path_put(&path);  // 释放通过 user_path 获取的路径资源
	}
	return error;  // 返回错误码
}

// 用于获取与打开的文件描述符关联的文件系统的状态信息
SYSCALL_DEFINE2(fstatfs, unsigned int, fd, struct statfs __user *, buf)
{
	struct file * file;  // 定义一个 file 结构体指针用来接收打开的文件对象
	struct statfs tmp;   // 定义一个 statfs 结构体用于存放文件系统的状态信息
	int error;           // 用于存放错误码

	error = -EBADF;      // 初始化错误码为 -EBADF，表示无效的文件描述符
	file = fget(fd);     // 通过文件描述符获取 file 结构体指针
	if (!file)           // 如果未获取到 file 结构体，说明文件描述符无效
		goto out;        // 跳转到函数出口，返回错误

	error = vfs_statfs_native(file->f_path.dentry, &tmp);  // 调用 vfs_statfs_native 获取文件系统状态信息
	if (!error && copy_to_user(buf, &tmp, sizeof(tmp)))    // 如果成功获取状态信息且将信息从内核空间复制到用户空间时发生错误
		error = -EFAULT;   // 设置错误码为 -EFAULT，表示在内存访问过程中发生错误

	fput(file);  // 减少文件对象的引用计数，如果引用计数为0，则释放该文件对象
out:
	return error;  // 返回错误码
}

// 用于通过文件描述符获取文件系统的状态信息，支持 64 位大小字段
SYSCALL_DEFINE3(fstatfs64, unsigned int, fd, size_t, sz, struct statfs64 __user *, buf)
{
	struct file * file;  // 用于存储由文件描述符指定的文件的结构体
	struct statfs64 tmp;  // 用于暂存文件系统的状态信息
	int error;  // 存储错误代码

	if (sz != sizeof(*buf))  // 检查用户传入的结构体大小是否正确
		return -EINVAL;  // 如果大小不正确，返回错误代码 -EINVAL，表示参数无效

	error = -EBADF;  // 假设错误代码为 -EBADF，表示无效的文件描述符
	file = fget(fd);  // 根据文件描述符获取文件对象
	if (!file)  // 如果文件对象不存在
		goto out;  // 跳转到 out 标签，返回错误
	error = vfs_statfs64(file->f_path.dentry, &tmp);  // 获取该文件的文件系统状态信息
	if (!error && copy_to_user(buf, &tmp, sizeof(tmp)))  // 如果获取成功且将数据复制到用户空间失败
		error = -EFAULT;  // 设置错误码为 -EFAULT，表示内存访问错误
	fput(file);  // 释放通过 fget 获取的文件对象
out:
	return error;  // 返回错误码
}

// 用于截断文件的函数 do_truncate，该函数更新文件大小和相关属性
int do_truncate(struct dentry *dentry, loff_t length, unsigned int time_attrs,
	struct file *filp)
{
	int ret;  // 用于存储函数返回值或操作结果
	struct iattr newattrs;  // 文件属性结构体

	/* Not pretty: "inode->i_size" shouldn't really be signed. But it is. */
	/* 不太美观：理论上 "inode->i_size" 不应该是有符号类型，但它是有符号的。 */
	if (length < 0)  // 如果新的文件大小小于0
		return -EINVAL;  // 返回错误代码 -EINVAL，表示无效的参数

	newattrs.ia_size = length;  // 设置新的文件大小
	newattrs.ia_valid = ATTR_SIZE | time_attrs;  // 设置文件属性有效标志
	if (filp) {  // 如果提供了文件对象
		newattrs.ia_file = filp;  // 关联此文件属性结构体与特定的文件对象
		newattrs.ia_valid |= ATTR_FILE;  // 添加文件属性标志
	}

	/* Remove suid/sgid on truncate too */
	/* 在截断时也移除suid/sgid */
	ret = should_remove_suid(dentry);  // 检查是否需要移除suid/sgid权限
	if (ret)
		newattrs.ia_valid |= ret | ATTR_FORCE;  // 如果需要，设置相应的属性标志

	mutex_lock(&dentry->d_inode->i_mutex);  // 锁定inode，以防止并发修改
	ret = notify_change(dentry, &newattrs);  // 通知系统文件属性已更改
	mutex_unlock(&dentry->d_inode->i_mutex);  // 解锁inode
	return ret;  // 返回操作结果
}

// 用于截断指定路径的文件到特定的长度
static long do_sys_truncate(const char __user *pathname, loff_t length)
{
	struct path path;
	struct inode *inode;
	int error;

	error = -EINVAL;  // 初始化错误代码为无效参数
	/* sorry, but loff_t says... */
	if (length < 0)  // 检查长度是否小于0
		goto out;  // 如果是，直接返回错误

	error = user_path(pathname, &path);  // 获取用户空间的路径名对应的内核路径结构
	if (error)  // 如果获取路径失败
		goto out;  // 返回错误
	inode = path.dentry->d_inode;  // 获取路径对应的inode

	/* For directories it's -EISDIR, for other non-regulars - -EINVAL */
	/* 对于目录文件，返回错误 -EISDIR，对于非常规文件，返回 -EINVAL */
	error = -EISDIR;  // 默认为是目录的错误代码
	if (S_ISDIR(inode->i_mode))  // 检查是否为目录
		goto dput_and_out;  // 是目录，返回错误

	error = -EINVAL;  // 重置错误代码为无效参数
	if (!S_ISREG(inode->i_mode))  // 检查是否为常规文件
		goto dput_and_out;  // 不是常规文件，返回错误

	error = mnt_want_write(path.mnt);  // 检查挂载点是否允许写操作
	if (error)  // 如果不允许
		goto dput_and_out;  // 返回错误

	error = inode_permission(inode, MAY_WRITE);  // 检查inode写权限
	if (error)  // 如果没有写权限
		goto mnt_drop_write_and_out;  // 返回错误

	error = -EPERM;  // 默认为权限不足的错误代码
	if (IS_APPEND(inode))  // 如果inode设置了追加属性
		goto mnt_drop_write_and_out;  // 返回错误

	error = get_write_access(inode);  // 获取inode的写访问权
	if (error)  // 如果获取失败
		goto mnt_drop_write_and_out;  // 返回错误

	/*
	 * Make sure that there are no leases.  get_write_access() protects
	 * against the truncate racing with a lease-granting setlease().
	 */
	/* 确保没有租约冲突，get_write_access() 会阻止与授权租约竞争的截断操作 */
	error = break_lease(inode, O_WRONLY);  // 尝试破坏对该inode的任何租约
	if (error)  // 如果破坏租约失败
		goto put_write_and_out;  // 返回错误

	error = locks_verify_truncate(inode, NULL, length);  // 验证是否可以截断
	if (!error)  // 如果可以截断
		error = security_path_truncate(&path, length, 0);  // 执行安全检查
	if (!error)  // 如果安全检查通过
		error = do_truncate(path.dentry, length, 0, NULL);  // 执行截断

put_write_and_out:
	put_write_access(inode);  // 释放写访问权
mnt_drop_write_and_out:
	mnt_drop_write(path.mnt);  // 释放挂载点的写锁
dput_and_out:
	path_put(&path);  // 释放获取的路径
out:
	return error;  // 返回结果
}

// 设置指定文件的大小。这个系统调用通过 SYSCALL_DEFINE2 宏定义，接受两个参数，并调用内部函数 do_sys_truncate 来执行实际的操作。
SYSCALL_DEFINE2(truncate, const char __user *, path, long, length)
{
	// 调用do_sys_truncate函数，传入用户空间提供的文件路径和期望的文件长度
	return do_sys_truncate(path, length);
}

// 系统调用 ftruncate 的辅助函数 do_sys_ftruncate
static long do_sys_ftruncate(unsigned int fd, loff_t length, int small)
{
	struct inode * inode;
	struct dentry *dentry;
	struct file * file;
	int error;

	error = -EINVAL;  // 初始化错误码为无效参数
	if (length < 0)  // 长度不能为负，否则返回错误
		goto out;
	error = -EBADF;  // 如果文件描述符无效，设置错误码为坏文件描述符
	file = fget(fd);  // 根据文件描述符获取文件结构体
	if (!file)       // 如果文件不存在，则退出
		goto out;

	/* explicitly opened as large or we are on 64-bit box */
	if (file->f_flags & O_LARGEFILE)  // 如果文件是大文件或者在64位系统上，则不处理small
		small = 0;

	dentry = file->f_path.dentry;
	inode = dentry->d_inode;  // 获取inode结构体
	error = -EINVAL;  // 如果文件不是普通文件或不可写，则返回错误
	if (!S_ISREG(inode->i_mode) || !(file->f_mode & FMODE_WRITE))
		goto out_putf;

	error = -EINVAL;  // 如果不支持大文件且长度超过最大非LFS长度，则返回错误
	/* Cannot ftruncate over 2^31 bytes without large file support */
	if (small && length > MAX_NON_LFS)
		goto out_putf;

	error = -EPERM;  // 如果文件设置了追加标志，则返回错误
	if (IS_APPEND(inode))
		goto out_putf;

	error = locks_verify_truncate(inode, file, length);  // 验证是否可以截断
	if (!error)
		error = security_path_truncate(&file->f_path, length, ATTR_MTIME|ATTR_CTIME);
	if (!error)
		error = do_truncate(dentry, length, ATTR_MTIME|ATTR_CTIME, file);
out_putf:
	fput(file);  // 释放文件结构体
out:
	return error;  // 返回错误码
}

// ftruncate 系统调用的实现，它截断由文件描述符指定的文件到指定的长度
SYSCALL_DEFINE2(ftruncate, unsigned int, fd, unsigned long, length)
{
	long ret = do_sys_ftruncate(fd, length, 1);	// 调用辅助函数，假定小文件模式
	/* avoid REGPARM breakage on x86: */
	asmlinkage_protect(2, ret, fd, length);		// 保护寄存器参数，防止在x86上的优化破坏
	return ret;	// 返回结果
}

/* LFS versions of truncate are only needed on 32 bit machines */
/* LFS（大文件支持）版本的truncate只在32位机器上需要 */
// 如果系统是32位的（BITS_PER_LONG等于32）
#if BITS_PER_LONG == 32

// 定义truncate64系统调用，处理64位的文件长度截断
SYSCALL_DEFINE(truncate64)(const char __user * path, loff_t length)
{
	// 调用do_sys_truncate执行实际的截断操作
	return do_sys_truncate(path, length);
}

#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
// 定义一个64位的truncate系统调用包装器（仅在有SYSCALL_WRAPPERS配置时使用）
asmlinkage long SyS_truncate64(long path, loff_t length)
{
	// 调用truncate64系统调用，将path参数转换为指向用户空间路径的指针
	return SYSC_truncate64((const char __user *) path, length);
}
// 将sys_truncate64与包装器函数SyS_truncate64关联
SYSCALL_ALIAS(sys_truncate64, SyS_truncate64);
#endif

// 定义ftruncate64系统调用，用于处理带有64位文件长度的文件截断
SYSCALL_DEFINE(ftruncate64)(unsigned int fd, loff_t length)
{
	// 调用do_sys_ftruncate执行实际的截断操作，指定large file support（大文件支持）
	long ret = do_sys_ftruncate(fd, length, 0);
	/* avoid REGPARM breakage on x86: */
	// 保护x86系统上的寄存器参数，防止因编译器优化导致的问题
	asmlinkage_protect(2, ret, fd, length);
	return ret;  // 返回截断操作的结果
}

#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
// 定义一个64位的ftruncate系统调用包装器（仅在有SYSCALL_WRAPPERS配置时使用）
asmlinkage long SyS_ftruncate64(long fd, loff_t length)
{
	// 调用ftruncate64系统调用，将fd转换为无符号整数并传递给实际的系统调用函数
	return SYSC_ftruncate64((unsigned int) fd, length);
}
// 将sys_ftruncate64与包装器函数SyS_ftruncate64关联
SYSCALL_ALIAS(sys_ftruncate64, SyS_ftruncate64);
#endif

#endif /* BITS_PER_LONG == 32 */  // 结束32位系统的条件编译块

int do_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file->f_path.dentry->d_inode; // 获取文件对应的inode节点
	long ret;

	if (offset < 0 || len <= 0) // 检查偏移量是否为负数或者长度是否小于等于0
		return -EINVAL; // 如果是，返回无效参数错误

	/* Return error if mode is not supported */
	/* 如果mode不支持并且不包含FALLOC_FL_KEEP_SIZE标志，则返回错误 */
	if (mode && !(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP; // 操作不支持

	if (!(file->f_mode & FMODE_WRITE)) // 检查文件是否以写模式打开
		return -EBADF; // 如果不是，返回错误的文件描述符错误

	/*
	 * Revalidate the write permissions, in case security policy has
	 * changed since the files were opened.
	 */
	/*
	 * 重新验证写权限，以防止在文件打开后安全策略发生变化。
	 */
	ret = security_file_permission(file, MAY_WRITE); // 验证文件的写权限
	if (ret)
		return ret; // 如果权限验证失败，返回错误代码

	if (S_ISFIFO(inode->i_mode)) // 检查文件是否是FIFO（命名管道）
		return -ESPIPE; // 如果是，返回非法寻址错误

	/*
	 * Let individual file system decide if it supports preallocation
	 * for directories or not.
	 */
	/*
	 * 让各个文件系统自行决定是否支持目录的预分配。
	 */
	if (!S_ISREG(inode->i_mode) && !S_ISDIR(inode->i_mode)) // 检查文件是否是常规文件或目录
		return -ENODEV; // 如果不是，返回无设备错误

	/* Check for wrap through zero too */
	/* 检查文件是否超出了文件系统的最大字节数或出现负值 */
	if (((offset + len) > inode->i_sb->s_maxbytes) || ((offset + len) < 0))
		return -EFBIG; // 如果是，返回文件太大错误

	if (!inode->i_op->fallocate) // 检查文件系统是否支持fallocate操作
		return -EOPNOTSUPP; // 如果不支持，返回操作不支持错误

	return inode->i_op->fallocate(inode, mode, offset, len); // 调用文件系统的fallocate函数执行实际的操作
}

SYSCALL_DEFINE(fallocate)(int fd, int mode, loff_t offset, loff_t len)
{
	struct file *file;  // 定义文件指针 `file`
	int error = -EBADF; // 初始化 `error` 为 `-EBADF`，表示“坏的文件描述符”错误

	file = fget(fd); // 获取文件描述符 `fd` 对应的 `file` 结构
	if (file) { // 如果成功获取文件
		error = do_fallocate(file, mode, offset, len); // 调用 `do_fallocate` 执行文件空间分配
		fput(file); // 释放文件引用计数
	}

	return error; // 返回操作的结果
}

#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
// 如果内核配置了系统调用包装器
asmlinkage long SyS_fallocate(long fd, long mode, loff_t offset, loff_t len)
{
	// 将传入的参数转换为合适的类型并调用 `SYSC_fallocate`
	return SYSC_fallocate((int)fd, (int)mode, offset, len);
}
SYSCALL_ALIAS(sys_fallocate, SyS_fallocate); // 为 `sys_fallocate` 创建别名 `SyS_fallocate`
#endif

/*
 * access() needs to use the real uid/gid, not the effective uid/gid.
 * We do this by temporarily clearing all FS-related capabilities and
 * switching the fsuid/fsgid around to the real ones.
 */
/*
 * access() 需要使用真实的 uid/gid，而不是有效的 uid/gid。
 * 我们通过暂时清除所有与文件系统相关的权限，并将 fsuid/fsgid 切换为真实的 uid/gid 来实现这一点。
 */
/*
 * faccessat 系统调用定义
 * @dfd: 文件描述符
 * @filename: 用户空间中的文件名
 * @mode: 访问模式
 *
 * 实现访问权限检查的系统调用。它用于检查调用者是否有权访问指定文件。
 */
SYSCALL_DEFINE3(faccessat, int, dfd, const char __user *, filename, int, mode)
{
	const struct cred *old_cred;  // 保存旧的凭据结构指针
	struct cred *override_cred;   // 新的凭据结构，用于覆盖旧的
	struct path path;             // 存储文件路径信息
	struct inode *inode;          // 指向文件 inode 的指针
	int res;                      // 用于存储函数的返回值

	// 检查提供的模式是否包含了除了 S_IRWXO 以外的其他位（S_IRWXO 包括读/写/执行权限）
	/* 是否包含 F_OK, X_OK, W_OK, R_OK 之外的值 */
	if (mode & ~S_IRWXO)	/* where's F_OK, X_OK, W_OK, R_OK? */
		return -EINVAL;	// 如果包含非法位，则返回错误

	// 准备新的凭证
	override_cred = prepare_creds();
	if (!override_cred)
		return -ENOMEM;	// 如果凭证准备失败，则返回内存不足错误

	// 设置新凭证的 fsuid 和 fsgid 为当前凭证的 uid 和 gid
	override_cred->fsuid = override_cred->uid;
	override_cred->fsgid = override_cred->gid;

	// 如果系统安全策略没有禁止 setuid/setgid 修复，则根据 uid 清除或设置有效的能力
	if (!issecure(SECURE_NO_SETUID_FIXUP)) {
		/* Clear the capabilities if we switch to a non-root user */
		// 如果 uid 不是 root，则清除所有有效的能力
		if (override_cred->uid)
			cap_clear(override_cred->cap_effective);
		else	// 否则，将允许的能力设置为有效能力
			override_cred->cap_effective =
				override_cred->cap_permitted;
	}

	// 应用新的凭证
	old_cred = override_creds(override_cred);

	// 获取文件路径，LOOKUP_FOLLOW 表示如果最后一个组件是符号链接，则应该追踪
	res = user_path_at(dfd, filename, LOOKUP_FOLLOW, &path);
	if (res)
		goto out;	// 如果路径获取失败，跳转到出口处理

	// 从路径中获取inode结构
	inode = path.dentry->d_inode;

	// 如果请求执行权限，并且文件是常规文件
	if ((mode & MAY_EXEC) && S_ISREG(inode->i_mode)) {	// 如果请求执行权限且文件类型为常规文件
		/*
		 * MAY_EXEC on regular files is denied if the fs is mounted
		 * with the "noexec" flag.
		 */
		/*
		 * 如果文件系统被挂载为noexec，则拒绝对常规文件的执行权限请求。
		 */
		res = -EACCES;	// 默认设置访问拒绝错误
		if (path.mnt->mnt_flags & MNT_NOEXEC)	// 检查挂载标志是否包含noexec
			goto out_path_release;	// 如果有，跳转到释放路径的标签
	}

	res = inode_permission(inode, mode | MAY_ACCESS);	// 检查inode访问权限
	/* SuS v2 requires we report a read only fs too */
	/* Single Unix Specification v2要求我们也报告只读文件系统的情况 */
	// 如果文件系统只读，也需要报告错误
	if (res || !(mode & S_IWOTH) || special_file(inode->i_mode))
		goto out_path_release;	// 如果检查失败或不允许写或是特殊文件，跳转到释放路径的标签
	/*
	 * This is a rare case where using __mnt_is_readonly()
	 * is OK without a mnt_want/drop_write() pair.  Since
	 * no actual write to the fs is performed here, we do
	 * not need to telegraph to that to anyone.
	 *
	 * By doing this, we accept that this access is
	 * inherently racy and know that the fs may change
	 * state before we even see this result.
	 */
	/*
	 * 这是一个罕见的情况，使用 __mnt_is_readonly() 函数检查文件系统是否只读
	 * 不需要 mnt_want/drop_write() 对，因为这里没有进行实际的写操作。
	 * 通过这样做，我们接受这种访问是有竞争条件的，并且知道文件系统可能在我们
	 * 看到这个结果之前就已经改变状态了。
	 */
	// 检查文件系统是否只读（无需写锁定，因为不会实际写入）
	if (__mnt_is_readonly(path.mnt))
		res = -EROFS;	// 如果文件系统是只读的，设置返回错误为只读文件系统

out_path_release:
	path_put(&path);	// 释放获取的路径
out:
	revert_creds(old_cred);	// 恢复原始凭证
	put_cred(override_cred);	// 释放修改后的凭证
	return res;	// 返回结果
}

/*
 * 访问系统调用的实现，检查对文件的访问权限。
 */
SYSCALL_DEFINE2(access, const char __user *, filename, int, mode)
{
	return sys_faccessat(AT_FDCWD, filename, mode);  // 调用faccessat实现access功能
}

/*
 * 改变当前工作目录到指定的路径。
 */
SYSCALL_DEFINE1(chdir, const char __user *, filename)
{
	struct path path;  // 用于存储解析的路径信息
	int error;  // 错误码

	error = user_path_dir(filename, &path);  // 获取给定路径的目录信息
	if (error)  // 如果有错误发生
		goto out;  // 直接跳转到处理结束

	error = inode_permission(path.dentry->d_inode, MAY_EXEC | MAY_ACCESS);  // 检查执行权限
	if (error)  // 如果检查权限时出错
		goto dput_and_out;  // 跳转到释放路径并处理结束

	set_fs_pwd(current->fs, &path);  // 设置新的工作目录

dput_and_out:
	path_put(&path);  // 释放获取的路径信息
out:
	return error;  // 返回操作结果
}

/*
 * 更改当前工作目录至由文件描述符指定的目录。
 */
SYSCALL_DEFINE1(fchdir, unsigned int, fd)
{
	struct file *file;  // 文件指针，用来访问文件描述符指向的文件
	struct inode *inode;  // inode结构指针，用来访问文件的元数据
	int error;  // 错误码

	error = -EBADF;  // 假设文件描述符无效
	file = fget(fd);  // 尝试获取文件描述符指向的文件
	if (!file)  // 如果获取失败，说明文件描述符无效
		goto out;  // 跳转至函数出口

	inode = file->f_path.dentry->d_inode;  // 获取文件的inode

	error = -ENOTDIR;  // 假设文件不是目录
	if (!S_ISDIR(inode->i_mode))  // 检查inode对应的文件类型是否为目录
		goto out_putf;  // 如果不是目录，释放文件并跳转至出口

	error = inode_permission(inode, MAY_EXEC | MAY_ACCESS);  // 检查对目录的执行权限
	if (!error)  // 如果有执行权限
		set_fs_pwd(current->fs, &file->f_path);  // 设置进程的工作目录为该目录

out_putf:
	fput(file);  // 释放文件引用
out:
	return error;  // 返回操作结果
}

/*
 * 通过给定的路径名实现chroot系统调用，改变进程的根目录到指定路径。
 */
SYSCALL_DEFINE1(chroot, const char __user *, filename)
{
	struct path path;  // 用于存储解析后的文件路径信息
	int error;  // 错误码

	// 尝试解析用户提供的目录名为path结构，确认目录存在
	error = user_path_dir(filename, &path);
	if (error)
		goto out;  // 如果路径无效或解析失败，跳到函数末尾处理错误

	// 检查当前进程是否有执行该路径的权限
	error = inode_permission(path.dentry->d_inode, MAY_EXEC | MAY_ACCESS);
	if (error)
		goto dput_and_out;  // 如果无权限，释放路径并返回错误

	// 检查当前进程是否具有进行chroot操作的能力（CAP_SYS_CHROOT）
	error = -EPERM;
	if (!capable(CAP_SYS_CHROOT))
		goto dput_and_out;  // 如果无chroot权限，释放路径并返回错误

	// 安全模块检查chroot操作
	error = security_path_chroot(&path);
	if (error)
		goto dput_and_out;  // 如果安全模块拒绝，释放路径并返回错误

	// 设置进程的根目录为指定路径
	set_fs_root(current->fs, &path);
	error = 0;  // 操作成功，设置返回值为0

dput_and_out:
	path_put(&path);  // 释放获取的路径资源
out:
	return error;  // 返回操作结果
}

/*
 * 系统调用 fchmod 用于修改文件的权限。
 * @fd: 文件描述符，指向需要更改权限的文件。
 * @mode: 新的权限模式。
 */
SYSCALL_DEFINE2(fchmod, unsigned int, fd, mode_t, mode)
{
	struct inode * inode;  // 指向文件的inode结构体
	struct dentry * dentry;  // 指向文件的dentry结构体
	struct file * file;  // 文件结构体指针
	int err = -EBADF;  // 默认返回值为错误的文件描述符
	struct iattr newattrs;  // 用于修改inode属性的结构体

	file = fget(fd);  // 根据文件描述符获取文件结构体
	if (!file)
		goto out;  // 如果文件不存在，则跳转到出错处理

	dentry = file->f_path.dentry;  // 获取与文件关联的dentry
	inode = dentry->d_inode;  // 获取与dentry关联的inode

	audit_inode(NULL, dentry);  // 审计文件

	err = mnt_want_write_file(file);  // 尝试获取文件系统的写权限
	if (err)
		goto out_putf;  // 获取写权限失败，处理错误

	mutex_lock(&inode->i_mutex);  // 对inode加锁，保证线程安全
	err = security_path_chmod(dentry, file->f_vfsmnt, mode);  // 安全检查
	if (err)
		goto out_unlock;  // 安全检查未通过，处理错误

	if (mode == (mode_t) -1)  // 如果模式为-1，使用原始模式
		mode = inode->i_mode;
	newattrs.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);  // 设置新的权限模式，保留非权限位
	newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;  // 设置修改的属性类型
	err = notify_change(dentry, &newattrs);  // 应用更改

out_unlock:
	mutex_unlock(&inode->i_mutex);  // 解锁inode
	mnt_drop_write(file->f_path.mnt);  // 释放文件系统的写权限
out_putf:
	fput(file);  // 释放文件结构体
out:
	return err;  // 返回操作结果
}

/*
 * 系统调用 fchmodat 用于修改指定路径文件的权限。
 * @dfd: 文件描述符或特殊值 AT_FDCWD。
 * @filename: 文件路径。
 * @mode: 新的权限模式。
 */
SYSCALL_DEFINE3(fchmodat, int, dfd, const char __user *, filename, mode_t, mode)
{
	struct path path;  // 路径结构体
	struct inode *inode;  // inode 结构体
	int error;  // 错误码
	struct iattr newattrs;  // inode 属性结构体

	// 通过文件描述符和文件名解析得到文件的路径
	error = user_path_at(dfd, filename, LOOKUP_FOLLOW, &path);
	if (error)
		goto out;  // 如果路径解析失败，跳到结束处理

	inode = path.dentry->d_inode;  // 获取路径对应的 inode

	// 请求文件系统写权限
	error = mnt_want_write(path.mnt);
	if (error)
		goto dput_and_out;  // 如果获取写权限失败，处理并退出

	mutex_lock(&inode->i_mutex);  // 锁定 inode 以进行操作
	// 执行权限修改的安全检查
	error = security_path_chmod(path.dentry, path.mnt, mode);
	if (error)
		goto out_unlock;  // 如果安全检查失败，解锁并退出

	// 如果 mode 为 -1，则使用当前 inode 的模式
	if (mode == (mode_t) -1)
		mode = inode->i_mode;

	// 设置新的权限模式，保留 inode 中非权限部分的模式位
	newattrs.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);
	newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;  // 设置修改的属性有效位

	// 应用权限更改
	error = notify_change(path.dentry, &newattrs);

out_unlock:
	mutex_unlock(&inode->i_mutex);  // 解锁 inode
	mnt_drop_write(path.mnt);  // 释放文件系统写权限

dput_and_out:
	path_put(&path);  // 释放路径

out:
	return error;  // 返回操作结果
}

/*
 * chmod 系统调用的包装，使用当前工作目录作为基路径。
 * @filename: 文件路径。
 * @mode: 新的权限模式。
 */
SYSCALL_DEFINE2(chmod, const char __user *, filename, mode_t, mode)
{
	// 直接调用 fchmodat 函数，使用 AT_FDCWD 表示相对于当前工作目录
	return sys_fchmodat(AT_FDCWD, filename, mode);
}

/*
 * chown_common - 公共函数用于更改文件的所有者和组
 * @path: 文件的路径结构
 * @user: 新的用户ID
 * @group: 新的组ID
 *
 * 如果user或group为-1，则表示不改变该值。
 * 返回0表示成功，否则为负的错误代码。
 */
static int chown_common(struct path *path, uid_t user, gid_t group)
{
	struct inode *inode = path->dentry->d_inode;  // 获取文件的 inode 结构
	int error;  // 用于存储错误码
	struct iattr newattrs;  // inode 属性结构体

	newattrs.ia_valid = ATTR_CTIME;  // 设置修改时间属性有效

	// 如果指定了有效的 user ID，则设置用户 ID
	if (user != (uid_t) -1) {
		newattrs.ia_valid |= ATTR_UID;  // 添加用户ID修改标志
		newattrs.ia_uid = user;  // 设置新的用户 ID
	}
	// 如果指定了有效的 group ID，则设置组 ID
	if (group != (gid_t) -1) {
		newattrs.ia_valid |= ATTR_GID;  // 添加组ID修改标志
		newattrs.ia_gid = group;  // 设置新的组 ID
	}
	// 如果文件不是目录，添加去除特权标志
	if (!S_ISDIR(inode->i_mode)) {
		newattrs.ia_valid |=
			ATTR_KILL_SUID | ATTR_KILL_SGID | ATTR_KILL_PRIV;
	}

	mutex_lock(&inode->i_mutex);  // 锁定 inode 以保护修改操作
	// 调用安全模块的权限修改检查
	error = security_path_chown(path, user, group);
	if (!error)
		error = notify_change(path->dentry, &newattrs);  // 通知系统属性更改
	mutex_unlock(&inode->i_mutex);  // 解锁 inode

	return error;  // 返回操作结果
}

/*
 * sys_chown - 改变文件的所有者和组
 * @filename: 用户空间提供的文件名
 * @user: 新的用户ID
 * @group: 新的组ID
 *
 * 返回0表示成功，否则返回错误码。
 */
SYSCALL_DEFINE3(chown, const char __user *, filename, uid_t, user, gid_t, group)
{
	struct path path;  // 文件路径结构
	int error;  // 错误码变量

	// 根据给定的用户空间文件名获取文件路径
	error = user_path(filename, &path);
	if (error)
		goto out;  // 如果出错，直接跳转到结束

	// 获取写权限，以便修改文件系统上的文件
	error = mnt_want_write(path.mnt);
	if (error)
		goto out_release;  // 如果获取写权限失败，跳转到释放路径

	// 使用共通函数chown_common来处理文件所有者和组的变更
	error = chown_common(&path, user, group);

	// 完成修改后释放写权限
	mnt_drop_write(path.mnt);
out_release:
	// 释放获取的路径
	path_put(&path);
out:
	// 返回操作结果
	return error;
}

/*
 * sys_fchownat - 在指定的目录文件描述符下改变文件的所有者和组
 * @dfd: 目录的文件描述符，用于相对路径查找
 * @filename: 用户空间提供的文件名
 * @user: 新的用户ID
 * @group: 新的组ID
 * @flag: 标志位，控制行为（例如是否遵循符号链接）
 *
 * 如果成功，返回0；否则返回错误码。
 */
SYSCALL_DEFINE5(fchownat, int, dfd, const char __user *, filename, uid_t, user,
		gid_t, group, int, flag)
{
	struct path path;  // 文件路径结构
	int error = -EINVAL;  // 默认错误码，无效参数
	int follow;

	// 校验标志位，仅接受AT_SYMLINK_NOFOLLOW，其他均为非法
	if ((flag & ~AT_SYMLINK_NOFOLLOW) != 0)
		goto out;

	// 根据标志位决定是否遵循符号链接
	follow = (flag & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
	// 获取文件路径
	error = user_path_at(dfd, filename, follow, &path);
	if (error)
		goto out;  // 获取路径失败

	// 获取对应文件系统的写权限
	error = mnt_want_write(path.mnt);
	if (error)
		goto out_release;  // 获取写权限失败

	// 调用chown_common共通函数来改变文件所有者和组
	error = chown_common(&path, user, group);

	// 操作完成后释放写权限
	mnt_drop_write(path.mnt);

out_release:
	// 释放获取的路径
	path_put(&path);
out:
	// 返回操作结果
	return error;
}

/*
 * sys_lchown - 更改符号链接本身的所有者和组，而不是它指向的目标
 * @filename: 用户空间提供的文件名
 * @user: 新的用户ID
 * @group: 新的组ID
 *
 * 返回0表示成功，否则返回错误码。
 */
SYSCALL_DEFINE3(lchown, const char __user *, filename, uid_t, user, gid_t, group)
{
	struct path path;  // 文件路径结构
	int error;

	// 使用不解析最终目标的方式获取文件路径
	error = user_lpath(filename, &path);
	if (error)
		goto out;  // 获取路径失败

	// 获取对应文件系统的写权限
	error = mnt_want_write(path.mnt);
	if (error)
		goto out_release;  // 获取写权限失败

	// 调用chown_common共通函数来改变文件所有者和组，特别是对符号链接本身
	error = chown_common(&path, user, group);

	// 操作完成后释放写权限
	mnt_drop_write(path.mnt);

out_release:
	// 释放获取的路径
	path_put(&path);
out:
	// 返回操作结果
	return error;
}

/*
 * sys_fchown - 系统调用：通过文件描述符更改文件的所有者和组
 * @fd: 文件描述符
 * @user: 新的用户ID
 * @group: 新的组ID
 *
 * 如果成功，返回0；如果出错，返回错误码。
 */
SYSCALL_DEFINE3(fchown, unsigned int, fd, uid_t, user, gid_t, group)
{
	struct file *file;  // 文件指针
	int error = -EBADF;  // 默认错误代码为无效的文件描述符
	struct dentry *dentry;

	// 根据文件描述符获取文件指针
	file = fget(fd);
	if (!file)  // 如果文件指针为空，表示无效的文件描述符
		goto out;

	// 请求对文件所在的文件系统进行写操作
	error = mnt_want_write_file(file);
	if (error)  // 如果请求失败，则处理退出
		goto out_fput;

	// 获取文件的目录项
	dentry = file->f_path.dentry;
	// 进行审计，传递NULL表示当前进程的上下文
	audit_inode(NULL, dentry);
	// 调用chown_common函数来更改文件所有者和组
	error = chown_common(&file->f_path, user, group);
	// 操作完成后释放文件系统的写权限
	mnt_drop_write(file->f_path.mnt);

out_fput:
	// 释放获取的文件引用
	fput(file);
out:
	// 返回操作结果
	return error;
}

/*
 * You have to be very careful that these write
 * counts get cleaned up in error cases and
 * upon __fput().  This should probably never
 * be called outside of __dentry_open().
 */
/*
 * 在错误情况和__fput()时必须非常小心确保写入计数被清理。
 * 这个函数可能永远不应该在__dentry_open()之外被调用。
 */
// 处理文件的写访问权限请求。它首先尝试为一个文件（inode）获取写访问权限，如果文件是一个特殊文件（例如设备文件），
// 则不会尝试获取挂载点的写权限，因为特殊文件通常不需要对其所在文件系统进行写操作。如果文件不是特殊文件，
// 则继续请求挂载点的写权限。这是因为普通文件的写操作可能会改变文件系统的状态，如修改文件系统的元数据等。
static inline int __get_file_write_access(struct inode *inode,
					  struct vfsmount *mnt)
{
	int error;  // 错误码初始化
	error = get_write_access(inode);  // 尝试获取inode的写访问权限
	if (error)  // 如果获取写访问权限失败，返回错误码
		return error;

	/*
	 * Do not take mount writer counts on
	 * special files since no writes to
	 * the mount itself will occur.
	 */
	/*
	 * 不在特殊文件上获取挂载点的写计数，
	 * 因为没有实际写入挂载点本身的操作。
	 */
	if (!special_file(inode->i_mode)) {  // 如果inode不是特殊文件
		/*
		 * Balanced in __fput()
		 */
		/*
		 * 在__fput()中平衡
		 */
		error = mnt_want_write(mnt);  // 请求挂载点的写权限
		if (error)  // 如果请求挂载点的写权限失败
			put_write_access(inode);  // 释放inode的写访问权限
	}
	return error;  // 返回结果
}

// 这段代码实现了在给定的挂载点和目录项上打开文件的功能。它处理了权限验证、文件模式设置、安全性检查，并在失败时进行了适当的资源清理。
static struct file *__dentry_open(struct dentry *dentry, struct vfsmount *mnt,
					struct file *f,
					int (*open)(struct inode *, struct file *),
					const struct cred *cred)
{
	struct inode *inode;  // 指向文件的inode结构
	int error;  // 用于存储错误代码

	// 设置文件模式标志：从文件标志中导出并添加读写和寻找操作的标志
	// 初始化文件模式，设置文件读写和定位标志
	f->f_mode = OPEN_FMODE(f->f_flags) | FMODE_LSEEK |
				FMODE_PREAD | FMODE_PWRITE;
	inode = dentry->d_inode;  // 从dentry获取inode结构
	if (f->f_mode & FMODE_WRITE) {  // 如果打开文件为写模式
		error = __get_file_write_access(inode, mnt);  // 获取写访问权限
		if (error)
			goto cleanup_file;  // 如果失败，跳转到清理代码
		if (!special_file(inode->i_mode))  // 如果不是特殊文件
			file_take_write(f);  // 增加文件的写计数
	}

	// 设置文件映射、路径、位置指针初始化为0
	f->f_mapping = inode->i_mapping;
	f->f_path.dentry = dentry;
	f->f_path.mnt = mnt;
	f->f_pos = 0;	// 文件位置指针置为0
	f->f_op = fops_get(inode->i_fop);	// 获取文件操作函数
	file_move(f, &inode->i_sb->s_files);	// 将文件移至文件系统的文件链表

	// 执行安全性检查
	error = security_dentry_open(f, cred);
	if (error)
		goto cleanup_all;	// 安全检查失败，执行全部清理

	// 如果未提供特定的open函数，并且文件操作中定义了open，使用它
	if (!open && f->f_op)
		open = f->f_op->open;
	// 如果存在open函数，调用之
	if (open) {
		error = open(inode, f);
		if (error)
			goto cleanup_all;	// open函数调用失败处理
	}
	// 更新IMA（Integrity Measurement Architecture）计数
	ima_counts_get(f);	// 处理IMA计数

	// 清除不再需要的文件打开标志
	f->f_flags &= ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);	// 移除标志中的创建、排他打开、不分配控制终端和截断标志，这些标志对后续操作不再相关

	file_ra_state_init(&f->f_ra, f->f_mapping->host->i_mapping);	// 初始化文件的预读（read-ahead）状态，为后续的文件读取操作优化性能

	/* NB: we're sure to have correct a_ops only after f_op->open */
	// 注意：我们只有在 f_op->open 调用后才确保正确的 a_ops。因此，这里检查直接I/O的可用性。
	// 处理直接IO标志
	if (f->f_flags & O_DIRECT) {
		if (!f->f_mapping->a_ops ||
		    ((!f->f_mapping->a_ops->direct_IO) &&
		    (!f->f_mapping->a_ops->get_xip_mem))) {
			// 如果不支持直接I/O或者获取即时执行内存，释放文件对象，并返回错误
			fput(f);
			f = ERR_PTR(-EINVAL);
		}
	}

	return f;	// 返回文件结构指针

cleanup_all:
	fops_put(f->f_op);  // 释放文件操作函数
	if (f->f_mode & FMODE_WRITE) {  // 如果是写模式
		put_write_access(inode);  // 释放写访问权限
		if (!special_file(inode->i_mode)) {// 如果不是特殊文件，重置写状态并且减少挂载点的写操作计数
			/*
			 * We don't consider this a real
			 * mnt_want/drop_write() pair
			 * because it all happenend right
			 * here, so just reset the state.
			 */
			// 这不是一个真正的挂载点写/释放写操作对，因为这里只是重置状态
			file_reset_write(f);	// 重置写状态
			mnt_drop_write(mnt);	// 释放挂载点的写权限
		}
	}
	// 销毁文件对象，清除其在系统内的注册信息
	file_kill(f);	// 删除文件结构
	f->f_path.dentry = NULL;
	f->f_path.mnt = NULL;
cleanup_file:
	// 释放文件对象，减少目录项和挂载点的引用计数
	put_filp(f);  // 释放文件指针
	dput(dentry);  // 减少dentry的引用计数
	mntput(mnt);  // 减少mnt的引用计数
	return ERR_PTR(error);  // 返回错误
}

/**
 * lookup_instantiate_filp - instantiates the open intent filp
 * @nd: pointer to nameidata
 * @dentry: pointer to dentry
 * @open: open callback
 *
 * Helper for filesystems that want to use lookup open intents and pass back
 * a fully instantiated struct file to the caller.
 * This function is meant to be called from within a filesystem's
 * lookup method.
 * Beware of calling it for non-regular files! Those ->open methods might block
 * (e.g. in fifo_open), leaving you with parent locked (and in case of fifo,
 * leading to a deadlock, as nobody can open that fifo anymore, because
 * another process to open fifo will block on locked parent when doing lookup).
 * Note that in case of error, nd->intent.open.file is destroyed, but the
 * path information remains valid.
 * If the open callback is set to NULL, then the standard f_op->open()
 * filesystem callback is substituted.
 */
/**
 * lookup_instantiate_filp - 实例化打开意图的文件对象
 * @nd: nameidata的指针
 * @dentry: dentry的指针
 * @open: 打开回调函数
 *
 * 为希望使用查找打开意图并将完全实例化的struct file返回给调用者的文件系统的帮助函数。
 * 此函数旨在从文件系统的lookup方法内部调用。
 * 警告：不要为非常规文件调用它！那些->open方法可能会阻塞
 * （例如，在fifo_open中），留下父目录锁定（并且在fifo的情况下，
 * 导致死锁，因为没有人能再打开那个fifo，因为
 * 另一个进程打开fifo时会因为锁定的父目录而在查找时阻塞）。
 * 注意，如果出错，nd->intent.open.file会被销毁，但路径信息仍然有效。
 * 如果打开回调设置为NULL，则替换为标准的f_op->open()
 * 文件系统回调。
 */
struct file *lookup_instantiate_filp(struct nameidata *nd, struct dentry *dentry,
		int (*open)(struct inode *, struct file *))
{
	const struct cred *cred = current_cred();  // 获取当前的凭证

	if (IS_ERR(nd->intent.open.file))  // 检查文件对象是否出错
		goto out;
	if (IS_ERR(dentry))  // 检查dentry是否出错
		goto out_err;
	// 调用__dentry_open实例化文件对象，传入dentry，mntget获取mnt的引用，文件对象，打开回调和凭证
	nd->intent.open.file = __dentry_open(dget(dentry), mntget(nd->path.mnt),
					     nd->intent.open.file,
					     open, cred);
out:
	return nd->intent.open.file;  // 返回文件对象
out_err:
	release_open_intent(nd);  // 释放打开意图
	nd->intent.open.file = (struct file *)dentry;  // 设置错误信息
	goto out;
}
EXPORT_SYMBOL_GPL(lookup_instantiate_filp);

/**
 * nameidata_to_filp - convert a nameidata to an open filp.
 * @nd: pointer to nameidata
 * @flags: open flags
 *
 * Note that this function destroys the original nameidata
 */
/**
 * nameidata_to_filp - 将nameidata转换为打开的文件对象。
 * @nd: 指向nameidata的指针
 * @flags: 打开文件时使用的标志
 *
 * 请注意，此函数会销毁原始的nameidata。
 */
struct file *nameidata_to_filp(struct nameidata *nd)
{
	const struct cred *cred = current_cred();  // 获取当前的认证信息
	struct file *filp;

	/* Pick up the filp from the open intent */
	/* 从打开意图中获取文件对象 */
	filp = nd->intent.open.file;
	/* Has the filesystem initialised the file for us? */
	/* 文件系统是否已为我们初始化了文件对象？ */
	if (filp->f_path.dentry == NULL)
		filp = __dentry_open(nd->path.dentry, nd->path.mnt, filp,
				     NULL, cred);  // 如果没有初始化，则调用__dentry_open进行初始化
	else
		path_put(&nd->path);  // 如果已经初始化，释放路径对象
	return filp;  // 返回文件对象
}

/*
 * dentry_open() will have done dput(dentry) and mntput(mnt) if it returns an
 * error.
 */
/**
 * dentry_open() 在返回错误时将执行dput(dentry)和mntput(mnt)。
 */
// 用于在给定目录项（dentry）、挂载点（mnt）、打开标志和用户凭证的情况下，创建并打开一个文件。
struct file *dentry_open(struct dentry *dentry, struct vfsmount *mnt, int flags,
			 const struct cred *cred)
{
	int error;
	struct file *f;

	validate_creds(cred);  // 验证传入的凭证是否有效

	/*
	 * We must always pass in a valid mount pointer.   Historically
	 * callers got away with not passing it, but we must enforce this at
	 * the earliest possible point now to avoid strange problems deep in the
	 * filesystem stack.
	 */
	/*
	 * 我们必须始终传入一个有效的挂载点指针。历史上调用者可以不传递它，但现在为了避免文件系统堆栈中的奇怪问题，
	 * 我们必须尽早强制执行此检查。
	 */
	if (!mnt) {
		printk(KERN_WARNING "%s called with NULL vfsmount\n", __func__);  // 如果挂载点为NULL，打印警告信息
		dump_stack();  // 打印堆栈信息，帮助调试
		return ERR_PTR(-EINVAL);  // 返回错误指针
	}

	error = -ENFILE;  // 初始化错误码为"文件太多"
	f = get_empty_filp();  // 获取一个空的文件指针结构
	if (f == NULL) {  // 如果无法获取文件指针
		dput(dentry);  // 释放目录项引用
		mntput(mnt);  // 释放挂载点引用
		return ERR_PTR(error);  // 返回错误指针
	}

	f->f_flags = flags;  // 设置文件打开标志
	return __dentry_open(dentry, mnt, f, NULL, cred);  // 调用 __dentry_open 函数打开文件
}
EXPORT_SYMBOL(dentry_open);

// __put_unused_fd - 内部函数用于实际释放文件描述符在文件表中的占位
static void __put_unused_fd(struct files_struct *files, unsigned int fd)
{
	struct fdtable *fdt = files_fdtable(files);  // 获取当前进程的文件描述符表
	__FD_CLR(fd, fdt->open_fds);  // 清除指定文件描述符的占位，表明这个描述符现在是可用的
	if (fd < files->next_fd)  // 如果释放的文件描述符编号小于当前最小的未使用的文件描述符编号
		files->next_fd = fd;  // 更新这个最小的未使用文件描述符编号为当前释放的描述符编号
}

// put_unused_fd - 公开的函数用于释放未使用的文件描述符
void put_unused_fd(unsigned int fd)
{
	struct files_struct *files = current->files;  // 获取当前进程的文件结构指针
	spin_lock(&files->file_lock);  // 上锁，因为可能多个线程同时操作文件表
	__put_unused_fd(files, fd);  // 调用内部函数释放文件描述符
	spin_unlock(&files->file_lock);  // 解锁
}
EXPORT_SYMBOL(put_unused_fd);

/*
 * Install a file pointer in the fd array.
 *
 * The VFS is full of places where we drop the files lock between
 * setting the open_fds bitmap and installing the file in the file
 * array.  At any such point, we are vulnerable to a dup2() race
 * installing a file in the array before us.  We need to detect this and
 * fput() the struct file we are about to overwrite in this case.
 *
 * It should never happen - if we allow dup2() do it, _really_ bad things
 * will follow.
 */
/*
 * 在文件描述符数组中安装一个文件指针。
 *
 * VFS中有很多地方在设置open_fds位图和将文件安装到文件数组之间会释放文件锁。
 * 在任何这样的点，我们都容易受到dup2()竞争的影响，dup2()可能会在我们之前将文件
 * 安装到数组中。我们需要检测到这一点，并在这种情况下对我们即将覆盖的struct file进行fput()操作。
 *
 * 这种情况本不应该发生 - 如果我们允许dup2()这样做，会有非常糟糕的后果。
 */
void fd_install(unsigned int fd, struct file *file)
{
	struct files_struct *files = current->files; // 获取当前进程的文件结构体
	struct fdtable *fdt;
	spin_lock(&files->file_lock); // 对文件锁进行上锁，保证线程安全
	fdt = files_fdtable(files);  // 获取文件描述符表
	BUG_ON(fdt->fd[fd] != NULL);  // 如果该描述符已被使用，则触发BUG
	rcu_assign_pointer(fdt->fd[fd], file); // 使用RCU机制安全地分配文件指针到描述符
	spin_unlock(&files->file_lock); // 解锁
}
EXPORT_SYMBOL(fd_install);

// 负责处理打开或创建文件的系统调用。
long do_sys_open(int dfd, const char __user *filename, int flags, int mode)
{
	char *tmp = getname(filename);  // 从用户空间获取文件名，并复制到内核空间
	int fd = PTR_ERR(tmp);  // 如果getname失败，获取错误代码

	if (!IS_ERR(tmp)) {  // 检查文件名是否成功获取
		fd = get_unused_fd_flags(flags);  // 获取一个未使用的文件描述符
		if (fd >= 0) {  // 检查文件描述符是否有效
			struct file *f = do_filp_open(dfd, tmp, flags, mode, 0);  // 尝试打开或创建文件
			if (IS_ERR(f)) {  // 检查文件是否成功打开
				put_unused_fd(fd);  // 如果打开文件失败，释放之前获取的文件描述符
				fd = PTR_ERR(f);  // 获取错误代码
			} else {
				fsnotify_open(f->f_path.dentry);  // 发送文件打开通知
				fd_install(fd, f);  // 安装文件指针到文件描述符表中
			}
		}
		putname(tmp);  // 释放由getname分配的内存
	}
	return fd;  // 返回文件描述符或错误代码
}

// 用来打开或创建一个文件的接口。
SYSCALL_DEFINE3(open, const char __user *, filename, int, flags, int, mode)
{
	long ret;  // 定义用于返回结果的变量

	if (force_o_largefile())  // 检查是否需要强制使用大文件支持
		flags |= O_LARGEFILE;  // 如果是，修改标志以包含 O_LARGEFILE

	ret = do_sys_open(AT_FDCWD, filename, flags, mode);  // 调用 do_sys_open 函数实际执行文件打开操作
	/* avoid REGPARM breakage on x86: */
	asmlinkage_protect(3, ret, filename, flags, mode);  // 用于确保在 x86 架构上，通过寄存器传递参数不会导致问题
	return ret;  // 返回文件操作的结果，可以是文件描述符或错误码
}

// open 系统调用的扩展，允许相对于一个目录文件描述符打开文件。
SYSCALL_DEFINE4(openat, int, dfd, const char __user *, filename, int, flags,
		int, mode)
{
	long ret;  // 定义用于返回结果的变量

	if (force_o_largefile())  // 检查是否需要强制使用大文件支持，通常是超过 2GB 的文件
		flags |= O_LARGEFILE;  // 如果是，修改标志以包含 O_LARGEFILE

	ret = do_sys_open(dfd, filename, flags, mode);  // 调用 do_sys_open 函数实际执行文件打开操作
	/* avoid REGPARM breakage on x86: */
	asmlinkage_protect(4, ret, dfd, filename, flags, mode);  // 用于确保在 x86 架构上，通过寄存器传递参数不会导致问题
	return ret;  // 返回文件操作的结果，可以是文件描述符或错误码
}

#ifndef __alpha__  // 检查是否在 Alpha 架构上编译，如果不是，则包含以下代码

/*
 * For backward compatibility?  Maybe this should be moved
 * into arch/i386 instead?
 */
// 为了向后兼容？或许这部分代码应该移动到 arch/i386 目录下？
// 使用宏 SYSCALL_DEFINE2 定义了一个名为 creat 的系统调用，接收两个参数：文件路径和文件模式
// 用于创建新文件或重写已存在文件的传统 UNIX 系统调用。
SYSCALL_DEFINE2(creat, const char __user *, pathname, int, mode)
{
	// 调用 sys_open 函数以 O_CREAT | O_WRONLY | O_TRUNC 标志打开或创建文件
	// O_CREAT - 如果指定文件不存在，则会创建一个新文件
	// O_WRONLY - 打开文件只允许写操作
	// O_TRUNC - 如果文件已存在且是一个普通文件，且以写入模式打开，则其长度将被截断为0
	return sys_open(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

#endif  // 结束宏定义检查

/*
 * "id" is the POSIX thread ID. We use the
 * files pointer for this..
 */
/*
 * "id" is the POSIX thread ID. We use the
 * files pointer for this..
 */
// "id" 是 POSIX 线程ID。我们使用文件指针来表示这个。
// 定义 filp_close 函数，接收一个文件指针和一个所有者（线程）标识
// 用于关闭一个文件描述符，并执行一些清理操作。
int filp_close(struct file *filp, fl_owner_t id)
{
	// 初始化返回值为 0
	int retval = 0;

	if (!file_count(filp)) {
		// 如果文件引用计数为0，打印错误信息
		printk(KERN_ERR "VFS: Close: file count is 0\n");
		// 直接返回0，不进行后续操作
		return 0;
	}

	// 如果文件操作结构中定义了 flush 方法，则调用它来清空缓存等操作，将结果赋值给 retval
	if (filp->f_op && filp->f_op->flush)
		retval = filp->f_op->flush(filp, id);

	// 调用 dnotify_flush 清理与目录更改通知相关的资源
	dnotify_flush(filp, id);

	// 移除文件上的 POSIX 锁
	locks_remove_posix(filp, id);

	// 递减文件引用计数，并在计数为0时释放文件资源
	fput(filp);

	// 返回操作结果
	return retval;
}
EXPORT_SYMBOL(filp_close);

/*
 * Careful here! We test whether the file pointer is NULL before
 * releasing the fd. This ensures that one clone task can't release
 * an fd while another clone is opening it.
 */
/*
 * Careful here! We test whether the file pointer is NULL before
 * releasing the fd. This ensures that one clone task can't release
 * an fd while another clone is opening it.
 */
// 注意：在释放文件描述符之前，我们会检查文件指针是否为NULL。
// 这确保了一个克隆任务不能在另一个克隆任务正在打开文件描述符时释放它。
// 定义系统调用 close，接受一个文件描述符作为参数
SYSCALL_DEFINE1(close, unsigned int, fd)
{
	// 定义局部变量：文件结构指针，当前进程的文件表，文件描述符表，返回值
	struct file * filp;
	struct files_struct *files = current->files;
	struct fdtable *fdt;
	int retval;

	// 上锁，以保护文件表
	spin_lock(&files->file_lock);
	// 获取文件描述符表
	fdt = files_fdtable(files);
	// 如果文件描述符超出最大值，则直接跳转至解锁部分
	if (fd >= fdt->max_fds)
		goto out_unlock;
	// 获取文件描述符指向的文件结构
	filp = fdt->fd[fd];
	// 如果文件结构为空，同样跳转至解锁部分
	if (!filp)
		goto out_unlock;
	// 将文件描述符对应的指针设置为NULL，实现释放
	rcu_assign_pointer(fdt->fd[fd], NULL);
	// 清除执行时关闭的标志
	FD_CLR(fd, fdt->close_on_exec);
	// 标记文件描述符为未使用
	__put_unused_fd(files, fd);
	// 解锁
	spin_unlock(&files->file_lock);
	// 关闭文件，并获取返回值
	retval = filp_close(filp, files);

	/* can't restart close syscall because file table entry was cleared */
	// 不能重新启动 close 系统调用，因为文件表条目已被清除
	if (unlikely(retval == -ERESTARTSYS ||
		     retval == -ERESTARTNOINTR ||
		     retval == -ERESTARTNOHAND ||
		     retval == -ERESTART_RESTARTBLOCK))
		// 如果返回是重新启动的错误代码，则转换为中断错误代码
		retval = -EINTR;

	// 返回结果
	return retval;

out_unlock:
	// 解锁并返回“坏文件描述符”错误
	spin_unlock(&files->file_lock);
	return -EBADF;
}
EXPORT_SYMBOL(sys_close);

/*
 * This routine simulates a hangup on the tty, to arrange that users
 * are given clean terminals at login time.
 */
// 这个程序模拟在TTY上进行挂断，以确保用户在登录时获得一个干净的终端。
// 定义无参数的系统调用 vhangup
// 该调用用于模拟在终端（TTY）上发生挂断，以确保用户在登录时获得一个干净的终端。
SYSCALL_DEFINE0(vhangup)
{
	// 如果当前进程拥有系统TTY配置的能力（即权限）
	if (capable(CAP_SYS_TTY_CONFIG)) {
		// 调用函数 tty_vhangup_self 来执行实际的挂断操作
		tty_vhangup_self();
		// 如果操作成功，返回0
		return 0;
	}
	// 如果当前进程不具备必要的权限，返回错误代码 EPERM，表示操作不允许
	return -EPERM;
}

/*
 * Called when an inode is about to be open.
 * We use this to disallow opening large files on 32bit systems if
 * the caller didn't specify O_LARGEFILE.  On 64bit systems we force
 * on this flag in sys_open.
 */

/* 
 * 当一个 inode（索引节点）即将被打开时调用此函数。
 * 我们使用这个函数来阻止在32位系统上打开大文件，
 * 除非调用者指定了 O_LARGEFILE 标志。
 * 在64位系统上，我们会在 sys_open 中强制启用这个标志。
 */
// 定义一个函数 generic_file_open，该函数接受一个指向 inode 结构体的指针 inode 和一个指向 file 结构体的指针 filp 作为参数。

// 定义了一个 generic_file_open 函数，该函数在打开文件时会检查文件的大小，以确保在32位系统上没有指定 O_LARGEFILE 标志的情况下，
// 不能打开超出非大文件支持最大限制的文件（通常是2GB）。如果文件大小超出了限制，则返回 -EOVERFLOW 错误，否则允许打开文件并返回0。
int generic_file_open(struct inode * inode, struct file * filp)
{
	// 如果文件描述符的标志位中不包含 O_LARGEFILE 标志，并且 inode 的大小超过了 MAX_NON_LFS（非大文件支持的最大大小）
	if (!(filp->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		// 返回 -EOVERFLOW 错误码，表示文件太大，导致溢出
		return -EOVERFLOW;
	// 否则，返回 0，表示成功打开文件
	return 0;
}
EXPORT_SYMBOL(generic_file_open);

/*
 * This is used by subsystems that don't want seekable
 * file descriptors
 */
/*
 * This is used by subsystems that don't want seekable
 * file descriptors
 */
/*
 *  这个函数用于那些不需要可定位文件描述符的子系统。
 * 
 * 可定位文件描述符允许在文件中移动文件指针，即支持 seek 操作。
 */

// 定义一个名为 nonseekable_open 的函数，该函数接受一个指向 inode 结构体的指针 inode 和一个指向 file 结构体的指针 filp 作为参数。

// 用于将文件描述符设置为不可定位。换句话说，调用该函数的子系统将不支持 seek、pread 和 pwrite 操作。
// 这通常用于不需要这些功能的子系统，比如某些类型的设备文件或管道。
int nonseekable_open(struct inode *inode, struct file *filp)
{
	// 将文件描述符的模式标志位中的 FMODE_LSEEK、FMODE_PREAD 和 FMODE_PWRITE 标志清除。
	// FMODE_LSEEK 表示文件支持定位操作（seek）。
	// FMODE_PREAD 表示文件支持位置独立的读取操作（pread）。
	// FMODE_PWRITE 表示文件支持位置独立的写入操作（pwrite）。
	// 清除这些标志意味着文件描述符不再支持这些操作，即文件描述符将变为不可定位的。
	filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);

	// 返回 0，表示成功。
	return 0;
}

EXPORT_SYMBOL(nonseekable_open);
