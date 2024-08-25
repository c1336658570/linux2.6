/*
 * fs/sysfs/bin.c - sysfs binary file implementation
 *
 * Copyright (c) 2003 Patrick Mochel
 * Copyright (c) 2003 Matthew Wilcox
 * Copyright (c) 2004 Silicon Graphics, Inc.
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 *
 * This file is released under the GPLv2.
 *
 * Please see Documentation/filesystems/sysfs.txt for more information.
 */

#undef DEBUG

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/mm.h>

#include <asm/uaccess.h>

#include "sysfs.h"

/*
 * There's one bin_buffer for each open file.
 *
 * filp->private_data points to bin_buffer and
 * sysfs_dirent->s_bin_attr.buffers points to a the bin_buffer s
 * sysfs_dirent->s_bin_attr.buffers is protected by sysfs_bin_lock
 */
/*
 * 对于每一个打开的文件都有一个bin_buffer。
 *
 * filp->private_data 指向 bin_buffer，
 * sysfs_dirent->s_bin_attr.buffers 指向 bin_buffer。
 * sysfs_dirent->s_bin_attr.buffers 由 sysfs_bin_lock 保护。
 */
static DEFINE_MUTEX(sysfs_bin_lock); // 定义互斥锁用于保护 bin_buffer 列表

struct bin_buffer {
	struct mutex			mutex;     // 互斥锁保护 bin_buffer 的访问
	void				*buffer;   // 指向实际数据缓冲区的指针
	int				mmapped;  // 是否被内存映射的标志
	const struct vm_operations_struct *vm_ops; // 虚拟内存操作结构
	struct file			*file;     // 关联的文件指针
	struct hlist_node		list;     // 用于将 bin_buffer 链接到哈希链表的节点
};

/*
 * 从系统文件系统(sysfs)中的二进制属性读取数据。首先从 dentry 获取相关的 sysfs_dirent，
 * 然后获取对应的 bin_attribute 和它的父 kobject。函数检查是否能够激活 attr_sd，如果不能，
 * 则返回设备不存在错误（-ENODEV）。如果二进制属性有读函数定义，则调用此函数以尝试读取数据。
 * 最后，释放获取的活跃引用并返回操作的结果。
 */
static int
fill_read(struct dentry *dentry, char *buffer, loff_t off, size_t count)
{
	struct sysfs_dirent *attr_sd = dentry->d_fsdata; // 从dentry获取sysfs_dirent
	struct bin_attribute *attr = attr_sd->s_bin_attr.bin_attr; // 从sysfs_dirent获取二进制属性
	struct kobject *kobj = attr_sd->s_parent->s_dir.kobj; // 获取包含该属性的kobject
	int rc;

	/* need attr_sd for attr, its parent for kobj */
	// 检查attr_sd的活跃引用，确保其有效
	if (!sysfs_get_active(attr_sd))
		return -ENODEV; // 如果无法获取活跃引用，返回设备不存在错误

	rc = -EIO; // 默认返回输入/输出错误
	// 如果属性有读函数，则调用之
	if (attr->read)
		rc = attr->read(kobj, attr, buffer, off, count); // 读取数据到buffer

	// 释放之前获取的活跃引用
	sysfs_put_active(attr_sd);

	return rc; // 返回读操作的结果
}

// 实现了一个sysfs文件的读操作，首先进行一系列检查和准备工作，然后从指定的文件位置读取数据到内核缓冲区，接着把数据从内核空间拷贝到用户空间。
static ssize_t
read(struct file *file, char __user *userbuf, size_t bytes, loff_t *off)
{
	struct bin_buffer *bb = file->private_data; // 从文件结构体获取二进制缓冲区
	struct dentry *dentry = file->f_path.dentry; // 获取文件的目录项
	int size = dentry->d_inode->i_size; // 获取inode中记录的文件大小
	loff_t offs = *off; // 获取当前的偏移量
	int count = min_t(size_t, bytes, PAGE_SIZE); // 限制每次读取的数据不超过一页内存的大小
	char *temp;

	if (!bytes) // 如果请求读取的字节数为0，则直接返回0
		return 0;

	if (size) { // 如果文件大小非0
		if (offs > size) // 如果偏移量已超出文件大小，没有数据可读
			return 0;
		if (offs + count > size) // 如果读取的数据量加上偏移量超出文件大小，则调整读取的数量
			count = size - offs;
	}

	temp = kmalloc(count, GFP_KERNEL); // 分配临时缓存
	if (!temp) // 如果内存分配失败
		return -ENOMEM; // 返回内存不足错误

	mutex_lock(&bb->mutex); // 锁定缓冲区

	count = fill_read(dentry, bb->buffer, offs, count); // 从文件中读取数据到缓冲区
	if (count < 0) { // 如果读取失败
		mutex_unlock(&bb->mutex);
		goto out_free; // 释放资源并返回
	}

	memcpy(temp, bb->buffer, count); // 将数据从缓冲区复制到临时缓存

	mutex_unlock(&bb->mutex); // 解锁

	if (copy_to_user(userbuf, temp, count)) { // 将数据从内核空间复制到用户空间
		count = -EFAULT; // 如果复制失败，返回错误
		goto out_free;
	}

	pr_debug("offs = %lld, *off = %lld, count = %d\n", offs, *off, count); // 打印调试信息

	*off = offs + count; // 更新文件偏移量

 out_free:
	kfree(temp); // 释放临时缓存
	return count; // 返回读取的字节数
}

/*
 * 实现了一个sysfs二进制文件的写入操作。它首先检查是否可以获取到sysfs条目的活跃引用，
 * 这通常用来确保相关的设备或资源在操作期间可用。然后，如果定义了相应的写方法（write），
 * 它会调用这个方法来实际写入数据。操作完成后，它会释放之前获取的活跃引用。如果在这个过
 * 程中没有定义写方法，或者写方法返回错误，它会返回一个错误代码。
 */
static int
flush_write(struct dentry *dentry, char *buffer, loff_t offset, size_t count)
{
	struct sysfs_dirent *attr_sd = dentry->d_fsdata; // 获取目录项中的sysfs_dirent
	struct bin_attribute *attr = attr_sd->s_bin_attr.bin_attr; // 获取关联的二进制属性
	struct kobject *kobj = attr_sd->s_parent->s_dir.kobj; // 获取kobject对象
	int rc;

	/* need attr_sd for attr, its parent for kobj */
	// 获取活跃的引用，如果无法获取表示设备不可用
	if (!sysfs_get_active(attr_sd))
		return -ENODEV;

	rc = -EIO; // 默认设置为输入/输出错误
	// 如果定义了write方法，调用它写入数据
	if (attr->write)
		rc = attr->write(kobj, attr, buffer, offset, count);

	sysfs_put_active(attr_sd); // 释放获取的活跃引用

	return rc; // 返回结果代码
}

/*
 * sysfs文件系统中处理文件写操作的函数。它首先确定需要写入的字节数量，
 * 然后从用户空间将数据复制到内核空间的临时缓存。之后，它锁定文件的bin_buffer缓冲区，
 * 将数据从临时缓存复制到bin_buffer的缓冲区中，然后调用flush_write来实际处理数据写入。
 * 最后，它更新文件的偏移量，并在完成后释放资源和锁。
 */
static ssize_t write(struct file *file, const char __user *userbuf,
		     size_t bytes, loff_t *off)
{
	struct bin_buffer *bb = file->private_data; // 从文件结构体中获取关联的二进制缓冲区
	struct dentry *dentry = file->f_path.dentry; // 从文件结构体中获取目录项
	int size = dentry->d_inode->i_size; // 获取文件的大小
	loff_t offs = *off; // 当前文件偏移
	int count = min_t(size_t, bytes, PAGE_SIZE); // 计算实际需要写入的字节数，不能超过页面大小
	char *temp;

	if (!bytes) // 如果没有字节要写入，直接返回0
		return 0;

	if (size) { // 如果文件有大小限制
		if (offs > size) // 如果偏移已经超出文件大小，返回0
			return 0;
		if (offs + count > size) // 如果写入结束位置超出文件大小，调整count
			count = size - offs;
	}

	temp = memdup_user(userbuf, count); // 从用户空间复制数据到内核空间
	if (IS_ERR(temp)) // 如果复制出错
		return PTR_ERR(temp); // 返回错误

	mutex_lock(&bb->mutex); // 锁定缓冲区

	memcpy(bb->buffer, temp, count); // 将数据从临时缓存复制到bin_buffer的缓存

	count = flush_write(dentry, bb->buffer, offs, count); // 调用flush_write函数写入数据
	mutex_unlock(&bb->mutex); // 解锁缓冲区

	if (count > 0) // 如果写入成功
		*off = offs + count; // 更新文件偏移

	kfree(temp); // 释放临时缓存
	return count; // 返回写入的字节数
}

/*
 * 用于处理虚拟内存区域（VMA）打开操作的函数，它主要用于二进制文件映射在sysfs中的处理。
 * 当VMA被打开时，这个函数会确保相关的vm_operations被调用，这是对映射区域进行特定操作的钩子
 * （如设备映射文件时的特殊处理）。此函数会首先检查是否存在vm_operations及其open方法，
 * 若存在则在获取sysfs目录项的活跃引用后调用该方法，之后释放引用。这样确保了在VMA被操作
 * 时目录项是活跃的，防止了在操作过程中目录项被错误地释放或删除。
 */
static void bin_vma_open(struct vm_area_struct *vma)
{
	struct file *file = vma->vm_file;  // 从虚拟内存区结构体中获取关联的文件结构体
	struct bin_buffer *bb = file->private_data;  // 获取文件的私有数据，即bin_buffer结构体
	struct sysfs_dirent *attr_sd = file->f_path.dentry->d_fsdata;  // 获取与文件关联的sysfs_dirent

	if (!bb->vm_ops || !bb->vm_ops->open)  // 如果没有定义vm_operations或open操作，直接返回
		return;

	if (!sysfs_get_active(attr_sd))  // 尝试获取活跃引用，如果失败，则直接返回
		return;

	bb->vm_ops->open(vma);  // 调用vm_operations中定义的open方法

	sysfs_put_active(attr_sd);  // 释放之前获取的活跃引用
}

// 处理虚拟内存区域（VMA）关闭操作的函数。它主要用于二进制文件在sysfs中的映射处理。当VMA被关闭时，这个函数会确保相关的vm_operations的close方法被调用。
static void bin_vma_close(struct vm_area_struct *vma)
{
	struct file *file = vma->vm_file;  // 从虚拟内存区结构体中获取关联的文件结构体
	struct bin_buffer *bb = file->private_data;  // 获取文件的私有数据，即bin_buffer结构体
	struct sysfs_dirent *attr_sd = file->f_path.dentry->d_fsdata;  // 获取与文件关联的sysfs_dirent

	if (!bb->vm_ops || !bb->vm_ops->close)  // 如果没有定义vm_operations或close操作，直接返回
		return;

	if (!sysfs_get_active(attr_sd))  // 尝试获取活跃引用，如果失败，则直接返回
		return;

	bb->vm_ops->close(vma);  // 调用vm_operations中定义的close方法

	sysfs_put_active(attr_sd);  // 释放之前获取的活跃引用
}

// 处理二进制文件在sysfs中映射时的缺页异常（fault）操作的函数。当虚拟内存区域发生缺页异常时，这个函数会确保相关的vm_operations的fault方法被调用。
static int bin_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct file *file = vma->vm_file;  // 从虚拟内存区结构体中获取关联的文件结构体
	struct bin_buffer *bb = file->private_data;  // 获取文件的私有数据，即bin_buffer结构体
	struct sysfs_dirent *attr_sd = file->f_path.dentry->d_fsdata;  // 获取与文件关联的sysfs_dirent
	int ret;

	if (!bb->vm_ops || !bb->vm_ops->fault)  // 如果没有定义vm_operations或fault操作，返回VM_FAULT_SIGBUS错误
		return VM_FAULT_SIGBUS;

	if (!sysfs_get_active(attr_sd))  // 尝试获取活跃引用，如果失败，返回VM_FAULT_SIGBUS错误
		return VM_FAULT_SIGBUS;

	ret = bb->vm_ops->fault(vma, vmf);  // 调用vm_operations中定义的fault方法

	sysfs_put_active(attr_sd);  // 释放之前获取的活跃引用
	return ret;  // 返回fault方法的返回值
}

/*
 * 处理二进制文件在sysfs中映射时的写入页错误（page_mkwrite）操作的函数。
 * 当虚拟内存区域发生写入页错误时，这个函数会确保相关的vm_operations的page_mkwrite方法被调用。
 */
static int bin_page_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct file *file = vma->vm_file;  // 从虚拟内存区域结构体中获取关联的文件结构体
	struct bin_buffer *bb = file->private_data;  // 获取文件的私有数据，即bin_buffer结构体
	struct sysfs_dirent *attr_sd = file->f_path.dentry->d_fsdata;  // 获取与文件关联的sysfs_dirent
	int ret;

	if (!bb->vm_ops)  // 如果没有定义vm_operations，返回VM_FAULT_SIGBUS错误
		return VM_FAULT_SIGBUS;

	if (!bb->vm_ops->page_mkwrite)  // 如果vm_operations中没有定义page_mkwrite操作，返回0
		return 0;

	if (!sysfs_get_active(attr_sd))  // 尝试获取活跃引用，如果失败，返回VM_FAULT_SIGBUS错误
		return VM_FAULT_SIGBUS;

	ret = bb->vm_ops->page_mkwrite(vma, vmf);  // 调用vm_operations中定义的page_mkwrite方法

	sysfs_put_active(attr_sd);  // 释放之前获取的活跃引用
	return ret;  // 返回page_mkwrite方法的返回值
}

// 定义了一个用于访问sysfs中二进制文件映射区域的函数bin_access
static int bin_access(struct vm_area_struct *vma, unsigned long addr,
		  void *buf, int len, int write)
{
	struct file *file = vma->vm_file;  // 从虚拟内存区域结构体中获取关联的文件结构体
	struct bin_buffer *bb = file->private_data;  // 获取文件的私有数据，即bin_buffer结构体
	struct sysfs_dirent *attr_sd = file->f_path.dentry->d_fsdata;  // 获取与文件关联的sysfs_dirent
	int ret;

	if (!bb->vm_ops || !bb->vm_ops->access)  // 如果没有定义vm_operations或没有定义access方法，返回-EINVAL错误
		return -EINVAL;

	if (!sysfs_get_active(attr_sd))  // 尝试获取活跃引用，如果失败，返回-EINVAL错误
		return -EINVAL;

	ret = bb->vm_ops->access(vma, addr, buf, len, write);  // 调用vm_operations中定义的access方法

	sysfs_put_active(attr_sd);  // 释放之前获取的活跃引用
	return ret;  // 返回access方法的返回值
}

#ifdef CONFIG_NUMA
/*
 * 实现了在NUMA环境下为sysfs文件映射的内存区域设置内存政策的功能。代码首先获取文件对象及其关联的bin_buffer和sysfs_dirent。
 * 如果相关操作（如设置内存政策）被定义，它会尝试执行这些操作并返回结果；如果未定义或者在执行过程中遇到错误，则会适当地返回错误代码。
 */
static int bin_set_policy(struct vm_area_struct *vma, struct mempolicy *new)
{
	struct file *file = vma->vm_file;  // 从VMA结构中获取文件对象
	struct bin_buffer *bb = file->private_data;  // 从文件中获取私有数据，即bin_buffer结构体
	struct sysfs_dirent *attr_sd = file->f_path.dentry->d_fsdata;  // 从文件路径中获取sysfs_dirent对象
	int ret;

	if (!bb->vm_ops || !bb->vm_ops->set_policy)  // 如果vm_ops未设置或set_policy未定义，则直接返回0
		return 0;

	if (!sysfs_get_active(attr_sd))  // 尝试获取sysfs_dirent的活跃引用，如果失败，返回-EINVAL
		return -EINVAL;

	ret = bb->vm_ops->set_policy(vma, new);  // 调用set_policy操作

	sysfs_put_active(attr_sd);  // 释放之前获取的活跃引用
	return ret;  // 返回操作的结果
}

/*
 * 实现了从一个sysfs二进制文件的虚拟内存区域获取内存策略的功能。代码首先验证是否设置了相关的操作，
 * 如果设置了，它将尝试获取并返回具体的内存策略；如果未定义或遇到错误，则返回虚拟内存区域当前的默认内存策略。
 */
static struct mempolicy *bin_get_policy(struct vm_area_struct *vma,
					unsigned long addr)
{
	struct file *file = vma->vm_file;  // 从虚拟内存区域（VMA）获取关联的文件对象
	struct bin_buffer *bb = file->private_data;  // 从文件的私有数据中获取bin_buffer结构
	struct sysfs_dirent *attr_sd = file->f_path.dentry->d_fsdata;  // 从文件的dentry结构中获取sysfs_dirent
	struct mempolicy *pol;  // 用于存放获取到的内存策略

	if (!bb->vm_ops || !bb->vm_ops->get_policy)  // 检查是否有vm_operations或get_policy方法未设置，如果未设置，返回VMA的默认内存策略
		return vma->vm_policy;

	if (!sysfs_get_active(attr_sd))  // 尝试获取sysfs_dirent的活跃引用，如果失败，返回VMA的默认内存策略
		return vma->vm_policy;

	pol = bb->vm_ops->get_policy(vma, addr);  // 调用vm_operations中的get_policy方法获取内存政策

	sysfs_put_active(attr_sd);  // 释放之前获取的活跃引用
	return pol;  // 返回获取到的内存政策
}

// 处理与sysfs二进制文件关联的虚拟内存区域的内存迁移。该函数首先检查必要的操作方法是否存在，然后执行内存迁移，并在操作完成后处理相关的资源。
static int bin_migrate(struct vm_area_struct *vma, const nodemask_t *from,
			const nodemask_t *to, unsigned long flags)
{
	struct file *file = vma->vm_file; // 获取与VMA关联的文件对象
	struct bin_buffer *bb = file->private_data; // 获取文件对象的私有数据，即bin_buffer结构体
	struct sysfs_dirent *attr_sd = file->f_path.dentry->d_fsdata; // 获取与文件相关联的sysfs_dirent结构体
	int ret; // 用于存储操作结果

	if (!bb->vm_ops || !bb->vm_ops->migrate) // 检查是否存在虚拟内存操作结构和迁移方法
		return 0; // 如果没有定义迁移操作，则直接返回0

	if (!sysfs_get_active(attr_sd)) // 尝试增加sysfs_dirent的活跃引用计数，失败则返回0
		return 0;

	ret = bb->vm_ops->migrate(vma, from, to, flags); // 调用migrate操作进行内存迁移

	sysfs_put_active(attr_sd); // 减少之前增加的活跃引用计数
	return ret; // 返回迁移操作的结果
}
#endif

// 定义了一个结构bin_vm_ops，其中包含了一组用于处理与sysfs二进制文件关联的虚拟内存区域操作的函数指针。
static const struct vm_operations_struct bin_vm_ops = {
	.open           = bin_vma_open,           // 当VMA（虚拟内存区域）被打开时调用此函数，用于初始化特定操作。
	.close          = bin_vma_close,          // 当VMA被关闭时调用此函数，用于进行清理操作。
	.fault          = bin_fault,              // 当访问VMA中的页面发生错误时调用，用于加载或修复页面。
	.page_mkwrite   = bin_page_mkwrite,       // 当写操作发生在由VMA映射的页面上时调用，用于处理写保护页面。
	.access         = bin_access,             // 提供对VMA中页面的访问，用于特定的访问请求处理。
#ifdef CONFIG_NUMA
	.set_policy     = bin_set_policy,         // 设置VMA的NUMA策略，影响内存分配行为。
	.get_policy     = bin_get_policy,         // 获取VMA的当前NUMA策略。
	.migrate        = bin_migrate,            // 当需要迁移VMA到其他内存节点时调用，处理NUMA迁移。
#endif
};

/*
 * 处理来自sysfs二进制文件的内存映射请求的函数。它首先从文件的私有数据中获取相关的结构，
 * 检查映射方法是否存在，然后尝试调用该方法。如果成功，它会更新虚拟内存区域的操作以使用
 * bin_vm_ops结构体，这包含了处理内存区域的各种方法。如果在任何步骤中失败，它会清理并返回相应的错误码。
 */
static int mmap(struct file *file, struct vm_area_struct *vma)
{
	struct bin_buffer *bb = file->private_data;  // 从文件中获取二进制缓冲区
	struct sysfs_dirent *attr_sd = file->f_path.dentry->d_fsdata;  // 获取与文件关联的sysfs目录项
	struct bin_attribute *attr = attr_sd->s_bin_attr.bin_attr;  // 获取与目录项关联的二进制属性
	struct kobject *kobj = attr_sd->s_parent->s_dir.kobj;  // 获取目录项的父级kobject
	int rc;  // 返回码

	mutex_lock(&bb->mutex);  // 锁定二进制缓冲区的互斥锁

	/* need attr_sd for attr, its parent for kobj */
	/* 需要attr_sd用于获取属性，它的父级用于获取kobj */
	// 验证目录项是否还有效
	rc = -ENODEV;
	if (!sysfs_get_active(attr_sd))	// 确保目录项仍处于活跃状态
		goto out_unlock;

	// 检查是否有映射方法
	rc = -EINVAL;
	if (!attr->mmap)	// 检查是否存在mmap方法
		goto out_put;

	// 调用映射方法
	rc = attr->mmap(kobj, attr, vma);	// 调用mmap方法
	if (rc)	// mmap调用失败
		goto out_put;

	/*
	 * PowerPC's pci_mmap of legacy_mem uses shmem_zero_setup()
	 * to satisfy versions of X which crash if the mmap fails: that
	 * substitutes a new vm_file, and we don't then want bin_vm_ops.
	 */
	/*
   * PowerPC平台的pci_mmap legacy_mem使用shmem_zero_setup()
   * 满足旧版X崩溃问题，如果mmap失败：
   * 这将替换新的vm_file，我们不再想使用bin_vm_ops。
   */
	// 在PowerPC平台上，避免PCI遗留内存映射中的问题
	if (vma->vm_file != file)	// 确保vm_file没有被替换
		goto out_put;

	// 确保不重复映射
	rc = -EINVAL;
	if (bb->mmapped && bb->vm_ops != vma->vm_ops)	// 确保没有重复映射，并且操作一致
		goto out_put;

	// 设置映射成功
	rc = 0;
	bb->mmapped = 1;
	bb->vm_ops = vma->vm_ops;  // 保存原有的虚拟内存操作
	vma->vm_ops = &bin_vm_ops;  // 设置新的虚拟内存操作

out_put:
	sysfs_put_active(attr_sd);  // 释放目录项的活跃引用
out_unlock:
	mutex_unlock(&bb->mutex);  // 解锁二进制缓冲区的互斥锁

	return rc;  // 返回操作结果
}

// 用于打开sysfs二进制文件的open函数，并确保了必要的权限和资源分配
static int open(struct inode *inode, struct file *file)
{
	struct sysfs_dirent *attr_sd = file->f_path.dentry->d_fsdata; // 从文件路径中获取sysfs目录项
	struct bin_attribute *attr = attr_sd->s_bin_attr.bin_attr; // 从目录项获取对应的二进制属性
	struct bin_buffer *bb = NULL; // 初始化二进制缓冲区指针
	int error; // 错误码变量

	/* binary file operations requires both @sd and its parent */
	/* 二进制文件操作需要sysfs_dirent及其父节点 */
	if (!sysfs_get_active(attr_sd)) // 确保目录项是活跃的
		return -ENODEV; // 如果不活跃，则返回设备不存在错误

	error = -EACCES; // 默认权限错误
	if ((file->f_mode & FMODE_WRITE) && !(attr->write || attr->mmap)) // 检查写权限
		goto err_out; // 如果无写权限且无mmap方法，则跳到错误处理
	if ((file->f_mode & FMODE_READ) && !(attr->read || attr->mmap)) // 检查读权限
		goto err_out; // 如果无读权限且无mmap方法，则跳到错误处理

	error = -ENOMEM; // 默认内存不足错误
	bb = kzalloc(sizeof(*bb), GFP_KERNEL); // 分配并清零一个bin_buffer结构体
	if (!bb) // 检查内存分配是否成功
		goto err_out; // 如果内存分配失败，则跳到错误处理

	bb->buffer = kmalloc(PAGE_SIZE, GFP_KERNEL); // 为buffer分配一页内存
	if (!bb->buffer) // 检查buffer的内存分配
		goto err_out; // 如果内存分配失败，则跳到错误处理

	mutex_init(&bb->mutex); // 初始化二进制缓冲区的互斥锁
	bb->file = file; // 将文件对象存储在缓冲区结构体中
	file->private_data = bb; // 将缓冲区结构体设为文件的私有数据

	mutex_lock(&sysfs_bin_lock); // 加锁全局的二进制锁
	hlist_add_head(&bb->list, &attr_sd->s_bin_attr.buffers); // 将新的buffer添加到目录项的buffer链表中
	mutex_unlock(&sysfs_bin_lock); // 解锁全局的二进制锁

	/* open succeeded, put active references */
	/* 打开操作成功，减少活跃引用计数 */
	sysfs_put_active(attr_sd); // 减少目录项的活跃引用
	return 0; // 返回成功

err_out:
	sysfs_put_active(attr_sd); // 错误处理：减少目录项的活跃引用
	kfree(bb); // 释放bin_buffer结构体
	return error; // 返回错误码
}

// 在关闭sysfs二进制文件时执行的release函数
static int release(struct inode *inode, struct file *file)
{
	struct bin_buffer *bb = file->private_data; // 从文件的私有数据中获取二进制缓冲区指针

	mutex_lock(&sysfs_bin_lock); // 加锁全局的二进制锁
	hlist_del(&bb->list); // 从链表中删除该缓冲区
	mutex_unlock(&sysfs_bin_lock); // 解锁全局的二进制锁

	kfree(bb->buffer); // 释放缓冲区占用的内存
	kfree(bb); // 释放缓冲区结构本身
	return 0; // 返回成功
}

// 提供了用于操作这些文件的文件操作结构bin_fops
const struct file_operations bin_fops = {
	.read       = read,              // 指定read函数，用于读取数据
	.write      = write,             // 指定write函数，用于写入数据
	.mmap       = mmap,              // 指定mmap函数，用于内存映射
	.llseek     = generic_file_llseek, // 使用通用的llseek
	.open       = open,              // 指定open函数，用于打开文件
	.release    = release,           // 指定release函数，用于释放文件
};

// 从内存中取消映射与二进制属性关联的所有文件，通常在文件或对象被删除时调用
void unmap_bin_file(struct sysfs_dirent *attr_sd)
{
	struct bin_buffer *bb;
	struct hlist_node *tmp;

	// 如果sysfs目录项类型不是SYSFS_KOBJ_BIN_ATTR，即不是二进制属性，直接返回
	if (sysfs_type(attr_sd) != SYSFS_KOBJ_BIN_ATTR)
		return;

	// 锁定sysfs二进制文件操作的互斥锁
	mutex_lock(&sysfs_bin_lock);

	// 遍历与该sysfs目录项关联的所有二进制缓冲区
	hlist_for_each_entry(bb, tmp, &attr_sd->s_bin_attr.buffers, list) {
		// 获取文件对应的inode结构
		struct inode *inode = bb->file->f_path.dentry->d_inode;

		// 取消映射文件的全部页面
		unmap_mapping_range(inode->i_mapping, 0, 0, 1);
	}

	// 解锁互斥锁
	mutex_unlock(&sysfs_bin_lock);
}

/**
 *	sysfs_create_bin_file - create binary file for object.
 *	@kobj:	object.
 *	@attr:	attribute descriptor.
 */
/**
 *	sysfs_create_bin_file - 为对象创建二进制文件。
 *	@kobj:	对象。
 *	@attr:	属性描述符。
 */
int sysfs_create_bin_file(struct kobject *kobj,
			  const struct bin_attribute *attr)
{
	BUG_ON(!kobj || !kobj->sd || !attr); // 检查传入的对象、其sysfs目录项或属性是否为空，若为空则内核崩溃。

	return sysfs_add_file(kobj->sd, &attr->attr, SYSFS_KOBJ_BIN_ATTR); // 将二进制属性添加到sysfs，标记为二进制属性类型。
}

/**
 *	sysfs_remove_bin_file - remove binary file for object.
 *	@kobj:	object.
 *	@attr:	attribute descriptor.
 */
/**
 *	sysfs_remove_bin_file - 为对象移除二进制文件。
 *	@kobj:	对象。
 *	@attr:	属性描述符。
 */
void sysfs_remove_bin_file(struct kobject *kobj,
			   const struct bin_attribute *attr)
{
	sysfs_hash_and_remove(kobj->sd, attr->attr.name); // 从sysfs中删除指定名称的二进制文件。
}

EXPORT_SYMBOL_GPL(sysfs_create_bin_file);
EXPORT_SYMBOL_GPL(sysfs_remove_bin_file);
