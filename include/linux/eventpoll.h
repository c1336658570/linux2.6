/*
 *  include/linux/eventpoll.h ( Efficent event polling implementation )
 *  Copyright (C) 2001,...,2006	 Davide Libenzi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Davide Libenzi <davidel@xmailserver.org>
 *
 */

#ifndef _LINUX_EVENTPOLL_H
#define _LINUX_EVENTPOLL_H

/* For O_CLOEXEC */
#include <linux/fcntl.h>
#include <linux/types.h>

/* Flags for epoll_create1.  */
/* epoll_create1函数的标志位。*/
#define EPOLL_CLOEXEC O_CLOEXEC	// 在执行exec()调用的程序替换时关闭文件描述符

/* Valid opcodes to issue to sys_epoll_ctl() */
/* 可以传递给sys_epoll_ctl()的有效操作码 */
#define EPOLL_CTL_ADD 1          // 添加一个文件描述符到epoll监听中
#define EPOLL_CTL_DEL 2          // 从epoll监听中删除一个文件描述符
#define EPOLL_CTL_MOD 3          // 修改一个已注册的文件描述符的监听事件

/* Set the One Shot behaviour for the target file descriptor */
/* 设置目标文件描述符的一次性行为 */
#define EPOLLONESHOT (1 << 30)	// 事件只会被触发一次，之后需重新设置

/* Set the Edge Triggered behaviour for the target file descriptor */
/* 设置目标文件描述符的边缘触发行为 */
#define EPOLLET (1 << 31)	// 事件在状态变化时被触发，适用于非阻塞I/O

/* 
 * On x86-64 make the 64bit structure have the same alignment as the
 * 32bit structure. This makes 32bit emulation easier.
 *
 * UML/x86_64 needs the same packing as x86_64
 */
/*
 * 在x86-64架构上，使64位结构与32位结构具有相同的对齐方式。
 * 这使得32位仿真更加容易。
 *
 * UML/x86_64需要与x86_64相同的打包方式
 */
#ifdef __x86_64__	// 如果是x86_64架构
#define EPOLL_PACKED __attribute__((packed))	// 定义EPOLL_PACKED为packed属性，指示编译器使用紧凑的内存布局
#else	// 如果不是x86_64架构
#define EPOLL_PACKED	// EPOLL_PACKED为空定义，不影响结构的默认对齐
#endif

struct epoll_event {        // 定义epoll事件结构
	__u32 events;           // 事件类型，使用32位无符号整数表示
	__u64 data;             // 与事件关联的用户数据，使用64位无符号整数表示
} EPOLL_PACKED;             // 应用EPOLL_PACKED属性，保证结构在所有平台上具有一致的内存布局

#ifdef __KERNEL__	// 如果这是内核代码编译环境

/* Forward declarations to avoid compiler errors */
/* 提前声明以避免编译器错误 */
struct file;	// 声明一个结构体，代表打开的文件或设备


#ifdef CONFIG_EPOLL	// 如果配置了EPOLL支持

/* Used to initialize the epoll bits inside the "struct file" */
/* 用于初始化“struct file”中的epoll相关部分 */
static inline void eventpoll_init_file(struct file *file)
{
	INIT_LIST_HEAD(&file->f_ep_links);	// 初始化file结构体中的f_ep_links链表头
}


/* Used to release the epoll bits inside the "struct file" */
/* 用于释放“struct file”中的epoll相关部分 */
void eventpoll_release_file(struct file *file);

/*
 * This is called from inside fs/file_table.c:__fput() to unlink files
 * from the eventpoll interface. We need to have this facility to cleanup
 * correctly files that are closed without being removed from the eventpoll
 * interface.
 */
/* 
 * 从fs/file_table.c:__fput()中调用此函数，以从事件轮询接口中取消链接文件。
 * 我们需要此功能来正确清理没有从事件轮询接口中移除就关闭的文件。
 */
static inline void eventpoll_release(struct file *file)
{

	/*
	 * Fast check to avoid the get/release of the semaphore. Since
	 * we're doing this outside the semaphore lock, it might return
	 * false negatives, but we don't care. It'll help in 99.99% of cases
	 * to avoid the semaphore lock. False positives simply cannot happen
	 * because the file in on the way to be removed and nobody ( but
	 * eventpoll ) has still a reference to this file.
	 */
	/* 
	 * 快速检查以避免获取/释放信号量。由于我们在信号量锁外执行此操作，
	 * 它可能返回假阴性，但我们不在乎。这将在99.99%的情况下帮助避免信号量锁。
	 * 假阳性根本不会发生，因为文件正在被移除的途中，没有人（除了eventpoll）还持有对此文件的引用。
	 */
	if (likely(list_empty(&file->f_ep_links)))	// 如果关联列表为空，则直接返回
		return;

	/*
	 * The file is being closed while it is still linked to an epoll
	 * descriptor. We need to handle this by correctly unlinking it
	 * from its containers.
	 */
	/* 
	 * 文件在仍然链接到epoll描述符的情况下被关闭。
	 * 我们需要通过正确地将其从其容器中取消链接来处理此问题。
	 */
	eventpoll_release_file(file);	// 调用释放函数处理文件关闭时的epoll链接
}

#else

static inline void eventpoll_init_file(struct file *file) {}	// 如果没有定义CONFIG_EPOLL，提供空的初始化函数
static inline void eventpoll_release(struct file *file) {}		// 如果没有定义CONFIG_EPOLL，提供空的释放函数

#endif

#endif /* #ifdef __KERNEL__ */

#endif /* #ifndef _LINUX_EVENTPOLL_H */

