/*
 * kref.c - library routines for handling generic reference counted objects
 *
 * Copyright (C) 2004 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (C) 2004 IBM Corp.
 *
 * based on kobject.h which was:
 * Copyright (C) 2002-2003 Patrick Mochel <mochel@osdl.org>
 * Copyright (C) 2002-2003 Open Source Development Labs
 *
 * This file is released under the GPLv2.
 *
 */

#ifndef _KREF_H_
#define _KREF_H_

#include <linux/types.h>

/* kref 结构体用于引用计数 */
struct kref {
	atomic_t refcount;	// 使用原子操作的引用计数
};

/* 设置 kref 的引用计数 */
void kref_set(struct kref *kref, int num);
/* 初始化 kref 结构，设置引用计数为 1 */
void kref_init(struct kref *kref);
/* 增加 kref 的引用计数 */
void kref_get(struct kref *kref);
/* 减少 kref 的引用计数，并在引用计数为 0 时调用释放函数 */
int kref_put(struct kref *kref, void (*release) (struct kref *kref));

#endif /* _KREF_H_ */
