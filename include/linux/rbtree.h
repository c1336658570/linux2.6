/*
  Red Black Trees
  (C) 1999  Andrea Arcangeli <andrea@suse.de>
  
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

  linux/include/linux/rbtree.h

  To use rbtrees you'll have to implement your own insert and search cores.
  This will avoid us to use callbacks and to drop drammatically performances.
  I know it's not the cleaner way,  but in C (not in C++) to get
  performances and genericity...

  Some example of insert and search follows here. The search is a plain
  normal search over an ordered tree. The insert instead must be implemented
  in two steps: First, the code must insert the element in order as a red leaf
  in the tree, and then the support library function rb_insert_color() must
  be called. Such function will do the not trivial work to rebalance the
  rbtree, if necessary.

-----------------------------------------------------------------------
// 在高速缓存中搜索一个文件区（由一个i节点和一个偏移量共同描述）。每个i节点都有自己的rbtree，以关联在文件中的页偏移
// 该函数搜索给定i节点的rbtree，以寻找匹配的偏移值
static inline struct page * rb_search_page_cache(struct inode * inode,
						 unsigned long offset)
{
	struct rb_node * n = inode->i_rb_page_cache.rb_node;
	struct page * page;

	// 遍历整个rbtree。offset决定向左还是向右遍历。
	while (n)
	{
		page = rb_entry(n, struct page, rb_page_cache);

		if (offset < page->offset)
			n = n->rb_left;
		else if (offset > page->offset)
			n = n->rb_right;
		else
			return page;
	}
	return NULL;
}

// 插入操作，如果页被加入到页高速缓存中，则返回NULL。如果页已经在高速缓存，返回已存在的页结构地址。
static inline struct page * __rb_insert_page_cache(struct inode * inode,
						   unsigned long offset,
						   struct rb_node * node)
{
	struct rb_node ** p = &inode->i_rb_page_cache.rb_node;
	struct rb_node * parent = NULL;
	struct page * page;

	// 遍历整颗树
	while (*p)
	{
		parent = *p;
		page = rb_entry(parent, struct page, rb_page_cache);

		if (offset < page->offset)
			p = &(*p)->rb_left;
		else if (offset > page->offset)
			p = &(*p)->rb_right;
		else
			return page;
	}

	rb_link_node(node, parent, p);

	return NULL;
}

// 插入操作
static inline struct page * rb_insert_page_cache(struct inode * inode,
						 unsigned long offset,
						 struct rb_node * node)
{
	struct page * ret;
	if ((ret = __rb_insert_page_cache(inode, offset, node)))
		goto out;
	rb_insert_color(node, &inode->i_rb_page_cache);
 out:
	return ret;
}
-----------------------------------------------------------------------
*/

#ifndef	_LINUX_RBTREE_H
#define	_LINUX_RBTREE_H

#include <linux/kernel.h>
#include <linux/stddef.h>

// 红黑树节点
struct rb_node
{
	unsigned long  rb_parent_color;	/* 父节点指针和颜色信息，用最低位表示颜色 */
/* 定义红色和黑色的常量 */
#define	RB_RED		0	/* 红色节点 */
#define	RB_BLACK	1	/* 黑色节点 */
	struct rb_node *rb_right;	/* 指向右子节点的指针 */
	struct rb_node *rb_left;	/* 指向左子节点的指针 */
} __attribute__((aligned(sizeof(long))));
    /* The alignment might seem pointless, but allegedly CRIS needs it */
		/* 这个对齐可能看起来没必要，但据称CRIS架构需要它 */

// 红黑树根节点
struct rb_root
{
	struct rb_node *rb_node;	/* 指向树的根节点 */
};

/* 获取节点的父节点，从rb_parent_color字段中提取父节点地址，忽略最低两位（用于颜色和对齐） */
#define rb_parent(r)   ((struct rb_node *)((r)->rb_parent_color & ~3))
/* 获取节点的颜色，从rb_parent_color字段的最低位获取颜色信息 */
#define rb_color(r)   ((r)->rb_parent_color & 1)
/* 判断节点是否为红色，如果颜色位为0（红色），则返回真（true） */
#define rb_is_red(r)   (!rb_color(r))
/* 判断节点是否为黑色，如果颜色位为1（黑色），则返回真（true） */
#define rb_is_black(r) rb_color(r)
/* 将节点设置为红色，将rb_parent_color字段的最低位清零，表示红色 */
#define rb_set_red(r)  do { (r)->rb_parent_color &= ~1; } while (0)
/* 将节点设置为黑色，将rb_parent_color字段的最低位设置为1，表示黑色 */
#define rb_set_black(r)  do { (r)->rb_parent_color |= 1; } while (0)

/* 设置节点的父节点 */
static inline void rb_set_parent(struct rb_node *rb, struct rb_node *p)
{
	// 保留rb_parent_color字段的最低两位（颜色和对齐信息），将其余位设置为父节点的地址
	rb->rb_parent_color = (rb->rb_parent_color & 3) | (unsigned long)p;
}
/* 设置节点的颜色 */
static inline void rb_set_color(struct rb_node *rb, int color)
{
	// 保留rb_parent_color字段的除最低位外的所有位，设置最低位为指定的颜色
	rb->rb_parent_color = (rb->rb_parent_color & ~1) | color;
}

// 定义一个临时的节点，节点内的指针指向空
#define RB_ROOT	(struct rb_root) { NULL, }	/* 定义一个空的树根结构体，其根节点指针初始化为NULL */
/* 从给定成员指针获取包含它的结构体的指针，container_of宏通常用于根据结构体中成员的地址反推出结构体的地址 */
#define	rb_entry(ptr, type, member) container_of(ptr, type, member)

/* 判断红黑树根是否为空，如果树的根节点指针为NULL，则该树为空 */
#define RB_EMPTY_ROOT(root)	((root)->rb_node == NULL)
/* 判断红黑树节点是否处于未链接状态，如果节点的父节点指针指向自身，则节点未链接到任何树上 */
#define RB_EMPTY_NODE(node)	(rb_parent(node) == node)
/* 将节点设置为未链接状态，将节点的父节点指针设置为指向自身，表示该节点不在树上 */
#define RB_CLEAR_NODE(node)	(rb_set_parent(node, node))

/* 外部函数声明，用于将节点插入红黑树并调整颜色，插入节点后进行颜色调整以维持红黑树性质 */
extern void rb_insert_color(struct rb_node *, struct rb_root *);
/* 外部函数声明，用于从红黑树中移除节点，移除节点并调整树以维持红黑树性质 */
extern void rb_erase(struct rb_node *, struct rb_root *);

/* Find logical next and previous nodes in a tree */
/* 寻找树中逻辑上的下一个和前一个节点 */
/* 这些函数用于在树中按照中序遍历的顺序找到下一个或前一个节点 */
extern struct rb_node *rb_next(const struct rb_node *);
extern struct rb_node *rb_prev(const struct rb_node *);
/* 寻找整棵树的第一个和最后一个节点 */
/* 这些函数分别返回树中最小和最大的节点 */
extern struct rb_node *rb_first(const struct rb_root *);
extern struct rb_node *rb_last(const struct rb_root *);

/* Fast replacement of a single node without remove/rebalance/add/rebalance */
/* 快速替换单个节点，无需进行移除、平衡、添加、再平衡的操作，直接替换树中的节点，而不改变树的其他部分 */
extern void rb_replace_node(struct rb_node *victim, struct rb_node *new, 
			    struct rb_root *root);

/* 链接节点到红黑树的某个位置，将新节点链接到树中指定的父节点和位置。节点的左右子节点初始化为NULL，并设置父节点 */
static inline void rb_link_node(struct rb_node * node, struct rb_node * parent,
				struct rb_node ** rb_link)
{
	node->rb_parent_color = (unsigned long )parent;	/* 将新节点的父节点设置为parent，并暂时不设置颜色 */
	node->rb_left = node->rb_right = NULL;	/* 初始化新节点的左右子节点为空 */

	*rb_link = node;	/* 通过rb_link参数更新父节点的相应子节点指针，使其指向新的节点 */
}

#endif	/* _LINUX_RBTREE_H */
