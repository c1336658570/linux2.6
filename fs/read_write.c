/*
 *  linux/fs/read_write.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/slab.h> 
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/smp_lock.h>
#include <linux/fsnotify.h>
#include <linux/security.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/pagemap.h>
#include <linux/splice.h>
#include "read_write.h"

#include <asm/uaccess.h>
#include <asm/unistd.h>

// 定义一个名为 generic_ro_fops 的只读文件操作结构体。
const struct file_operations generic_ro_fops = {
	.llseek		= generic_file_llseek,      // 文件寻址操作。允许文件内部寻址。
	.read		= do_sync_read,             // 同步读文件操作。从文件中同步读取数据。
	.aio_read	= generic_file_aio_read,    // 异步读文件操作。从文件中异步读取数据。
	.mmap		= generic_file_readonly_mmap, // 将文件映射到内存操作。仅支持只读映射。
	.splice_read	= generic_file_splice_read, // 利用splice机制读取文件。允许在两个文件描述符之间移动数据，而不需要将数据复制到用户空间。
};
EXPORT_SYMBOL(generic_ro_fops);

/**
 * generic_file_llseek_unlocked - lockless generic llseek implementation
 * @file:	file structure to seek on
 * @offset:	file offset to seek to
 * @origin:	type of seek
 *
 * Updates the file offset to the value specified by @offset and @origin.
 * Locking must be provided by the caller.
 */
/**
 * generic_file_llseek_unlocked - 无锁的通用文件寻址实现
 * @file:   需要寻址的文件结构体
 * @offset: 要寻址到的文件偏移量
 * @origin: 寻址的类型
 *
 * 根据 @offset 和 @origin 指定的值更新文件偏移量。
 * 锁定必须由调用者提供。
 */
loff_t
generic_file_llseek_unlocked(struct file *file, loff_t offset, int origin)
{
	struct inode *inode = file->f_mapping->host;  // 获取文件的 inode 结构

	switch (origin) {
	case SEEK_END:  // 寻址类型为从文件末尾开始
		offset += inode->i_size;  // 将偏移量设置为文件大小加上指定的偏移
		break;
	case SEEK_CUR:  // 寻址类型为从当前位置开始
		/*
		 * Here we special-case the lseek(fd, 0, SEEK_CUR)
		 * position-querying operation.  Avoid rewriting the "same"
		 * f_pos value back to the file because a concurrent read(),
		 * write() or lseek() might have altered it
		 */
		/*
		 * 特殊处理 lseek(fd, 0, SEEK_CUR) 这种查询当前位置的操作。
		 * 避免将 "相同" 的 f_pos 值回写到文件，因为可能有并发的 read()、
		 * write() 或 lseek() 修改了它。
		 */
		if (offset == 0)  // 如果偏移量为 0，即只查询当前位置，直接返回当前位置
			return file->f_pos;
		offset += file->f_pos;  // 将偏移量设置为当前位置加上指定的偏移
		break;
	}

	// 检查新的偏移量是否有效
	if (offset < 0 || offset > inode->i_sb->s_maxbytes)
		return -EINVAL;  // 如果偏移量无效，返回错误

	/* Special lock needed here? */
	// 特殊锁定需要在此处吗？
	if (offset != file->f_pos) {
		file->f_pos = offset;  // 更新文件的当前位置
		file->f_version = 0;  // 重置文件版本
	}

	return offset;  // 返回新的偏移量
}
EXPORT_SYMBOL(generic_file_llseek_unlocked);

/**
 * generic_file_llseek - generic llseek implementation for regular files
 * @file:	file structure to seek on
 * @offset:	file offset to seek to
 * @origin:	type of seek
 *
 * This is a generic implemenation of ->llseek useable for all normal local
 * filesystems.  It just updates the file offset to the value specified by
 * @offset and @origin under i_mutex.
 */
/**
 * generic_file_llseek - 常规文件的通用 llseek 实现
 * @file:   需要寻址的文件结构体
 * @offset: 要寻址到的文件偏移量
 * @origin: 寻址的类型
 *
 * 这是一个通用的 llseek 实现，适用于所有常规的本地文件系统。
 * 它在 i_mutex 的保护下更新文件偏移量到由 @offset 和 @origin 指定的值。
 */
loff_t generic_file_llseek(struct file *file, loff_t offset, int origin)
{
	loff_t rval;  // 用来存储寻址结果

	// 加锁文件的互斥锁，保证线程安全
	mutex_lock(&file->f_dentry->d_inode->i_mutex);
	// 调用无锁版本的寻址函数进行实际的寻址操作
	rval = generic_file_llseek_unlocked(file, offset, origin);
	// 解锁文件的互斥锁
	mutex_unlock(&file->f_dentry->d_inode->i_mutex);

	return rval;  // 返回寻址结果
}
EXPORT_SYMBOL(generic_file_llseek);

/*
 * no_llseek - 无操作的 llseek 实现
 * @file: 文件结构体指针，实际未使用
 * @offset: 寻址偏移量，实际未使用
 * @origin: 寻址起点类型，实际未使用
 * 返回值：始终返回 -ESPIPE，表示对此类型文件不支持寻址操作
 *
 * 此函数对所有传入参数不进行操作，直接返回错误码 -ESPIPE，
 * 表明某些文件类型（如管道）不支持寻址。
 */
loff_t no_llseek(struct file *file, loff_t offset, int origin)
{
	return -ESPIPE; // 返回错误码 -ESPIPE，表示管道错误，通常用于非寻址的文件类型。
}
EXPORT_SYMBOL(no_llseek);

/*
 * default_llseek - 为常规文件提供的默认寻址实现
 * @file: 需要寻址的文件结构体
 * @offset: 要设置的文件偏移量
 * @origin: 寻址的起点类型，如 SEEK_SET, SEEK_CUR, SEEK_END
 * 返回值：新的文件偏移量或错误码
 *
 * 此函数是一个通用的文件偏移调整实现，用于大多数文件系统。
 * 它通过加锁确保操作的原子性，并根据 origin 指示的寻址方式调整 offset，
 * 最后更新文件的 f_pos 成员并返回新的偏移量或错误。
 */
loff_t default_llseek(struct file *file, loff_t offset, int origin)
{
	loff_t retval; // 用于存储计算后的结果或错误码

	lock_kernel(); // 锁定内核，防止并发问题
	switch (origin) { // 根据起点类型进行不同处理
		case SEEK_END: // 从文件末尾开始寻址
			// 读取文件大小，并与偏移量相加
			offset += i_size_read(file->f_path.dentry->d_inode);
			break;
		case SEEK_CUR: // 从当前位置开始寻址
			if (offset == 0) { // 特殊情况：查询当前位置
				retval = file->f_pos; // 直接返回当前文件位置
				goto out; // 跳过后续处理
			}
			offset += file->f_pos; // 将当前位置与偏移量相加
	}
	retval = -EINVAL; // 默认返回无效参数错误
	if (offset >= 0) { // 检查新的偏移量是否有效
		if (offset != file->f_pos) { // 如果偏移量有变化
			file->f_pos = offset; // 更新文件的当前位置
			file->f_version = 0; // 重置文件版本标记
		}
		retval = offset; // 返回新的偏移量
	}
out:
	unlock_kernel(); // 解锁内核
	return retval; // 返回计算的结果或错误码
}
EXPORT_SYMBOL(default_llseek);

/*
 * vfs_llseek - VFS 层的通用 llseek 实现
 * @file: 需要寻址的文件结构体
 * @offset: 要设置的文件偏移量
 * @origin: 寻址的起点类型
 * 返回值：新的文件偏移量或错误码
 *
 * 此函数根据文件的 f_mode 和 f_op 配置动态选择适当的 llseek 实现。
 * 它首先假设 no_llseek 为默认实现（通常用于不支持寻址的文件类型）。
 * 如果文件模式允许寻址（FMODE_LSEEK），则默认使用 default_llseek。
 * 如果文件操作结构体中存在自定义的 llseek 函数，那么将使用该函数进行寻址。
 * 最终调用选定的函数，并传入相应的参数。
 */
loff_t vfs_llseek(struct file *file, loff_t offset, int origin)
{
	loff_t (*fn)(struct file *, loff_t, int);  // 函数指针，用于指向具体的 llseek 实现

	fn = no_llseek;  // 默认使用 no_llseek 函数，适用于不支持寻址的文件类型
	if (file->f_mode & FMODE_LSEEK) {  // 检查文件模式是否允许寻址
		fn = default_llseek;  // 如果允许，使用 default_llseek 作为默认实现
		if (file->f_op && file->f_op->llseek)  // 如果文件操作结构体中定义了自定义的 llseek 函数
			fn = file->f_op->llseek;  // 使用自定义的 llseek 函数
	}
	return fn(file, offset, origin);  // 调用选定的 llseek 函数实现，并返回结果
}
EXPORT_SYMBOL(vfs_llseek);

/*
 * SYSCALL_DEFINE3(lseek, unsigned int, fd, off_t, offset, unsigned int, origin)
 * 系统调用：lseek
 * @fd: 文件描述符
 * @offset: 寻址偏移量
 * @origin: 寻址起点
 * 返回值：新的文件偏移量或错误码
 *
 * lseek 系统调用用于设置文件的读/写偏移量，根据 origin 参数确定偏移量的计算方式。
 * 如果调用成功，返回新的读/写偏移量；如果失败，返回相应的错误码。
 */
SYSCALL_DEFINE3(lseek, unsigned int, fd, off_t, offset, unsigned int, origin)
{
	off_t retval;             // 函数返回值
	struct file *file;        // 文件指针
	int fput_needed;          // 标记是否需要释放文件引用

	retval = -EBADF;          // 默认返回值，文件描述符无效
	file = fget_light(fd, &fput_needed);  // 轻量级获取文件指针
	if (!file)
		goto bad;             // 如果文件指针为空，跳转到错误处理代码

	retval = -EINVAL;         // 如果文件指针有效，设置默认错误为无效参数
	if (origin <= SEEK_MAX) {  // 检查寻址起点是否有效
		loff_t res = vfs_llseek(file, offset, origin);  // 调用 VFS 层的 llseek
		retval = res;         // 尝试更新文件偏移量
		if (res != (loff_t)retval)
			/* LFS: should only happen on 32 bit platforms */
			retval = -EOVERFLOW;  // LFS: 应该只在32位平台发生，偏移量溢出
	}
	fput_light(file, fput_needed);  // 释放文件引用

bad:  // 错误处理标签
	return retval;  // 返回操作结果
}

#ifdef __ARCH_WANT_SYS_LLSEEK
/*
 * SYSCALL_DEFINE5(llseek, unsigned int, fd, unsigned long, offset_high,
 *                 unsigned long, offset_low, loff_t __user *, result,
 *                 unsigned int, origin)
 * 系统调用：llseek
 * @fd: 文件描述符
 * @offset_high: 偏移量的高32位
 * @offset_low: 偏移量的低32位
 * @result: 用户空间的指针，用于存放最终的偏移结果
 * @origin: 寻址的起点类型
 * 返回值：操作的结果状态码
 *
 * llseek 系统调用允许用户空间程序设置大文件的读/写偏移量。
 * 它通过接受一个高位和一个低位来构造一个完整的64位偏移量。
 * 如果操作成功，结果将被写回到用户空间，函数返回0；如果失败，返回相应的错误码。
 */
SYSCALL_DEFINE5(llseek, unsigned int, fd, unsigned long, offset_high,
		unsigned long, offset_low, loff_t __user *, result,
		unsigned int, origin)
{
	int retval;            // 用于存放返回值
	struct file *file;     // 文件指针
	loff_t offset;         // 完整的64位偏移量
	int fput_needed;       // 标记是否需要释放文件引用

	retval = -EBADF;       // 默认返回文件描述符无效错误
	file = fget_light(fd, &fput_needed);  // 轻量级获取文件指针
	if (!file)
		goto bad;          // 如果文件指针为空，跳转到错误处理代码

	retval = -EINVAL;      // 如果文件指针有效，设置默认错误为无效参数
	if (origin > SEEK_MAX)
		goto out_putf;     // 如果起点类型无效，跳转到释放文件引用并返回

	// 构造完整的64位偏移量
	offset = vfs_llseek(file, ((loff_t) offset_high << 32) | offset_low, origin);

	retval = (int)offset;  // 将偏移量转换为整型存储到retval
	if (offset >= 0) {     // 如果偏移量有效
		retval = -EFAULT;  // 设置默认错误为复制到用户空间失败
		// 将偏移量复制回用户空间
		if (!copy_to_user(result, &offset, sizeof(offset)))
			retval = 0;   // 如果复制成功，设置返回值为0
	}
out_putf:
	fput_light(file, fput_needed);  // 释放文件引用
bad:
	return retval;  // 返回操作结果
}
#endif

/*
 * rw_verify_area doesn't like huge counts. We limit
 * them to something that fits in "int" so that others
 * won't have to do range checks all the time.
 */
/*
 * rw_verify_area 函数不喜欢处理过大的 count 值。我们将其限制在一个符合 int 类型的范围内，
 * 这样其他函数就不需要一直进行范围检查。
 */
#define MAX_RW_COUNT (INT_MAX & PAGE_CACHE_MASK)	// 定义最大的读写计数，与页面缓存掩码进行与操作，确保不超出 int 范围。

/*
 * 函数 rw_verify_area
 * @read_write: 指示读操作还是写操作。
 * @file: 操作的文件结构体指针。
 * @ppos: 文件内的位置指针。
 * @count: 请求读写的字节数。
 * 返回值：经过检查后安全的 count 值，或在出错时返回错误码。
 */
int rw_verify_area(int read_write, struct file *file, loff_t *ppos, size_t count)
{
	struct inode *inode;  // 文件的 inode 结构体
	loff_t pos;           // 文件内的位置
	int retval = -EINVAL; // 初始设定返回值为无效参数错误码

	// 获取 inode 结构体
	inode = file->f_path.dentry->d_inode;
	// 如果 count 为负，直接返回错误
	if (unlikely((ssize_t) count < 0))
		return retval;
	pos = *ppos;  // 从指针中取得位置值
	// 检查位置是否有效（不小于0，且加上 count 后不溢出）
	if (unlikely((pos < 0) || (loff_t) (pos + count) < 0))
		return retval;

	// 如果 inode 上存在强制锁，并且该锁被激活
	if (unlikely(inode->i_flock && mandatory_lock(inode))) {
		// 调用 locks_mandatory_area 函数检查区域是否被锁定
		retval = locks_mandatory_area(
			read_write == READ ? FLOCK_VERIFY_READ : FLOCK_VERIFY_WRITE,
			inode, file, pos, count);
		// 如果锁定检查出错，返回错误
		if (retval < 0)
			return retval;
	}
	// 检查文件访问权限
	retval = security_file_permission(file,
				read_write == READ ? MAY_READ : MAY_WRITE);
	// 如果权限检查失败，返回错误
	if (retval)
		return retval;
	// 如果 count 大于 MAX_RW_COUNT，则返回 MAX_RW_COUNT，否则返回 count
	return count > MAX_RW_COUNT ? MAX_RW_COUNT : count;
}

// 在同步 I/O 控制块上等待，直到可以重试读操作
static void wait_on_retry_sync_kiocb(struct kiocb *iocb)
{
	// 设置当前任务状态为不可中断
	set_current_state(TASK_UNINTERRUPTIBLE);
	// 检查 I/O 控制块是否被标记为需要重试
	if (!kiocbIsKicked(iocb))
		schedule();	// 如果没有被标记，则挂起当前任务，等待被唤醒
	else
		kiocbClearKicked(iocb);	// 如果被标记了，清除标记
		// 恢复任务状态为运行中
	__set_current_state(TASK_RUNNING);
}

ssize_t do_sync_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	// 初始化读取数据的向量
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	struct kiocb kiocb;	// 定义一个同步 I/O 控制块
	ssize_t ret;

	// 初始化 I/O 控制块
	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = *ppos;	// 设置起始读取位置
	kiocb.ki_left = len;	// 设置剩余要读取的字节数
	kiocb.ki_nbytes = len;	// 设置总共要读取的字节数

	for (;;) {
		// 尝试进行异步 I/O 读取
		ret = filp->f_op->aio_read(&kiocb, &iov, 1, kiocb.ki_pos);
		// 如果不需要重试，则跳出循环
		if (ret != -EIOCBRETRY)
			break;
		// 如果需要重试，则等待
		wait_on_retry_sync_kiocb(&kiocb);
	}

	// 如果 I/O 被队列化了
	if (-EIOCBQUEUED == ret)
		// 等待 I/O 完成
		ret = wait_on_sync_kiocb(&kiocb);
	// 更新文件位置
	*ppos = kiocb.ki_pos;
	return ret;	// 返回读取结果
}

EXPORT_SYMBOL(do_sync_read);

ssize_t vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	// 检查文件是否允许读取
	if (!(file->f_mode & FMODE_READ))
		return -EBADF;	// 如果文件不允许读取，返回错误
	// 检查文件操作指针是否存在，以及对应的读函数是否存在
	if (!file->f_op || (!file->f_op->read && !file->f_op->aio_read))
		return -EINVAL;
	// 检查用户空间的缓冲区是否可以写入
	if (unlikely(!access_ok(VERIFY_WRITE, buf, count)))
		return -EFAULT;

	// 检查读取操作是否会超出文件大小或者存在其他问题
	ret = rw_verify_area(READ, file, pos, count);
	if (ret >= 0) {	// 如果没有问题
		count = ret;	// 更新读取字节数
		// 如果定义了直接读取函数，使用之
		if (file->f_op->read)
			ret = file->f_op->read(file, buf, count, pos);
		else	// 如果没有定义，使用同步读取函数
			ret = do_sync_read(file, buf, count, pos);
		if (ret > 0) {	// 如果读取成功
			// 通知文件被访问
			fsnotify_access(file->f_path.dentry);
			// 增加当前进程的读取计数
			add_rchar(current, ret);
		}
		inc_syscr(current);	// 增加系统读取调用计数
	}

	return ret;	// 返回读取的字节数或错误码
}

EXPORT_SYMBOL(vfs_read);

// 处理同步写操作，将用户空间的数据写入文件。
ssize_t do_sync_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
	struct iovec iov = { .iov_base = (void __user *)buf, .iov_len = len };
	struct kiocb kiocb;	// 内核异步I/O控制块
	ssize_t ret;

	// 初始化同步I/O控制块
	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = *ppos;	// 设置文件位置
	kiocb.ki_left = len;	// 设置剩余字节数
	kiocb.ki_nbytes = len;	// 设置总字节数

	for (;;) {
		ret = filp->f_op->aio_write(&kiocb, &iov, 1, kiocb.ki_pos);
		// 如果不需要重试则退出循环
		if (ret != -EIOCBRETRY)
			break;
		// 等待I/O操作可再次执行
		wait_on_retry_sync_kiocb(&kiocb);
	}

	// 如果操作已排队，等待完成
	if (-EIOCBQUEUED == ret)
		// 更新文件位置
		ret = wait_on_sync_kiocb(&kiocb);
	*ppos = kiocb.ki_pos;
	return ret;
}

EXPORT_SYMBOL(do_sync_write);

ssize_t vfs_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	// 检查文件是否具有写权限。
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	// 检查文件操作结构（file->f_op）是否存在以及相应的写方法是否有效。
	if (!file->f_op || (!file->f_op->write && !file->f_op->aio_write))
		return -EINVAL;
	// 验证用户空间的地址有效性。
	if (unlikely(!access_ok(VERIFY_READ, buf, count)))
		return -EFAULT;
	
	// 验证和调整写入操作的范围。
	ret = rw_verify_area(WRITE, file, pos, count);
	if (ret >= 0) {
		count = ret;
		if (file->f_op->write)
			// // 执行同步写
			ret = file->f_op->write(file, buf, count, pos);
		else
			// 执行同步写
			ret = do_sync_write(file, buf, count, pos);
		if (ret > 0) {
			// 通知文件系统发生了修改
			fsnotify_modify(file->f_path.dentry);
			// 增加写入字符计数
			add_wchar(current, ret);
		}
		// 增加系统调用写计数
		inc_syscw(current);
	}

	return ret;
}

EXPORT_SYMBOL(vfs_write);

// 定义内联函数用于读取文件的当前位置。
static inline loff_t file_pos_read(struct file *file)
{
	return file->f_pos;	// 返回文件位置字段。
}

// 定义内联函数用于设置文件的当前位置。
static inline void file_pos_write(struct file *file, loff_t pos)
{
	// 设置文件位置字段。
	file->f_pos = pos;
}

/*
 * 系统调用定义，接受三个参数：文件描述符、用户空间的缓冲区地址和要读取的字节数。
 */
SYSCALL_DEFINE3(read, unsigned int, fd, char __user *, buf, size_t, count)
{
	struct file *file;	// 文件结构指针，用于引用文件
	ssize_t ret = -EBADF;// 默认返回值设置为“无效的文件描述符”错误。
	int fput_needed;	// 用于跟踪文件是否需要释放的标记。

	/*
	 * 尝试获取与文件描述符关联的文件结构。fget_light 是一个轻量级的版本，
	 * 它不会完全增加文件的使用计数，而是返回一个标志（fput_needed）以指示是否需要后续减少引用。
	 */
	file = fget_light(fd, &fput_needed);
	if (file) {	// 如果文件描述符有效，即能获取到文件结构。
	// 读取文件的当前位置。
		loff_t pos = file_pos_read(file);
		/*
		 * 执行实际上的读操作。vfs_read 是 VFS 层提供的读函数，
		 * 它会调用相应文件系统的读方法。传入的参数包括文件结构、缓冲区、计数和位置。
		 */
		ret = vfs_read(file, buf, count, &pos);
		file_pos_write(file, pos);	// 更新文件的位置。
		fput_light(file, fput_needed);	// 根据 fput_needed 的值减少文件的引用计数。
	}

	return ret;
}

/*
 * 系统调用定义，接受三个参数：文件描述符、用户空间的缓冲区地址和要写入的字节数。
 */
SYSCALL_DEFINE3(write, unsigned int, fd, const char __user *, buf,
		size_t, count)
{
	// 文件结构指针，用于引用文件。
	struct file *file;
	// 默认返回值设置为“无效的文件描述符”错误。
	ssize_t ret = -EBADF;
	// 用于跟踪文件是否需要释放的标记。
	int fput_needed;

	/*
	 * 尝试获取与文件描述符关联的文件结构。fget_light 是一个轻量级的版本，
	 * 它不会完全增加文件的使用计数，而是返回一个标志（fput_needed）以指示是否需要后续减少引用。
	 */
	file = fget_light(fd, &fput_needed);
	if (file) {	// 如果文件描述符有效，即能获取到文件结构。
	// 读取文件的当前位置。
		loff_t pos = file_pos_read(file);
		/*
		 * 执行实际上的写操作。vfs_write 是 VFS 层提供的写函数，
		 * 它会调用相应文件系统的写方法。传入的参数包括文件结构、缓冲区、计数和位置。
		 */
		ret = vfs_write(file, buf, count, &pos);
		file_pos_write(file, pos);	// 更新文件的位置。
		// 根据 fput_needed 的值减少文件的引用计数。
		fput_light(file, fput_needed);
	}

	return ret;
}

/*
 * 系统调用：pread64
 * @fd: 文件描述符
 * @buf: 用户空间的缓冲区，用于存放读取的数据
 * @count: 要读取的字节数
 * @pos: 文件中的读取起始位置
 * 返回值：读取的字节数，或在出错时返回错误码
 *
 * pread64 允许从指定位置读取数据，而不改变文件描述符的当前偏移量。
 * 这对于多线程环境中的文件操作是特别有用的。
 */
SYSCALL_DEFINE(pread64)(unsigned int fd, char __user *buf,
                        size_t count, loff_t pos)
{
	struct file *file;      // 文件结构体指针
	ssize_t ret = -EBADF;   // 默认返回文件描述符无效错误
	int fput_needed;        // 标记是否需要释放文件引用

	// 检查传入的文件位置是否有效
	if (pos < 0)
		return -EINVAL;     // 如果位置无效，返回无效参数错误

	// 通过文件描述符获取文件结构体
	file = fget_light(fd, &fput_needed);
	if (file) {            // 如果文件结构体有效
		ret = -ESPIPE;     // 默认设置为错误码，表示非法寻址（通常对管道或FIFO文件）
		if (file->f_mode & FMODE_PREAD)  // 检查文件模式是否支持预读操作
			ret = vfs_read(file, buf, count, &pos);  // 调用 VFS 层的读取函数
		fput_light(file, fput_needed);  // 释放文件引用
	}

	return ret;  // 返回读取结果或错误码
}
/*
 * 如果内核配置启用了系统调用包装器，下面的定义将被包括进来。
 * 这些包装器和别名有助于在不同的硬件架构上保持系统调用接口的一致性。
 */
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
/*
 * asmlinkage long SyS_pread64(long fd, long buf, long count, loff_t pos)
 * 系统调用包装器：SyS_pread64
 * @fd: 文件描述符，传入类型为 long，内部转换为 unsigned int
 * @buf: 用户空间缓冲区地址，传入类型为 long，内部转换为 char __user *
 * @count: 要读取的字节数，传入类型为 long，内部转换为 size_t
 * @pos: 文件中的读取起始位置
 * 返回值：读取的字节数，或在出错时返回错误码
 *
 * 这个函数是 pread64 的系统调用包装器，它确保了在所有硬件架构上的调用一致性。
 * 它主要负责将传入的参数从 long 类型转换为适当的类型，并调用实际的系统调用实现函数。
 */
asmlinkage long SyS_pread64(long fd, long buf, long count, loff_t pos)
{
	// 调用实际的系统调用实现函数 SYSC_pread64，参数类型转换如下：
	// fd 转换为 unsigned int，buf 转换为 char __user *，count 转换为 size_t
	return SYSC_pread64((unsigned int) fd, (char __user *) buf,
			    (size_t) count, pos);
}
/*
 * SYSCALL_ALIAS(sys_pread64, SyS_pread64);
 * 系统调用别名：sys_pread64
 * 这一行定义了 sys_pread64 作为 SyS_pread64 的别名。
 * 这使得 sys_pread64 和 SyS_pread64 可以互换使用，进一步增强了接口的一致性。
 */
SYSCALL_ALIAS(sys_pread64, SyS_pread64);
#endif

/*
 * SYSCALL_DEFINE(pwrite64)(unsigned int fd, const char __user *buf,
 *                          size_t count, loff_t pos)
 * 系统调用：pwrite64
 * @fd: 文件描述符
 * @buf: 指向用户空间的数据缓冲区
 * @count: 要写入的字节数
 * @pos: 文件中的写入起始位置
 * 返回值：写入的字节数，或在出错时返回错误码
 *
 * pwrite64 允许从指定位置写入数据，而不改变文件描述符的当前偏移量。
 * 这对于多线程环境中的文件操作是特别有用的。
 */
SYSCALL_DEFINE(pwrite64)(unsigned int fd, const char __user *buf,
                         size_t count, loff_t pos)
{
	struct file *file;      // 文件结构体指针
	ssize_t ret = -EBADF;   // 默认返回文件描述符无效错误
	int fput_needed;        // 标记是否需要释放文件引用

	if (pos < 0)
		return -EINVAL;     // 如果位置无效，返回无效参数错误

	file = fget_light(fd, &fput_needed);  // 轻量级获取文件指针
	if (file) {
		ret = -ESPIPE;     // 默认设置为错误码，表示非法寻址（通常对管道或FIFO文件）
		if (file->f_mode & FMODE_PWRITE)  // 检查文件模式是否支持预写操作
			ret = vfs_write(file, buf, count, &pos);  // 调用 VFS 层的写入函数
		fput_light(file, fput_needed);  // 释放文件引用
	}

	return ret;  // 返回写入结果或错误码
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
/*
 * asmlinkage long SyS_pwrite64(long fd, long buf, long count, loff_t pos)
 * 系统调用包装器：SyS_pwrite64
 * @fd: 文件描述符，传入类型为 long，内部转换为 unsigned int
 * @buf: 用户空间缓冲区地址，传入类型为 long，内部转换为 const char __user *
 * @count: 要写入的字节数，传入类型为 long，内部转换为 size_t
 * @pos: 文件中的写入起始位置
 * 返回值：写入的字节数，或在出错时返回错误码
 *
 * 这个函数是 pwrite64 的系统调用包装器，它确保了在所有硬件架构上的调用一致性。
 * 它主要负责将传入的参数从 long 类型转换为适当的类型，并调用实际的系统调用实现函数。
 */
asmlinkage long SyS_pwrite64(long fd, long buf, long count, loff_t pos)
{
	return SYSC_pwrite64((unsigned int) fd, (const char __user *) buf,
			     (size_t) count, pos);
}

/*
 * SYSCALL_ALIAS(sys_pwrite64, SyS_pwrite64);
 * 系统调用别名：sys_pwrite64
 * 这一行定义了 sys_pwrite64 作为 SyS_pwrite64 的别名。
 * 这使得 sys_pwrite64 和 SyS_pwrite64 可以互换使用，进一步增强了接口的一致性。
 */
SYSCALL_ALIAS(sys_pwrite64, SyS_pwrite64);
#endif

/*
 * Reduce an iovec's length in-place.  Return the resulting number of segments
 */
/*
 * 函数说明：在原地缩短 iovec 的长度。返回结果中的段数。
 * @iov: 指向 iovec 数组的指针
 * @nr_segs: iovec 数组的段数
 * @to: 需要缩减到的总长度
 * 返回值：调整后的 iovec 数组中的段数
 *
 * 这个函数缩短 iovec 结构的长度，以使整个 iovec 的总长度不超过指定的 'to' 值。
 * 它逐个遍历 iovec 结构，逐段调整长度，一旦累计长度达到或超过 'to'，则停止处理。
 */
unsigned long iov_shorten(struct iovec *iov, unsigned long nr_segs, size_t to)
{
	unsigned long seg = 0;  // 用于计数处理过的 iovec 段
	size_t len = 0;         // 累计长度

	while (seg < nr_segs) {  // 遍历每个 iovec 段
		seg++;               // 处理的段数递增
		if (len + iov->iov_len >= to) {  // 如果当前段加上累计长度超过或等于目标长度
			iov->iov_len = to - len;  // 调整当前 iovec 的长度
			break;  // 达到目标长度，停止处理
		}
		len += iov->iov_len;  // 累加当前 iovec 的长度
		iov++;               // 移动到下一个 iovec 结构
	}
	return seg;  // 返回处理过的 iovec 段数
}
EXPORT_SYMBOL(iov_shorten);

/*
 * do_sync_readv_writev - 执行同步的向量读写操作
 * @filp: 文件指针
 * @iov: 指向 iovec 数组的指针，iovec 数组包含数据缓冲区信息
 * @nr_segs: iovec 数组的段数
 * @len: 要读写的总数据长度
 * @ppos: 指向文件中起始偏移量的指针
 * @fn: 执行实际读写操作的函数指针
 * 返回值：读写操作的结果，为读写的字节数或错误码
 *
 * 此函数通过调用指定的读写函数来进行操作，处理可能出现的重试和排队情况，并在操作完成后更新文件位置。
 */
ssize_t do_sync_readv_writev(struct file *filp, const struct iovec *iov,
                             unsigned long nr_segs, size_t len, loff_t *ppos, iov_fn_t fn)
{
	struct kiocb kiocb;  // 异步 I/O 控制块
	ssize_t ret;         // 函数返回值

	init_sync_kiocb(&kiocb, filp);  // 初始化同步的 I/O 控制块
	kiocb.ki_pos = *ppos;           // 设置 I/O 的起始位置
	kiocb.ki_left = len;            // 设置剩余字节数
	kiocb.ki_nbytes = len;          // 设置总字节数

	for (;;) {  // 无限循环，直到读写完成或不需要重试
		ret = fn(&kiocb, iov, nr_segs, kiocb.ki_pos);  // 调用传入的函数进行读写
		if (ret != -EIOCBRETRY)  // 如果结果不是重试的错误码
			break;               // 退出循环
		wait_on_retry_sync_kiocb(&kiocb);  // 等待重试条件满足
	}

	if (ret == -EIOCBQUEUED)  // 如果结果表明 I/O 操作已入队等待处理
		ret = wait_on_sync_kiocb(&kiocb);  // 等待 I/O 操作完成
	*ppos = kiocb.ki_pos;  // 更新文件位置
	return ret;  // 返回操作结果
}

/* Do it by hand, with file-ops */
/* 手动执行，使用文件操作 */
/*
 * do_loop_readv_writev - 通过文件操作手动执行循环的读写向量操作
 * @filp: 文件指针
 * @iov: 指向 iovec 数组的指针，iovec 数组包含要读写的数据缓冲区信息
 * @nr_segs: iovec 数组的段数
 * @ppos: 指向文件中起始偏移量的指针
 * @fn: 执行实际读写操作的函数指针
 * 返回值：读写操作的总字节数，如果有错误发生则返回错误码
 *
 * 此函数通过手动方式循环处理每个 iovec 结构，并调用提供的函数指针进行实际的读写操作。
 */
ssize_t do_loop_readv_writev(struct file *filp, struct iovec *iov,
		unsigned long nr_segs, loff_t *ppos, io_fn_t fn)
{
	struct iovec *vector = iov;  // 当前处理的 iovec 指针
	ssize_t ret = 0;  // 累计读写字节数，初始为 0

	while (nr_segs > 0) {  // 当还有未处理的 iovec 时循环
		void __user *base;  // 用户空间数据缓冲区地址
		size_t len;         // 数据缓冲区长度
		ssize_t nr;         // 单次操作返回的结果

		base = vector->iov_base;  // 获取当前 iovec 的数据缓冲区地址
		len = vector->iov_len;    // 获取当前 iovec 的数据缓冲区长度
		vector++;  // 移动到下一个 iovec
		nr_segs--;  // 处理完一个段后减少段数

		nr = fn(filp, base, len, ppos);  // 调用函数指针进行读写操作

		if (nr < 0) {  // 如果读写操作返回错误
			if (!ret)
				ret = nr;  // 如果之前没有读写成功，则返回错误
			break;  // 终止循环
		}
		ret += nr;  // 累加成功读写的字节数
		if (nr != len)
			break;  // 如果本次未完全读写指定长度，终止循环
	}

	return ret;  // 返回累计读写的总字节数或错误码
}

/* A write operation does a read from user space and vice versa */
/* 写操作从用户空间读取数据，反之亦然 */
#define vrfy_dir(type) ((type) == READ ? VERIFY_WRITE : VERIFY_READ)

ssize_t rw_copy_check_uvector(int type, const struct iovec __user * uvector,
			      unsigned long nr_segs, unsigned long fast_segs,
			      struct iovec *fast_pointer,
			      struct iovec **ret_pointer)
{
	unsigned long seg;  // 用于循环的段索引
	ssize_t ret;  // 函数的返回值
	struct iovec *iov = fast_pointer;  // 指向 iovec 数组的指针，初始化为快速指针数组

  	/*
  	 * SuS says "The readv() function *may* fail if the iovcnt argument
  	 * was less than or equal to 0, or greater than {IOV_MAX}.  Linux has
  	 * traditionally returned zero for zero segments, so...
  	 */
		/*
		 * SuS 规范说明 "readv() 函数可能会失败，如果 iovcnt 参数小于或等于 0，
		 * 或者大于 {IOV_MAX}。Linux 传统上对于零段返回零，所以..."
		 */
	if (nr_segs == 0) {  // 检查段数是否为零
		ret = 0;  // 对于零段，按传统返回 0
			goto out;  // 直接跳转到出口标签
	}

  	/*
  	 * First get the "struct iovec" from user memory and
  	 * verify all the pointers
  	 */
		/*
		 * 首先从用户内存获取 "struct iovec" 并验证所有指针
		 */
	if (nr_segs > UIO_MAXIOV) {	// 如果段数超过了最大允许值
		ret = -EINVAL;	// 如果段数超过最大值，返回无效参数错误
  		goto out;	// 跳转到出口标签
	}
	if (nr_segs > fast_segs) {	// 如果段数超过快速指针数组的大小
  		iov = kmalloc(nr_segs*sizeof(struct iovec), GFP_KERNEL);	// 如果段数超过快速指针数组大小，动态分配内存
		if (iov == NULL) {	// 检查内存是否成功分配
			ret = -ENOMEM;	// 如果内存分配失败，返回内存不足错误
  			goto out;			// 跳转到出口标签
		}
  	}
	if (copy_from_user(iov, uvector, nr_segs*sizeof(*uvector))) {	// 从用户空间复制数据
		ret = -EFAULT;	// 从用户空间复制 iovec 失败，返回错误
  		goto out;			// 跳转到出口标签
	}

  /*
	 * According to the Single Unix Specification we should return EINVAL
	 * if an element length is < 0 when cast to ssize_t or if the
	 * total length would overflow the ssize_t return value of the
	 * system call.
   */
	/*
	 * 根据单一 Unix 规范，如果元素长度转换为 ssize_t 后小于 0，或者
	 * 总长度将溢出系统调用的 ssize_t 返回值，我们应该返回 EINVAL。
	 */
	ret = 0;	// 初始化累计长度为0
  	for (seg = 0; seg < nr_segs; seg++) {	// 遍历所有 iovec 段
  		void __user *buf = iov[seg].iov_base;	// 获取当前段的用户空间缓冲区地址
  		ssize_t len = (ssize_t)iov[seg].iov_len;	// 获取当前段的长度，并转换为 ssize_t

		/* see if we we're about to use an invalid len or if
		 * it's about to overflow ssize_t */
		/* 查看是否即将使用无效的 len 或是否将溢出 ssize_t */
		if (len < 0 || (ret + len < ret)) {	// 检查长度是否为负或累加后溢出
			ret = -EINVAL;	// 如果长度无效或溢出，返回无效参数错误
  			goto out;			// 跳转到出口标签处理
		}
		if (unlikely(!access_ok(vrfy_dir(type), buf, len))) {	// 检查内存访问权限
			ret = -EFAULT;	// 检查内存访问权限，如果不通过，返回错误
  			goto out;			// 跳转到出口标签处理
		}

		ret += len;	// 累加长度
  	}
out:
	*ret_pointer = iov;	// 设置返回的 iov 指针
	return ret;		// 返回累计长度或错误码
}

// 用于处理读或写多个内存区域（通过 iovec 指定）的底层文件操作函数
static ssize_t do_readv_writev(int type, struct file *file,
                               const struct iovec __user * uvector,
                               unsigned long nr_segs, loff_t *pos)
{
	size_t tot_len;  // 总长度变量
	struct iovec iovstack[UIO_FASTIOV];  // 栈上的 iovec 数组
	struct iovec *iov = iovstack;  // 指向 iovec 的指针，初始化为指向栈上数组
	ssize_t ret;  // 函数的返回值
	io_fn_t fn;  // 指向相应的读或写函数的指针
	iov_fn_t fnv;  // 指向异步读或写函数的指针

	if (!file->f_op) {
		ret = -EINVAL;  // 如果文件操作指针为空，返回无效参数错误
		goto out;
	}

	// 从用户空间复制并检查 iovec 结构数组
	ret = rw_copy_check_uvector(type, uvector, nr_segs,
				ARRAY_SIZE(iovstack), iovstack, &iov);
	if (ret <= 0)  // 如果复制或检查失败（或无数据），直接跳转到出口
		goto out;

	tot_len = ret;  // 获取总长度
	// 验证读写区域
	ret = rw_verify_area(type, file, pos, tot_len);
	if (ret < 0)  // 如果验证失败，跳转到出口
		goto out;

	fnv = NULL;
	if (type == READ) {
		fn = file->f_op->read;  // 获取文件的读函数
		fnv = file->f_op->aio_read;  // 获取文件的异步读函数
	} else {
		fn = (io_fn_t)file->f_op->write;  // 获取文件的写函数
		fnv = file->f_op->aio_write;  // 获取文件的异步写函数
	}

	// 根据是否支持异步操作，调用相应的读写函数
	if (fnv)
		ret = do_sync_readv_writev(file, iov, nr_segs, tot_len, pos, fnv);
	else
		ret = do_loop_readv_writev(file, iov, nr_segs, pos, fn);

out:
	if (iov != iovstack)  // 如果 iov 指针不指向栈上数组，说明使用了动态内存
		kfree(iov);  // 释放动态分配的内存
	// 如果操作成功，并根据操作类型，通知文件系统相关事件
	if ((ret + (type == READ)) > 0) {
		if (type == READ)
			fsnotify_access(file->f_path.dentry);  // 读访问通知
		else
			fsnotify_modify(file->f_path.dentry);  // 写修改通知
	}
	return ret;  // 返回操作结果
}

/* 参数说明：
 * READ: 指示这是一个读操作
 * file: 文件结构体指针，包含文件的操作信息
 * vec: 指向用户空间的 iovec 结构数组的指针，指定读取数据的目标缓冲区
 * vlen: iovec 结构的数量，指定 vec 中有多少个内存区域需要读取数据
 * pos: 指向一个 off_t 类型的变量，表示从文件中的何处开始读取数据
 */
// Linux VFS (Virtual File System) 层用于处理向量读取操作的函数。此函数封装了对文件的读取操作，使用 iovec 结构数组来指定多个内存区域，以从指定的文件位置读取数据到这些区域
ssize_t vfs_readv(struct file *file, const struct iovec __user *vec,
                  unsigned long vlen, loff_t *pos)
{
	// 检查文件是否开启了读取模式
	if (!(file->f_mode & FMODE_READ))
		return -EBADF;  // 如果没有开启读取模式，返回错误码 -EBADF (Bad file descriptor)

	// 检查文件操作指针是否为空，或者文件是否缺少读取操作的实现
	if (!file->f_op || (!file->f_op->aio_read && !file->f_op->read))
		return -EINVAL;  // 如果缺少必要的读操作，返回错误码 -EINVAL (Invalid argument)

	// 调用 do_readv_writev 函数执行实际的读操作
	return do_readv_writev(READ, file, vec, vlen, pos);
}

EXPORT_SYMBOL(vfs_readv);


/* 参数说明：
 * WRITE: 指示这是一个写操作
 * file: 文件结构体指针，包含文件的操作信息
 * vec: 指向用户空间的 iovec 结构数组的指针，指定源数据缓冲区
 * vlen: iovec 结构的数量，指定 vec 中有多少个内存区域将数据写入文件
 * pos: 指向一个 loff_t 类型的变量，表示从文件中的何处开始写入数据
 */
// 虚拟文件系统层（VFS）提供的接口，用于处理向量写操作。这个函数通过接收一组 iovec 结构来指定多个内存区域，并将这些区域的数据写入到文件中指定的位置。
ssize_t vfs_writev(struct file *file, const struct iovec __user *vec,
                   unsigned long vlen, loff_t *pos)
{
	// 检查文件模式是否允许写操作
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;  // 如果文件不允许写操作，返回错误码 -EBADF (Bad file descriptor)

	// 检查文件操作结构是否存在，以及是否提供了写操作函数
	if (!file->f_op || (!file->f_op->aio_write && !file->f_op->write))
		return -EINVAL;  // 如果没有写操作函数，返回错误码 -EINVAL (Invalid argument)

	// 调用 do_readv_writev 函数进行实际的写操作
	return do_readv_writev(WRITE, file, vec, vlen, pos);
}
EXPORT_SYMBOL(vfs_writev);

// 该调用从文件描述符指定的文件中读取数据，数据被读入一个或多个缓冲区，这些缓冲区由 iovec 结构数组指定。
SYSCALL_DEFINE3(readv, unsigned long, fd, const struct iovec __user *, vec,
                unsigned long, vlen)
{
	struct file *file;  // 文件指针
	ssize_t ret = -EBADF;  // 默认返回错误码 -EBADF，表示无效的文件描述符
	int fput_needed;  // 用于标记是否需要释放文件引用

	// 尝试获取文件描述符对应的文件结构
	file = fget_light(fd, &fput_needed);
	if (file) {  // 如果获取文件结构成功
		loff_t pos = file_pos_read(file);  // 读取当前文件的位置
		ret = vfs_readv(file, vec, vlen, &pos);  // 执行向量读操作
		file_pos_write(file, pos);  // 更新文件位置
		fput_light(file, fput_needed);  // 释放文件引用
	}

	// 如果读取操作成功且读取的字节数大于0
	if (ret > 0)
		add_rchar(current, ret);  // 增加当前进程的读取字节统计
	inc_syscr(current);  // 增加当前进程的系统调用计数
	return ret;  // 返回读取操作的结果
}

// 用于将数据从多个内存区域（由 iovec 结构数组指定）写入到一个指定的文件描述符关联的文件中
SYSCALL_DEFINE3(writev, unsigned long, fd, const struct iovec __user *, vec,
                unsigned long, vlen)
{
	struct file *file;  // 文件结构指针
	ssize_t ret = -EBADF;  // 初始化返回值为 -EBADF，表示文件描述符无效
	int fput_needed;  // 用于标记是否需要释放文件引用

	// 尝试根据文件描述符获取文件结构体
	file = fget_light(fd, &fput_needed);
	if (file) {  // 如果文件结构体获取成功
		loff_t pos = file_pos_read(file);  // 读取当前的文件位置
		ret = vfs_writev(file, vec, vlen, &pos);  // 执行向量写操作
		file_pos_write(file, pos);  // 更新文件位置
		fput_light(file, fput_needed);  // 释放文件引用
	}

	// 如果写操作成功，并且写入的字节数大于0
	if (ret > 0)
		add_wchar(current, ret);  // 增加当前进程的写入字节统计
	inc_syscw(current);  // 增加当前进程的系统调用写计数
	return ret;  // 返回写操作的结果
}

// 用于计算64位偏移量的辅助函数 pos_from_hil
static inline loff_t pos_from_hilo(unsigned long high, unsigned long low)
{
#define HALF_LONG_BITS (BITS_PER_LONG / 2)	// 定义一个半长位数，用于位移操作
	// 首先将 high 左移半长位数再左移半长位数（总共BITS_PER_LONG位数），然后与 low 进行按位或操作以形成完整的64位偏移量
	return (((loff_t)high << HALF_LONG_BITS) << HALF_LONG_BITS) | low;
}

// 用于从指定的文件描述符读取数据到一个或多个用户空间缓冲区，位置由两个32位整数组合成一个64位整数指定
SYSCALL_DEFINE5(preadv, unsigned long, fd, const struct iovec __user *, vec,
                unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h)
{
	loff_t pos = pos_from_hilo(pos_h, pos_l);  // 使用 pos_from_hilo 计算从高低位组合的完整64位偏移量
	struct file *file;  // 文件结构体指针
	ssize_t ret = -EBADF;  // 默认返回错误码 -EBADF，表示无效的文件描述符
	int fput_needed;  // 用于标记是否需要释放文件引用

	if (pos < 0)  // 检查计算出的偏移量是否为负
		return -EINVAL;  // 返回错误码 -EINVAL，表示无效的参数

	file = fget_light(fd, &fput_needed);  // 尝试获取文件描述符对应的文件结构
	if (file) {  // 如果文件获取成功
		ret = -ESPIPE;  // 默认设置为错误码 -ESPIPE，表示非法的寻址操作
		if (file->f_mode & FMODE_PREAD)  // 检查文件模式是否支持预定位读操作
			ret = vfs_readv(file, vec, vlen, &pos);  // 执行向量读操作
		fput_light(file, fput_needed);  // 释放文件引用
	}

	if (ret > 0)  // 如果读取操作成功，并且读取的字节数大于0
		add_rchar(current, ret);  // 增加当前进程的读取字节统计
	inc_syscr(current);  // 增加当前进程的系统调用计数
	return ret;  // 返回读取操作的结果
}

// 将来自用户空间的多个缓冲区的数据写入到文件的指定位置，而不改变文件的当前偏移量。这个系统调用特别适用于需要高效管理大文件或者多线程环境中的文件写入操作。
SYSCALL_DEFINE5(pwritev, unsigned long, fd, const struct iovec __user *, vec,
                unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h)
{
	// 使用两个32位整数 pos_h (高位) 和 pos_l (低位) 组合成一个 64 位偏移量。
	loff_t pos = pos_from_hilo(pos_h, pos_l);  
	struct file *file;  // 文件结构体指针
	ssize_t ret = -EBADF;  // 初始化返回值为 -EBADF，表示文件描述符无效
	int fput_needed;  // 用于标记是否需要释放文件引用

	// 检查计算出的偏移量是否为负，如果为负则返回无效参数错误
	if (pos < 0)
		return -EINVAL;

	// 尝试获取文件描述符对应的文件结构体
	file = fget_light(fd, &fput_needed);
	if (file) {  // 如果成功获取文件结构
		ret = -ESPIPE;  // 默认设置为错误码 -ESPIPE，表示非法的寻址操作
		// 检查文件模式是否支持预定位写操作
		if (file->f_mode & FMODE_PWRITE)
			ret = vfs_writev(file, vec, vlen, &pos);  // 执行向量写操作
		fput_light(file, fput_needed);  // 释放文件引用
	}

	// 如果写操作成功，并且写入的字节数大于0
	if (ret > 0)
		add_wchar(current, ret);  // 增加当前进程的写入字节统计
	inc_syscw(current);  // 增加当前进程的系统调用写计数
	return ret;  // 返回写操作的结果
}

// 用于在 Linux 内核中实现 sendfile 功能，允许将数据从一个文件直接传输到另一个文件，常用于在网络和文件系统之间高效传输数据。
static ssize_t do_sendfile(int out_fd, int in_fd, loff_t *ppos,
			   size_t count, loff_t max)
{
	struct file * in_file, * out_file; // 定义指向输入文件和输出文件的指针
	struct inode * in_inode, * out_inode; // 定义指向输入文件和输出文件的 inode 结构的指针
	loff_t pos; // 用于存储文件位置的变量
	ssize_t retval; // 函数返回值，用于错误处理和返回最终的传输字节数
	int fput_needed_in, fput_needed_out, fl; // 标记是否需要释放文件指针

	/*
	 * Get input file, and verify that it is ok..
	 */
	/*
	 * 获取输入文件，并验证其是否有效..
   */
	retval = -EBADF;	// 默认返回错误码 -EBADF，表示无效的文件描述符
	in_file = fget_light(in_fd, &fput_needed_in);	// 获取输入文件
	if (!in_file)	// 如果输入文件为空，即文件描述符无效
		goto out;		// 跳转至函数出口，返回错误
	if (!(in_file->f_mode & FMODE_READ))	// 检查输入文件是否可读
		goto fput_in;	// 如果没有读权限，跳转至释放输入文件引用的标签
	retval = -ESPIPE;	// 如果文件位置指针为空或文件不支持预定位读，设置错误码
	if (!ppos)	// 如果传入的文件位置指针为空
		ppos = &in_file->f_pos;	// 如果未指定位置，则使用文件的当前位置
	else if (!(in_file->f_mode & FMODE_PREAD))	// 检查文件是否支持预定位读操作
		goto fput_in;	 // 如果不支持，跳转至释放输入文件引用的标签
	retval = rw_verify_area(READ, in_file, ppos, count);	// 验证读操作的安全性，确保访问的内存区域有效
	if (retval < 0)	// 如果验证失败（返回负值）
		goto fput_in;	// 跳转至释放输入文件引用的标签
	count = retval;	// 验证成功，更新计划读取的字节数

	/*
	 * Get output file, and verify that it is ok..
	 */
	/*
   * 获取输出文件，并验证其是否有效..
   */
	retval = -EBADF;	// 如果无法获取文件，则默认返回 -EBADF，表示文件描述符无效
	out_file = fget_light(out_fd, &fput_needed_out);	// 获取输出文件
	if (!out_file)
		goto fput_in;	// 如果获取输出文件失败，则跳转到释放输入文件的标签
	if (!(out_file->f_mode & FMODE_WRITE))
		goto fput_out;	// 检查输出文件是否有写权限，没有则跳转到释放输出文件的标签
	retval = -EINVAL;	// 设置默认错误为 -EINVAL，表示无效的参数
	in_inode = in_file->f_path.dentry->d_inode;	// 获取输入文件的 inode
	out_inode = out_file->f_path.dentry->d_inode;	// 获取输出文件的 inode
	retval = rw_verify_area(WRITE, out_file, &out_file->f_pos, count);	// 验证输出文件的写操作是否合法
	if (retval < 0)
		goto fput_out;	// 如果验证失败，跳转到释放输出文件的标签
	count = retval;		// 更新应传输的字节数

	if (!max)
		// 如果未设置最大字节数限制，取输入和输出文件允许的最大字节数的最小值
		max = min(in_inode->i_sb->s_maxbytes, out_inode->i_sb->s_maxbytes);

	pos = *ppos;	// 获取当前的文件偏移位置
	if (unlikely(pos + count > max)) {
		retval = -EOVERFLOW;	// 如果传输的结束位置超出了最大限制，设置错误为 -EOVERFLOW
		if (pos >= max)
			goto fput_out;	// 如果起始位置就已经超过最大限制，跳转到释放输出文件的标签
		count = max - pos;	// 调整传输的字节数，确保不超过最大限制
	}

	fl = 0;	// 默认不使用非阻塞标志
#if 0
	/*
	 * We need to debate whether we can enable this or not. The
	 * man page documents EAGAIN return for the output at least,
	 * and the application is arguably buggy if it doesn't expect
	 * EAGAIN on a non-blocking file descriptor.
	 */
	/*
	 * 我们需要讨论是否可以启用这个功能。至少在手册页中记录了对输出返回 EAGAIN，
   * 如果应用程序不期望在非阻塞文件描述符上得到 EAGAIN，这可能是一个错误。
	 */
	// 如果输入文件是非阻塞的，设置 SPLICE_F_NONBLOCK 标志
	if (in_file->f_flags & O_NONBLOCK)
		fl = SPLICE_F_NONBLOCK;	// 讨论是否启用非阻塞传输
#endif
	// 直接进行内核级别的数据传输，避免将数据复制到用户空间
	retval = do_splice_direct(in_file, ppos, out_file, count, fl);	// 直接执行数据传输

	if (retval > 0) {
		add_rchar(current, retval);	// 更新当前进程的读字节计数
		add_wchar(current, retval);	// 更新当前进程的写字节计数
	}

	inc_syscr(current);	// 增加系统调用读计数
	inc_syscw(current);	// 增加系统调用写计数
	if (*ppos > max)
		retval = -EOVERFLOW;	// 如果当前位置超出最大限制，设置返回值为 -EOVERFLOW

fput_out:
	fput_light(out_file, fput_needed_out);	// 释放输出文件引用
fput_in:
	fput_light(in_file, fput_needed_in);		// 释放输入文件引用
out:
	return retval;		// 返回操作结果
}

// 用于在文件之间高效传输数据的系统调用，特别是从一个文件（通常是一个打开的文件描述符）到另一个文件（通常是一个网络连接的文件描述符）。
SYSCALL_DEFINE4(sendfile, int, out_fd, int, in_fd, off_t __user *, offset, size_t, count)
{
	loff_t pos;  // 用于64位的文件偏移
	off_t off;  // 用户空间提供的偏移量（可能是32位或64位，依系统而定）
	ssize_t ret;  // 函数返回值

	if (offset) {  // 如果提供了偏移量指针
		if (unlikely(get_user(off, offset)))  // 从用户空间获取偏移量
			return -EFAULT;  // 如果获取失败，返回错误
		pos = off;  // 将用户空间的偏移量转换为内核使用的 loff_t 类型
		ret = do_sendfile(out_fd, in_fd, &pos, count, MAX_NON_LFS);  // 调用 do_sendfile 进行文件传输
		if (unlikely(put_user(pos, offset)))  // 将更新后的偏移量写回用户空间
			return -EFAULT;  // 如果写回失败，返回错误
		return ret;  // 返回文件传输的结果
	}

	return do_sendfile(out_fd, in_fd, NULL, count, 0);  // 如果没有提供偏移量，直接调用 do_sendfile
}

// 允许在两个文件描述符之间传输数据，并且特别支持64位的文件偏移量，适用于处理大文件。
SYSCALL_DEFINE4(sendfile64, int, out_fd, int, in_fd, loff_t __user *, offset, size_t, count)
{
	loff_t pos;  // 定义64位的文件偏移量变量
	ssize_t ret;  // 函数的返回值，用于存储发送文件的结果或错误代码

	if (offset) {  // 如果提供了偏移量的地址
		// 从用户空间拷贝偏移量到内核空间
		if (unlikely(copy_from_user(&pos, offset, sizeof(loff_t))))
			return -EFAULT;  // 如果拷贝失败，返回错误代码 -EFAULT
		// 执行文件发送操作，指定文件偏移
		ret = do_sendfile(out_fd, in_fd, &pos, count, 0);
		// 将更新后的偏移量写回用户空间
		if (unlikely(put_user(pos, offset)))
			return -EFAULT;  // 如果写回失败，返回错误代码 -EFAULT
		return ret;  // 返回文件发送的结果
	}

	// 如果没有提供偏移量，执行文件发送操作，不指定偏移量
	return do_sendfile(out_fd, in_fd, NULL, count, 0);
}
