/*
 * fs/sysfs/file.c - sysfs regular (text) file implementation
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 *
 * This file is released under the GPLv2.
 *
 * Please see Documentation/filesystems/sysfs.txt for more information.
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/kallsyms.h>
#include <linux/slab.h>
#include <linux/fsnotify.h>
#include <linux/namei.h>
#include <linux/poll.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/limits.h>
#include <asm/uaccess.h>

#include "sysfs.h"

/* used in crash dumps to help with debugging */
// 在崩溃转储中使用，帮助调试
static char last_sysfs_file[PATH_MAX];  // 定义一个静态字符数组用于存储最后一个 sysfs 文件的路径
// 打印最后一个访问的 sysfs 文件的路径
void sysfs_printk_last_file(void)
{
	printk(KERN_EMERG "last sysfs file: %s\n", last_sysfs_file);  // 使用 printk 函数打印紧急日志信息
}

/*
 * There's one sysfs_buffer for each open file and one
 * sysfs_open_dirent for each sysfs_dirent with one or more open
 * files.
 *
 * filp->private_data points to sysfs_buffer and
 * sysfs_dirent->s_attr.open points to sysfs_open_dirent.  s_attr.open
 * is protected by sysfs_open_dirent_lock.
 */
/*
 * 对于每个打开的文件，都有一个 sysfs_buffer；对于每个有一个或多个打开文件的 sysfs_dirent，
 * 都有一个 sysfs_open_dirent。
 *
 * filp->private_data 指向 sysfs_buffer，
 * sysfs_dirent->s_attr.open 指向 sysfs_open_dirent。s_attr.open
 * 受 sysfs_open_dirent_lock 保护。
 */
static DEFINE_SPINLOCK(sysfs_open_dirent_lock); // 定义一个自旋锁，用于保护 sysfs_open_dirent 的访问

// 用于每个打开的 sysfs 目录项的结构
struct sysfs_open_dirent {
	atomic_t        refcnt;     // 引用计数
	atomic_t        event;      // 事件计数器
	wait_queue_head_t poll;     // 轮询等待队列
	/* goes through sysfs_buffer.list */
	struct list_head buffers;   // 通过 sysfs_buffer.list 链接的缓冲区列表
};

// 用于 sysfs 文件读写操作的缓冲区结构
struct sysfs_buffer {
	size_t          count;      // 缓冲区中数据的字节数
	loff_t          pos;        // 当前读写位置
	char            *page;      // 指向缓冲区页的指针
	const struct sysfs_ops *ops; // 指向 sysfs 操作的指针
	struct mutex    mutex;      // 用于同步访问的互斥锁
	int             needs_read_fill; // 标记是否需要填充读缓冲区
	int             event;      // 相关事件的计数器
	struct list_head list;      // 链接到 sysfs_open_dirent 的 buffers 列表
};

/**
 *	fill_read_buffer - allocate and fill buffer from object.
 *	@dentry:	dentry pointer.
 *	@buffer:	data buffer for file.
 *
 *	Allocate @buffer->page, if it hasn't been already, then call the
 *	kobject's show() method to fill the buffer with this attribute's 
 *	data. 
 *	This is called only once, on the file's first read unless an error
 *	is returned.
 */
/**
 * fill_read_buffer - 从对象分配并填充缓冲区。
 * @dentry: 目录项指针。
 * @buffer: 文件的数据缓冲区。
 *
 * 如果尚未分配 @buffer->page，则分配它，然后调用 kobject 的 show() 方法
 * 将此属性的数据填充到缓冲区中。
 * 除非返回错误，否则这只在文件的第一次读取时调用一次。
 */
static int fill_read_buffer(struct dentry *dentry, struct sysfs_buffer *buffer)
{
	struct sysfs_dirent *attr_sd = dentry->d_fsdata;  // 获取目录项的文件系统数据
	struct kobject *kobj = attr_sd->s_parent->s_dir.kobj;  // 获取 kobject 对象
	const struct sysfs_ops *ops = buffer->ops;  // 获取操作结构
	int ret = 0;
	ssize_t count;

	if (!buffer->page)
		buffer->page = (char *) get_zeroed_page(GFP_KERNEL);  // 分配一个初始化为零的页面
	if (!buffer->page)
		return -ENOMEM;  // 如果分配失败，返回内存不足错误

	/* need attr_sd for attr and ops, its parent for kobj */
	// 获取 attr_sd 以用于 attr 和 ops，它的 parent 用于 kobj
	if (!sysfs_get_active(attr_sd))
		return -ENODEV;  // 如果不能激活 sysfs_dirent，返回设备不存在错误

	buffer->event = atomic_read(&attr_sd->s_attr.open->event);  // 读取事件计数器
	count = ops->show(kobj, attr_sd->s_attr.attr, buffer->page);  // 调用 show 方法填充页面

	sysfs_put_active(attr_sd);  // 释放活跃引用

	/*
	 * The code works fine with PAGE_SIZE return but it's likely to
	 * indicate truncated result or overflow in normal use cases.
	 */
	// 代码处理 PAGE_SIZE 返回的情况正常，但通常来说返回 PAGE_SIZE 可能表示结果被截断或溢出
	if (count >= (ssize_t)PAGE_SIZE) {
		print_symbol("fill_read_buffer: %s returned bad count\n",
				(unsigned long)ops->show);  // 如果返回的计数异常，打印警告
		/* Try to struggle along */
		// 尝试继续处理
		count = PAGE_SIZE - 1;
	}
	if (count >= 0) {
		buffer->needs_read_fill = 0;  // 标记不需要重新填充读取
		buffer->count = count;  // 设置缓冲区计数
	} else {
		ret = count;  // 设置返回值为错误代码
	}
	return ret;
}

/**
 *	sysfs_read_file - read an attribute. 
 *	@file:	file pointer.
 *	@buf:	buffer to fill.
 *	@count:	number of bytes to read.
 *	@ppos:	starting offset in file.
 *
 *	Userspace wants to read an attribute file. The attribute descriptor
 *	is in the file's ->d_fsdata. The target object is in the directory's
 *	->d_fsdata.
 *
 *	We call fill_read_buffer() to allocate and fill the buffer from the
 *	object's show() method exactly once (if the read is happening from
 *	the beginning of the file). That should fill the entire buffer with
 *	all the data the object has to offer for that attribute.
 *	We then call flush_read_buffer() to copy the buffer to userspace
 *	in the increments specified.
 */
/**
 * sysfs_read_file - 读取一个属性。
 * @file: 文件指针。
 * @buf: 填充数据的缓冲区。
 * @count: 要读取的字节数。
 * @ppos: 文件中的起始偏移量。
 *
 * 用户空间想要读取一个属性文件。属性描述符在文件的 ->d_fsdata 中。
 * 目标对象在目录的 ->d_fsdata 中。
 *
 * 我们调用 fill_read_buffer() 一次性从对象的 show() 方法中分配并填充缓冲区
 * （如果从文件开头读取）。这将使用对象为该属性提供的所有数据填充整个缓冲区。
 * 然后我们调用 flush_read_buffer() 按指定的增量将缓冲区复制到用户空间。
 */
static ssize_t
sysfs_read_file(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct sysfs_buffer *buffer = file->private_data;  // 获取文件的私有数据，即 sysfs_buffer
	ssize_t retval = 0;

	mutex_lock(&buffer->mutex);  // 加锁，保证对 buffer 的访问是线程安全的
	if (buffer->needs_read_fill || *ppos == 0) {  // 如果需要填充缓冲区或从文件开头读取
		retval = fill_read_buffer(file->f_path.dentry, buffer);  // 调用 fill_read_buffer 填充缓冲区
		if (retval)
			goto out;  // 如果填充缓冲区失败，跳转到 out 标签进行解锁并返回错误
	}
	pr_debug("%s: count = %zd, ppos = %lld, buf = %s\n",
					__func__, count, *ppos, buffer->page);  // 调试信息，打印读取的相关信息
	retval = simple_read_from_buffer(buf, count, ppos, buffer->page,
																		buffer->count);  // 从缓冲区中读取数据到用户空间
out:
	mutex_unlock(&buffer->mutex);  // 解锁
	return retval;  // 返回读取的字节数或错误代码
}

/**
 *	fill_write_buffer - copy buffer from userspace.
 *	@buffer:	data buffer for file.
 *	@buf:		data from user.
 *	@count:		number of bytes in @userbuf.
 *
 *	Allocate @buffer->page if it hasn't been already, then
 *	copy the user-supplied buffer into it.
 */
/**
 * fill_write_buffer - 从用户空间复制缓冲区。
 * @buffer: 文件的数据缓冲区。
 * @buf: 用户提供的数据。
 * @count: @userbuf 中的字节数。
 *
 * 如果尚未分配 @buffer->page，则分配它，然后将用户提供的缓冲区复制进去。
 */
static int 
fill_write_buffer(struct sysfs_buffer * buffer, const char __user * buf, size_t count)
{
	int error;

	if (!buffer->page)
		buffer->page = (char *)get_zeroed_page(GFP_KERNEL);  // 分配一个初始化为零的页面
	if (!buffer->page)
		return -ENOMEM;  // 如果页面分配失败，则返回内存不足错误

	if (count >= PAGE_SIZE)
		count = PAGE_SIZE - 1;  // 如果计数超过页面大小，调整计数以适应页面

	error = copy_from_user(buffer->page, buf, count);  // 从用户空间复制数据到内核空间
	buffer->needs_read_fill = 1;  // 标记缓冲区需要重新填充
	/* if buf is assumed to contain a string, terminate it by \0,
	   so e.g. sscanf() can scan the string easily */
	/* 如果 buf 被假定为包含字符串，通过 \0 终止它，
			这样例如 sscanf() 可以容易地扫描字符串 */
	buffer->page[count] = 0;  // 添加字符串终止符
	return error ? -EFAULT : count;  // 如果复制过程中出错，返回错误，否则返回复制的字节数
}

/**
 *	flush_write_buffer - push buffer to kobject.
 *	@dentry:	dentry to the attribute
 *	@buffer:	data buffer for file.
 *	@count:		number of bytes
 *
 *	Get the correct pointers for the kobject and the attribute we're
 *	dealing with, then call the store() method for the attribute, 
 *	passing the buffer that we acquired in fill_write_buffer().
 */
/**
 * flush_write_buffer - 将缓冲区推送到 kobject。
 * @dentry: 属性的目录项
 * @buffer: 文件的数据缓冲区。
 * @count: 字节数
 *
 * 获取我们正在处理的 kobject 和属性的正确指针，然后调用该属性的 store() 方法，
 * 传递我们在 fill_write_buffer() 中获取的缓冲区。
 */
static int
flush_write_buffer(struct dentry *dentry, struct sysfs_buffer *buffer, size_t count)
{
	struct sysfs_dirent *attr_sd = dentry->d_fsdata;  // 从目录项获取 sysfs_dirent
	struct kobject *kobj = attr_sd->s_parent->s_dir.kobj;  // 获取 kobject
	const struct sysfs_ops *ops = buffer->ops;  // 获取 sysfs 操作
	int rc;

	/* need attr_sd for attr and ops, its parent for kobj */
	// 需要 attr_sd 用于获取 attr 和 ops，它的 parent 用于获取 kobj
	if (!sysfs_get_active(attr_sd))
		return -ENODEV;  // 如果无法激活 sysfs_dirent，返回设备不存在错误

	rc = ops->store(kobj, attr_sd->s_attr.attr, buffer->page, count);  // 调用 store() 方法写入数据

	sysfs_put_active(attr_sd);  // 释放 sysfs_dirent

	return rc;  // 返回 store() 方法的结果
}

/**
 *	sysfs_write_file - write an attribute.
 *	@file:	file pointer
 *	@buf:	data to write
 *	@count:	number of bytes
 *	@ppos:	starting offset
 *
 *	Similar to sysfs_read_file(), though working in the opposite direction.
 *	We allocate and fill the data from the user in fill_write_buffer(),
 *	then push it to the kobject in flush_write_buffer().
 *	There is no easy way for us to know if userspace is only doing a partial
 *	write, so we don't support them. We expect the entire buffer to come
 *	on the first write. 
 *	Hint: if you're writing a value, first read the file, modify only the
 *	the value you're changing, then write entire buffer back. 
 */
/**
 * sysfs_write_file - 写入一个属性。
 * @file: 文件指针
 * @buf: 要写入的数据
 * @count: 字节数
 * @ppos: 起始偏移量
 *
 * 类似于 sysfs_read_file()，但操作方向相反。
 * 我们在 fill_write_buffer() 中分配并填充来自用户的数据，
 * 然后在 flush_write_buffer() 中推送它到 kobject。
 * 我们无法轻易知道用户空间是否只进行了部分写入，所以我们不支持部分写入。
 * 我们期望在第一次写入时接收到整个缓冲区。
 * 提示：如果你正在写入一个值，首先读取文件，只修改你需要改变的值，然后将整个缓冲区写回。
 */
static ssize_t
sysfs_write_file(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct sysfs_buffer *buffer = file->private_data;  // 获取文件的私有数据，即 sysfs_buffer
	ssize_t len;

	mutex_lock(&buffer->mutex);  // 加锁，保证对 buffer 的访问是线程安全的
	len = fill_write_buffer(buffer, buf, count);  // 将用户空间数据填充到缓冲区
	if (len > 0)
		len = flush_write_buffer(file->f_path.dentry, buffer, len);  // 将填充的数据推送到 kobject
	if (len > 0)
		*ppos += len;  // 更新文件的位置偏移
	mutex_unlock(&buffer->mutex);  // 解锁
	return len;  // 返回写入的字节数或错误代码
}

/**
 *	sysfs_get_open_dirent - get or create sysfs_open_dirent
 *	@sd: target sysfs_dirent
 *	@buffer: sysfs_buffer for this instance of open
 *
 *	If @sd->s_attr.open exists, increment its reference count;
 *	otherwise, create one.  @buffer is chained to the buffers
 *	list.
 *
 *	LOCKING:
 *	Kernel thread context (may sleep).
 *
 *	RETURNS:
 *	0 on success, -errno on failure.
 */
/**
 * sysfs_get_open_dirent - 获取或创建 sysfs_open_dirent
 * @sd: 目标 sysfs_dirent
 * @buffer: 这个打开实例的 sysfs_buffer
 *
 * 如果 @sd->s_attr.open 存在，增加其引用计数；否则，创建一个新的。
 * @buffer 被链接到缓冲区列表中。
 *
 * 锁定：
 * 内核线程上下文（可能会睡眠）。
 *
 * 返回值：
 * 成功时返回 0，失败时返回 -errno。
 */
static int sysfs_get_open_dirent(struct sysfs_dirent *sd,
				 struct sysfs_buffer *buffer)
{
	struct sysfs_open_dirent *od, *new_od = NULL;

 retry:
	spin_lock_irq(&sysfs_open_dirent_lock);  // 加锁，防止中断影响

	if (!sd->s_attr.open && new_od) {
		sd->s_attr.open = new_od;  // 如果不存在 open_dirent，则赋予新创建的
		new_od = NULL;  // 已使用 new_od，现在将其置为 NULL
	}

	od = sd->s_attr.open;  // 获取现有的 open_dirent
	if (od) {
		atomic_inc(&od->refcnt);  // 增加引用计数
		list_add_tail(&buffer->list, &od->buffers);  // 将 buffer 添加到链表中
	}

	spin_unlock_irq(&sysfs_open_dirent_lock);  // 解锁

	if (od) {
		kfree(new_od);  // 如果使用了旧的 open_dirent，释放新分配的内存
		return 0;  // 成功返回
	}

	/* not there, initialize a new one and retry */
	// 如果没有找到，初始化一个新的然后重试
	new_od = kmalloc(sizeof(*new_od), GFP_KERNEL);  // 分配内存
	if (!new_od)
		return -ENOMEM;  // 内存分配失败

	atomic_set(&new_od->refcnt, 0);  // 初始化引用计数
	atomic_set(&new_od->event, 1);  // 初始化事件计数
	init_waitqueue_head(&new_od->poll);  // 初始化等待队列头
	INIT_LIST_HEAD(&new_od->buffers);  // 初始化链表头
	goto retry;  // 重试整个过程
}

/**
 *	sysfs_put_open_dirent - put sysfs_open_dirent
 *	@sd: target sysfs_dirent
 *	@buffer: associated sysfs_buffer
 *
 *	Put @sd->s_attr.open and unlink @buffer from the buffers list.
 *	If reference count reaches zero, disassociate and free it.
 *
 *	LOCKING:
 *	None.
 */
/**
 * sysfs_put_open_dirent - 释放 sysfs_open_dirent
 * @sd: 目标 sysfs_dirent
 * @buffer: 相关联的 sysfs_buffer
 *
 * 对 @sd->s_attr.open 进行释放，并从缓冲区列表中解链 @buffer。
 * 如果引用计数降至零，则解除关联并释放它。
 *
 * 锁定：
 * 无。
 */
static void sysfs_put_open_dirent(struct sysfs_dirent *sd,
				  struct sysfs_buffer *buffer)
{
	struct sysfs_open_dirent *od = sd->s_attr.open;  // 获取关联的 sysfs_open_dirent
	unsigned long flags;

	spin_lock_irqsave(&sysfs_open_dirent_lock, flags);  // 加锁，保存中断状态

	list_del(&buffer->list);  // 从链表中移除 buffer
	if (atomic_dec_and_test(&od->refcnt))  // 如果引用计数减至 0
		sd->s_attr.open = NULL;  // 清除 sd 中的 open 指针
	else
		od = NULL;  // 如果还有其他引用，保留 od

	spin_unlock_irqrestore(&sysfs_open_dirent_lock, flags);  // 解锁，恢复中断状态

	kfree(od);  // 如果 od 已无用，则释放内存
}

/**
 * sysfs_open_file - 打开 sysfs 文件。
 * @inode: 文件的 inode。
 * @file: 要打开的文件。
 *
 * 这个函数负责处理 sysfs 文件的打开操作，包括权限检查、缓冲区分配，
 * 并确保相关的操作（读取和写入）可以被正确执行。
 *
 * 返回值:
 * 成功时返回0，失败时返回负的错误代码。
 */
static int sysfs_open_file(struct inode *inode, struct file *file)
{
	struct sysfs_dirent *attr_sd = file->f_path.dentry->d_fsdata; // 获取 sysfs_dirent
	struct kobject *kobj = attr_sd->s_parent->s_dir.kobj; // // 获取包含该属性的 kobject
	struct sysfs_buffer *buffer; // 声明一个 sysfs_buffer 指针，用于读写操作的缓冲区
	const struct sysfs_ops *ops; // sysfs 操作指针
	int error = -EACCES; // 默认错误代码为访问被拒绝
	char *p;

	// 获取当前 sysfs 文件的路径并存储到 last_sysfs_file 中，用于错误调试
	p = d_path(&file->f_path, last_sysfs_file, sizeof(last_sysfs_file));
	if (p)
		memmove(last_sysfs_file, p, strlen(p) + 1);

	/* need attr_sd for attr and ops, its parent for kobj */
	// 验证 attr_sd 是否处于活跃状态
	if (!sysfs_get_active(attr_sd))	// 如果获取活跃的 sysfs_dirent 失败，返回 -ENODEV（设备不存在）
		return -ENODEV; // 设备不存在

	/* every kobject with an attribute needs a ktype assigned */
	// 确保每个有属性的 kobject 都分配了 ktype
	if (kobj->ktype && kobj->ktype->sysfs_ops)	// 检查 kobject 是否有对应的 sysfs 操作，如果没有，则输出警告并处理错误
		ops = kobj->ktype->sysfs_ops;
	else {
		WARN(1, KERN_ERR "missing sysfs attribute operations for "
		       "kobject: %s\n", kobject_name(kobj));
		goto err_out;
	}

	/* File needs write support.
	 * The inode's perms must say it's ok, 
	 * and we must have a store method.
	 */
	// 检查是否有写权限，如果 inode 权限和操作中没有写方法，则处理错误
	if (file->f_mode & FMODE_WRITE) {
		if (!(inode->i_mode & S_IWUGO) || !ops->store)
			goto err_out;
	}

	/* File needs read support.
	 * The inode's perms must say it's ok, and we there
	 * must be a show method for it.
	 */
	// 检查是否有读权限，如果 inode 权限和操作中没有读方法，则处理错误
	if (file->f_mode & FMODE_READ) {
		if (!(inode->i_mode & S_IRUGO) || !ops->show)
			goto err_out;
	}

	/* No error? Great, allocate a buffer for the file, and store it
	 * it in file->private_data for easy access.
	 */
	// 分配缓冲区并初始化
	error = -ENOMEM;
	buffer = kzalloc(sizeof(struct sysfs_buffer), GFP_KERNEL);
	if (!buffer)
		goto err_out;

	// 初始化缓冲区
	mutex_init(&buffer->mutex); // 初始化互斥锁
	buffer->needs_read_fill = 1; // 标记需要填充读缓冲区
	buffer->ops = ops; // 设置文件操作
	file->private_data = buffer; // 将 buffer 设置为文件的私有数据

	/* make sure we have open dirent struct */
	// 确保已经创建或获取了 open dirent 结构
	error = sysfs_get_open_dirent(attr_sd, buffer);
	if (error)
		goto err_free;

	/* open succeeded, put active references */
	// 如果打开成功，释放活跃的引用
	sysfs_put_active(attr_sd);
	return 0;

err_free:
	kfree(buffer); // 如果出错，释放缓冲区
err_out:
	sysfs_put_active(attr_sd); // 释放 attr_sd 的活跃状态
	return error;
}

/**
 * sysfs_release - 释放 sysfs 文件。
 * @inode: 文件的 inode 对象。
 * @filp: 文件对象。
 *
 * 当最后一个引用到 sysfs 文件的文件描述符被关闭时调用此函数。
 * 它负责释放与文件关联的所有资源。
 *
 * 返回值:
 * 总是返回 0。
 */
static int sysfs_release(struct inode *inode, struct file *filp)
{
	struct sysfs_dirent *sd = filp->f_path.dentry->d_fsdata; // 从文件对象获取 sysfs_dirent
	struct sysfs_buffer *buffer = filp->private_data; // 从文件对象获取私有数据，即之前分配的缓冲区

	sysfs_put_open_dirent(sd, buffer); // 释放与打开的 sysfs_dirent 关联的资源

	if (buffer->page) // 检查缓冲区是否存在
		free_page((unsigned long)buffer->page); // 释放分配的页
	kfree(buffer); // 释放 sysfs_buffer 结构

	return 0; // 返回成功
}

/* Sysfs attribute files are pollable.  The idea is that you read
 * the content and then you use 'poll' or 'select' to wait for
 * the content to change.  When the content changes (assuming the
 * manager for the kobject supports notification), poll will
 * return POLLERR|POLLPRI, and select will return the fd whether
 * it is waiting for read, write, or exceptions.
 * Once poll/select indicates that the value has changed, you
 * need to close and re-open the file, or seek to 0 and read again.
 * Reminder: this only works for attributes which actively support
 * it, and it is not possible to test an attribute from userspace
 * to see if it supports poll (Neither 'poll' nor 'select' return
 * an appropriate error code).  When in doubt, set a suitable timeout value.
 */
/* Sysfs属性文件是可轮询的。这意味着你可以读取内容，然后使用'poll'或'select'等待内容的变化。
 * 当内容变化时（假设kobject的管理者支持通知），poll将返回POLLERR|POLLPRI，
 * 而select将返回文件描述符，无论它是等待读取、写入还是异常。
 * 一旦poll/select指示值已经改变，你需要关闭并重新打开文件，或者重新定位到文件开头并再次读取。
 * 提醒：这只适用于主动支持该功能的属性，并且无法从用户空间测试属性是否支持poll
 * （'poll'和'select'都不会返回合适的错误码）。当有疑问时，设置一个合适的超时值。
 */
/**
 * sysfs_poll - 在 sysfs 属性文件上执行 poll 操作。
 * @filp: 文件对象。
 * @wait: poll_table 结构，用于注册等待队列。
 *
 * sysfs 属性文件支持 poll 操作。读取文件内容后，使用 'poll' 或 'select'
 * 等待内容变化。内容发生变化时（假设 kobject 的管理者支持通知），poll 会
 * 返回 POLLERR|POLLPRI，select 会返回相应的文件描述符。
 * 一旦 poll/select 表明值已改变，你需要关闭并重新打开文件，或者重新定位到文件开头并再次读取。
 * 提醒：这仅适用于主动支持它的属性，并且无法从用户空间测试属性是否支持 poll
 * （'poll' 或 'select' 不返回适当的错误代码）。如有疑问，请设置合适的超时值。
 *
 * 返回值:
 * 如果没有变化，则返回默认的 poll 掩码。如果检测到变化，除了默认的 poll 掩码外，
 * 还将添加 POLLERR 和 POLLPRI。
 */
static unsigned int sysfs_poll(struct file *filp, poll_table *wait)
{
	struct sysfs_buffer *buffer = filp->private_data; // 获取文件的私有数据
	struct sysfs_dirent *attr_sd = filp->f_path.dentry->d_fsdata; // 获取 sysfs_dirent
	struct sysfs_open_dirent *od = attr_sd->s_attr.open; // 获取打开的 dirent

	/* need parent for the kobj, grab both */
	// 需要获取父对象的 kobj，首先获取 attr_sd
	if (!sysfs_get_active(attr_sd))
		goto trigger; // 如果获取失败，直接跳转到触发变化处理

	poll_wait(filp, &od->poll, wait); // 在等待队列中注册 poll 表

	sysfs_put_active(attr_sd); // 释放活跃引用

	if (buffer->event != atomic_read(&od->event)) // 检查事件计数器是否发生变化
		goto trigger; // 如果发生变化，处理变化

	return DEFAULT_POLLMASK; // 返回默认的 poll 掩码

trigger: // 触发变化的处理逻辑
	buffer->needs_read_fill = 1; // 标记需要重新填充读缓冲区
	return DEFAULT_POLLMASK | POLLERR | POLLPRI; // 返回表示错误和优先级事件的掩码
}

/**
 * sysfs_notify_dirent - 通知 sysfs 目录项有事件发生。
 * @sd: 目标 sysfs_dirent 对象。
 *
 * 当 sysfs 目录项的内容发生变化时调用此函数，它增加目录项事件计数器并唤醒
 * 等待该目录项事件的所有进程。
 */
void sysfs_notify_dirent(struct sysfs_dirent *sd)
{
	struct sysfs_open_dirent *od; // 定义指向打开的目录项的指针
	unsigned long flags; // 定义用于保存中断状态的变量

	spin_lock_irqsave(&sysfs_open_dirent_lock, flags); // 加锁，保护临界区，并保存中断状态

	od = sd->s_attr.open; // 获取与 sysfs 目录项关联的打开目录项结构
	if (od) { // 如果打开的目录项存在
		atomic_inc(&od->event); // 原子增加事件计数器
		wake_up_interruptible(&od->poll); // 唤醒所有在此目录项的 poll 队列中等待的进程
	}

	spin_unlock_irqrestore(&sysfs_open_dirent_lock, flags); // 解锁并恢复之前保存的中断状态
}
EXPORT_SYMBOL_GPL(sysfs_notify_dirent);

/**
 * sysfs_notify - 通知相关的 sysfs 目录项发生变化。
 * @k: 目标 kobject。
 * @dir: 目录的名称。
 * @attr: 属性的名称。
 *
 * 此函数在指定的 kobject 目录项或属性发生变化时被调用。
 * 它将递归查找指定的目录和属性，并触发相应的通知。
 */
void sysfs_notify(struct kobject *k, const char *dir, const char *attr)
{
	struct sysfs_dirent *sd = k->sd; // 获取 kobject 的 sysfs_dirent

	mutex_lock(&sysfs_mutex); // 加锁，保护对 sysfs 结构的修改

	if (sd && dir)
			sd = sysfs_find_dirent(sd, dir); // 如果指定了目录，找到该目录对应的 dirent
	if (sd && attr)
			sd = sysfs_find_dirent(sd, attr); // 如果指定了属性，找到该属性对应的 dirent
	if (sd)
		sysfs_notify_dirent(sd); // 如果 dirent 存在，则触发通知

	mutex_unlock(&sysfs_mutex); // 解锁
}
EXPORT_SYMBOL_GPL(sysfs_notify);

/**
 * sysfs_file_operations - 操作 sysfs 文件的函数集。
 *
 * 定义了 sysfs 文件的基本操作，包括读写、定位、打开、释放和轮询。
 */
const struct file_operations sysfs_file_operations = {
	.read    = sysfs_read_file,        // 读文件
	.write   = sysfs_write_file,       // 写文件
	.llseek  = generic_file_llseek,    // 文件定位
	.open    = sysfs_open_file,        // 打开文件
	.release = sysfs_release,          // 释放文件
	.poll    = sysfs_poll,             // 轮询文件
};

/**
 * sysfs_add_file_mode - 在 sysfs 中添加一个文件，并设置其模式。
 * @dir_sd: 要添加文件的目录项。
 * @attr: 要添加的属性。
 * @type: 文件类型（例如 SYSFS_KOBJ_ATTR）。
 * @amode: 文件的访问模式。
 *
 * 创建一个新的 sysfs 文件项，并将其与指定的属性关联。
 * 返回值:
 * 成功时返回 0；如果内存分配失败，返回 -ENOMEM；其他错误时返回对应的错误代码。
 */
int sysfs_add_file_mode(struct sysfs_dirent *dir_sd,
			const struct attribute *attr, int type, mode_t amode)
{
	umode_t mode = (amode & S_IALLUGO) | S_IFREG; // 计算文件的访问权限模式，并设置文件类型为普通文件
	struct sysfs_addrm_cxt acxt; // 声明一个上下文变量，用于管理文件添加或移除操作
	struct sysfs_dirent *sd; // 声明一个指向sysfs目录项的指针
	int rc; // 声明一个整型变量用于存放函数返回值

	sd = sysfs_new_dirent(attr->name, mode, type); // 创建一个新的 sysfs_dirent 结构
	if (!sd) // 如果创建失败
		return -ENOMEM; // 返回内存不足的错误
	// 将属性与新创建的 dirent 关联
	sd->s_attr.attr = (void *)attr; // 将属性指针存储在sysfs目录项中
	// 初始化锁依赖跟踪，用于调试
	sysfs_dirent_init_lockdep(sd); // 初始化目录项的锁依赖

	// 初始化地址管理上下文
	sysfs_addrm_start(&acxt, dir_sd); // 开始添加或移除目录项的操作，锁定相关的互斥量
	// 尝试添加 dirent 到 sysfs
	rc = sysfs_add_one(&acxt, sd); // 将新创建的目录项添加到父目录中
	// 完成地址管理操作
	sysfs_addrm_finish(&acxt); // 完成添加或移除目录项的操作，释放互斥量

	if (rc) // 如果添加失败
		sysfs_put(sd); // 如果添加失败，减少 dirent 的引用计数，可能导致其释放

	return rc; // 返回操作结果
}

/**
 * sysfs_add_file - 向 sysfs 目录项添加一个文件
 * @dir_sd: 目标目录项的 sysfs_dirent
 * @attr: 要添加的属性
 * @type: 文件类型
 *
 * 这个函数简单封装了 sysfs_add_file_mode 函数，用于添加一个属性文件到 sysfs 中。
 * 它使用提供的属性结构中定义的模式 (mode) 作为文件的权限。
 *
 * 返回值:
 * 如果成功，返回 0。如果失败，返回一个负的错误代码。
 */
int sysfs_add_file(struct sysfs_dirent *dir_sd, const struct attribute *attr,
		   int type)
{
	// 调用 sysfs_add_file_mode，传入目录项、属性、文件类型和文件模式
	// attr->mode 通常定义了文件的读写权限
	return sysfs_add_file_mode(dir_sd, attr, type, attr->mode);
}


/**
 *	sysfs_create_file - create an attribute file for an object.
 *	@kobj:	object we're creating for. 
 *	@attr:	attribute descriptor.
 */
/**
 * sysfs_create_file - 为对象创建一个属性文件。
 * @kobj: 我们为其创建属性文件的对象。
 * @attr: 属性描述符。
 *
 * 为 kobject 创建一个 sysfs 属性文件。这允许用户空间通过文件系统与内核对象的属性进行交互。
 *
 * 返回值:
 * 成功时返回 0，失败时返回负值错误代码。
 */
// 创建新的属性，attr参数指向相应的attribute结构体，kobj指向属性所在的kobject对象
// 成功返回0,失败返回负的错误码
int sysfs_create_file(struct kobject * kobj, const struct attribute * attr)
{
	BUG_ON(!kobj || !kobj->sd || !attr); // 断言 kobj 和 attr 都不为空，kobj 必须有有效的 sysfs_dirent

	// 调用 sysfs_add_file 函数，传入 kobject 的 sysfs_dirent、属性和指定类型为 SYSFS_KOBJ_ATTR
	return sysfs_add_file(kobj->sd, attr, SYSFS_KOBJ_ATTR);
}

/**
 * sysfs_create_files - 为对象创建多个属性文件。
 * @kobj: 我们为其创建属性文件的对象。
 * @ptr: 指向属性描述符数组的指针，数组以 NULL 结尾。
 *
 * 为给定的 kobject 创建多个属性文件。如果在创建文件的过程中出现错误，
 * 将撤销所有已经创建的文件，确保操作的原子性。
 *
 * 返回值:
 * 成功时返回 0，如果有一个或多个文件创建失败，则返回负值错误代码。
 */
int sysfs_create_files(struct kobject *kobj, const struct attribute **ptr)
{
	int err = 0; // 错误代码初始化为 0，表示无错误
	int i;

	for (i = 0; ptr[i] && !err; i++) // 遍历属性数组，直到数组结束或遇到错误
		err = sysfs_create_file(kobj, ptr[i]); // 创建单个属性文件
	if (err) // 如果有错误发生
		while (--i >= 0) // 回滚操作，删除已创建的所有文件
			sysfs_remove_file(kobj, ptr[i]);
	return err; // 返回错误代码或成功状态
}

/**
 * sysfs_add_file_to_group - add an attribute file to a pre-existing group.
 * @kobj: object we're acting for.
 * @attr: attribute descriptor.
 * @group: group name.
 */
/**
 * sysfs_add_file_to_group - 将属性文件添加到已存在的组中。
 * @kobj: 我们正在操作的对象。
 * @attr: 属性描述符。
 * @group: 组的名称。
 *
 * 为给定的 kobject 在 sysfs 中的特定组下添加一个属性文件。如果组名为 NULL，则添加到对象的根目录下。
 * 返回值:
 * 成功时返回 0；如果组不存在或添加失败，则返回负值错误代码。
 */
int sysfs_add_file_to_group(struct kobject *kobj,
		const struct attribute *attr, const char *group)
{
	struct sysfs_dirent *dir_sd; // 目录项变量
	int error; // 用于存储返回的错误代码

	if (group)
		dir_sd = sysfs_get_dirent(kobj->sd, group); // 获取指定组名的目录项
	else
		dir_sd = sysfs_get(kobj->sd); // 如果没有指定组名，则获取 kobject 的根目录项

	if (!dir_sd)
		return -ENOENT; // 如果目录项不存在，返回 -ENOENT 错误

	error = sysfs_add_file(dir_sd, attr, SYSFS_KOBJ_ATTR); // 尝试在找到的目录项下添加文件
	sysfs_put(dir_sd); // 减少目录项的引用计数

	return error; // 返回操作结果
}
EXPORT_SYMBOL_GPL(sysfs_add_file_to_group);

/**
 * sysfs_chmod_file - update the modified mode value on an object attribute.
 * @kobj: object we're acting for.
 * @attr: attribute descriptor.
 * @mode: file permissions.
 *
 */
/**
 * sysfs_chmod_file - 更新对象属性上的权限模式。
 * @kobj: 我们正在操作的对象。
 * @attr: 属性描述符。
 * @mode: 文件权限。
 *
 * 此函数用于更改 sysfs 中特定属性文件的权限，允许更细粒度地控制对属性的访问。
 * 返回值:
 * 成功时返回 0；如果未找到相应的目录项或属性，则返回 -ENOENT 错误。
 */
int sysfs_chmod_file(struct kobject *kobj, struct attribute *attr, mode_t mode)
{
	struct sysfs_dirent *sd; // 代表 sysfs 目录项的结构
	struct iattr newattrs;   // inode 属性结构
	int rc;                  // 用于存储返回的结果代码

	mutex_lock(&sysfs_mutex); // 获取互斥锁以确保线程安全

	rc = -ENOENT; // 默认设置返回值为 -ENOENT，表示找不到文件或目录
	sd = sysfs_find_dirent(kobj->sd, attr->name); // 查找与属性名对应的 sysfs 目录项
	if (!sd) // 如果目录项不存在
		goto out; // 直接跳转到出口代码

	// 准备新的属性值
	newattrs.ia_mode = (mode & S_IALLUGO) | (sd->s_mode & ~S_IALLUGO); // 更新权限模式，保留非权限位
	newattrs.ia_valid = ATTR_MODE; // 指定正在更改的属性是模式
	rc = sysfs_sd_setattr(sd, &newattrs); // 更新目录项的属性

 out: // 出口代码
	mutex_unlock(&sysfs_mutex); // 释放互斥锁
	return rc; // 返回操作结果
}
EXPORT_SYMBOL_GPL(sysfs_chmod_file);

/**
 *	sysfs_remove_file - remove an object attribute.
 *	@kobj:	object we're acting for.
 *	@attr:	attribute descriptor.
 *
 *	Hash the attribute name and kill the victim.
 */
/**
 * sysfs_remove_file - 删除对象属性。
 * @kobj: 我们正在操作的对象。
 * @attr: 属性描述符。
 *
 * 通过属性名进行散列并删除对应的条目。
 */
// 在 sysfs 中删除一个属性
void sysfs_remove_file(struct kobject * kobj, const struct attribute * attr)
{
	// 调用 sysfs_hash_and_remove 函数删除属性
	sysfs_hash_and_remove(kobj->sd, attr->name);
}

/**
 * sysfs_remove_files - 删除一组对象属性。
 * @kobj: 我们正在操作的对象。
 * @ptr: 指向属性描述符数组的指针，该数组以 NULL 结尾。
 *
 * 遍历属性描述符数组，逐个删除属性。
 */
void sysfs_remove_files(struct kobject * kobj, const struct attribute **ptr)
{
	int i; // 循环索引
	// 遍历属性数组，直到遇到 NULL 指针
	for (i = 0; ptr[i]; i++)
		// 删除单个属性
		sysfs_remove_file(kobj, ptr[i]);
}

/**
 * sysfs_remove_file_from_group - remove an attribute file from a group.
 * @kobj: object we're acting for.
 * @attr: attribute descriptor.
 * @group: group name.
 */
/**
 * sysfs_remove_file_from_group - 从组中删除一个属性文件。
 * @kobj: 我们正在操作的对象。
 * @attr: 属性描述符。
 * @group: 组名。
 */
void sysfs_remove_file_from_group(struct kobject *kobj,
		const struct attribute *attr, const char *group)
{
	struct sysfs_dirent *dir_sd; // 定义一个指向 sysfs_dirent 结构的指针

	// 如果指定了组名，尝试获取该组对应的 sysfs_dirent 结构
	if (group)
		dir_sd = sysfs_get_dirent(kobj->sd, group);
	else
		// 如果没有指定组名，则获取 kobject 的 sysfs_dirent 结构
		dir_sd = sysfs_get(kobj->sd);

	// 如果成功获取到了 sysfs_dirent 结构
	if (dir_sd) {
		// 删除指定的属性文件
		sysfs_hash_and_remove(dir_sd, attr->name);
		// 释放对 sysfs_dirent 的引用
		sysfs_put(dir_sd);
	}
}
EXPORT_SYMBOL_GPL(sysfs_remove_file_from_group);

// 定义一个结构体，用于工作队列中的任务项
struct sysfs_schedule_callback_struct {
	struct list_head	workq_list;    // 工作队列链表节点
	struct kobject		*kobj;         // 相关联的 kobject
	void			(*func)(void *); // 要执行的回调函数
	void			*data;          // 传递给回调函数的数据
	struct module		*owner;        // 拥有者模块，用于模块引用计数
	struct work_struct	work;          // 内嵌的工作结构体，用于工作队列机制
};

// 声明一个全局的工作队列
static struct workqueue_struct *sysfs_workqueue;
// 定义一个互斥锁，用于同步对工作队列的访问
static DEFINE_MUTEX(sysfs_workq_mutex);
// 初始化工作队列链表头
static LIST_HEAD(sysfs_workq);

// 定义工作队列的工作处理函数
static void sysfs_schedule_callback_work(struct work_struct *work)
{
	struct sysfs_schedule_callback_struct *ss = container_of(work,
			struct sysfs_schedule_callback_struct, work); // 从 work_struct 获取封装的 sysfs_schedule_callback_struct 结构体

	(ss->func)(ss->data); // 执行存储的回调函数，传递给定数据
	kobject_put(ss->kobj); // 减少 kobject 的引用计数，如果减到零，则释放 kobject
	module_put(ss->owner); // 减少模块的引用计数，如果减到零，则可以卸载模块
	mutex_lock(&sysfs_workq_mutex); // 上锁保护工作队列
	list_del(&ss->workq_list); // 从工作队列中移除当前工作项
	mutex_unlock(&sysfs_workq_mutex); // 解锁
	kfree(ss); // 释放为工作项分配的内存
}

/**
 * sysfs_schedule_callback - helper to schedule a callback for a kobject
 * @kobj: object we're acting for.
 * @func: callback function to invoke later.
 * @data: argument to pass to @func.
 * @owner: module owning the callback code
 *
 * sysfs attribute methods must not unregister themselves or their parent
 * kobject (which would amount to the same thing).  Attempts to do so will
 * deadlock, since unregistration is mutually exclusive with driver
 * callbacks.
 *
 * Instead methods can call this routine, which will attempt to allocate
 * and schedule a workqueue request to call back @func with @data as its
 * argument in the workqueue's process context.  @kobj will be pinned
 * until @func returns.
 *
 * Returns 0 if the request was submitted, -ENOMEM if storage could not
 * be allocated, -ENODEV if a reference to @owner isn't available,
 * -EAGAIN if a callback has already been scheduled for @kobj.
 */
/**
 * sysfs_schedule_callback - 为 kobject 安排一个回调函数
 * @kobj: 我们要操作的对象。
 * @func: 稍后调用的回调函数。
 * @data: 传递给 @func 的参数。
 * @owner: 拥有回调代码的模块
 *
 * sysfs 属性方法不应该取消注册自己或其父 kobject（这实际上是一回事）。尝试这样做将会导致死锁，
 * 因为取消注册与驱动回调是互斥的。
 *
 * 相反，方法可以调用这个例程，它将尝试分配并安排一个工作队列请求，在工作队列的进程上下文中调用 @func，
 * 参数为 @data。在 @func 返回之前，@kobj 将被固定。
 *
 * 如果请求提交成功，返回 0；如果无法分配存储空间，返回 -ENOMEM；
 * 如果无法获得对 @owner 的引用，返回 -ENODEV；如果已为 @kobj 安排了回调，返回 -EAGAIN。
 */
int sysfs_schedule_callback(struct kobject *kobj, void (*func)(void *),
			void *data, struct module *owner)
{
	struct sysfs_schedule_callback_struct *ss, *tmp;

	// 尝试获取模块引用，如果失败，返回 -ENODEV
	if (!try_module_get(owner))
		return -ENODEV;

	// 锁定工作队列，保证线程安全
	mutex_lock(&sysfs_workq_mutex);

	// 遍历现有的工作队列，检查是否已有针对同一个 kobject 的调度工作
	list_for_each_entry_safe(ss, tmp, &sysfs_workq, workq_list)
		if (ss->kobj == kobj) {	// 检查是否已经为这个 kobj 安排了回调
			module_put(owner);
			mutex_unlock(&sysfs_workq_mutex);
			return -EAGAIN; // 如果找到，释放模块引用，解锁，返回 -EAGAIN
		}
	mutex_unlock(&sysfs_workq_mutex);

	// 如果工作队列尚未创建，创建之
	if (sysfs_workqueue == NULL) {
		sysfs_workqueue = create_singlethread_workqueue("sysfsd");
		if (sysfs_workqueue == NULL) {
			module_put(owner);
			return -ENOMEM; // 如果创建失败，释放模块引用，返回 -ENOMEM
		}
	}

	// 为新的回调分配内存
	ss = kmalloc(sizeof(*ss), GFP_KERNEL);	// 分配和初始化回调结构体
	if (!ss) {
		module_put(owner);
		return -ENOMEM; // 分配失败，释放模块引用，返回 -ENOMEM
	}

	// 获取 kobj 的引用，并初始化回调结构
	kobject_get(kobj);
	ss->kobj = kobj;
	ss->func = func;
	ss->data = data;
	ss->owner = owner;
	INIT_WORK(&ss->work, sysfs_schedule_callback_work);
	INIT_LIST_HEAD(&ss->workq_list);
	// 将回调结构添加到工作队列
	mutex_lock(&sysfs_workq_mutex);
	list_add_tail(&ss->workq_list, &sysfs_workq);
	mutex_unlock(&sysfs_workq_mutex);
	// 将工作项加入工作队列
	queue_work(sysfs_workqueue, &ss->work);
	return 0;
}
EXPORT_SYMBOL_GPL(sysfs_schedule_callback);


EXPORT_SYMBOL_GPL(sysfs_create_file);
EXPORT_SYMBOL_GPL(sysfs_remove_file);
EXPORT_SYMBOL_GPL(sysfs_remove_files);
EXPORT_SYMBOL_GPL(sysfs_create_files);
