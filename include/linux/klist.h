/*
 *	klist.h - Some generic list helpers, extending struct list_head a bit.
 *
 *	Implementations are found in lib/klist.c
 *
 *
 *	Copyright (C) 2005 Patrick Mochel
 *
 *	This file is rleased under the GPL v2.
 */

#ifndef _LINUX_KLIST_H
#define _LINUX_KLIST_H

#include <linux/spinlock.h>
#include <linux/kref.h>
#include <linux/list.h>

struct klist_node;
/* klist 结构定义 */
struct klist {
	spinlock_t		k_lock;		/* 自旋锁，保护链表操作 */
	struct list_head	k_list;		/* 实际存储链表节点的头节点 */
	void			(*get)(struct klist_node *); /* 增加节点引用的函数指针 */
	void			(*put)(struct klist_node *); /* 减少节点引用的函数指针 */
} __attribute__ ((aligned (4)));  /* 确保结构体对齐 */

/* 初始化一个 klist */
#define KLIST_INIT(_name, _get, _put)					\
	{ .k_lock	= __SPIN_LOCK_UNLOCKED(_name.k_lock),		\
	  .k_list	= LIST_HEAD_INIT(_name.k_list),			\
	  .get		= _get,						\
	  .put		= _put, }

/* 便捷宏定义，用于定义并初始化 klist */
#define DEFINE_KLIST(_name, _get, _put)					\
	struct klist _name = KLIST_INIT(_name, _get, _put)

/* 初始化 klist 结构 */
extern void klist_init(struct klist *k, void (*get)(struct klist_node *),
		       void (*put)(struct klist_node *));

/* klist 节点结构定义 */
struct klist_node {
	void			*n_klist;	/* 指向所属 klist 的指针，不直接访问 */
	struct list_head	n_node;		/* 链表节点 */
	struct kref		n_ref;		/* 引用计数 */
};

/* 向 klist 添加节点的函数 */
extern void klist_add_tail(struct klist_node *n, struct klist *k);
extern void klist_add_head(struct klist_node *n, struct klist *k);
extern void klist_add_after(struct klist_node *n, struct klist_node *pos);
extern void klist_add_before(struct klist_node *n, struct klist_node *pos);

/* 从 klist 删除节点的函数 */
extern void klist_del(struct klist_node *n);
extern void klist_remove(struct klist_node *n);	/* 删除并释放资源 */

/* 检查节点是否已经附加到 klist */
extern int klist_node_attached(struct klist_node *n);

/* klist 迭代器结构定义 */
struct klist_iter {
	struct klist		*i_klist; /* 指向迭代的 klist */
	struct klist_node	*i_cur;   /* 当前迭代到的节点 */
};

/* klist 迭代器相关函数 */
extern void klist_iter_init(struct klist *k, struct klist_iter *i);
extern void klist_iter_init_node(struct klist *k, struct klist_iter *i,
				 struct klist_node *n);
extern void klist_iter_exit(struct klist_iter *i);	/* 结束迭代，释放资源 */
extern struct klist_node *klist_next(struct klist_iter *i);	/* 获取下一个节点 */

#endif
