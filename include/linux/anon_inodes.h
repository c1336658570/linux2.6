/*
 *  include/linux/anon_inodes.h
 *
 *  Copyright (C) 2007  Davide Libenzi <davidel@xmailserver.org>
 *
 */

#ifndef _LINUX_ANON_INODES_H
#define _LINUX_ANON_INODES_H

// 定义一个函数，用于创建一个不与任何具体文件系统文件路径关联的匿名inode文件对象
// name: 用于识别的名称，主要用于调试
// fops: 文件操作指针，定义了文件的行为
// priv: 传递给文件操作的私有数据
// flags: 创建文件时使用的标志位
struct file *anon_inode_getfile(const char *name,
				const struct file_operations *fops,
				void *priv, int flags);
// 定义一个函数，用于获取一个与匿名inode关联的文件描述符
// name: 用于识别的名称，主要用于调试
// fops: 文件操作指针，定义了文件的行为
// priv: 传递给文件操作的私有数据
// flags: 创建文件时使用的标志位
int anon_inode_getfd(const char *name, const struct file_operations *fops,
		     void *priv, int flags);

#endif /* _LINUX_ANON_INODES_H */

