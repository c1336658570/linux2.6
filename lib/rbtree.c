/*
  Red Black Trees
  (C) 1999  Andrea Arcangeli <andrea@suse.de>
  (C) 2002  David Woodhouse <dwmw2@infradead.org>
  
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

  linux/lib/rbtree.c
*/

#include <linux/rbtree.h>
#include <linux/module.h>

/* 静态函数 __rb_rotate_left 用于对指定节点执行左旋操作 */
static void __rb_rotate_left(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *right = node->rb_right;			/* 保存节点右子节点 */
	struct rb_node *parent = rb_parent(node);		/* 获取节点的父节点 */

	if ((node->rb_right = right->rb_left))			/* 如果右子节点的左子节点存在，将当前节点的右子节点指向右子节点的左子节点 */
		rb_set_parent(right->rb_left, node);			/* 将右子节点的左子节点的父指针设为当前节点 */
	right->rb_left = node;	/* 将右子节点的左子指针指向当前节点 */

	rb_set_parent(right, parent);		/* 将右子节点的父指针设为当前节点的父节点 */

	if (parent)	/* 如果父节点存在 */
	{
		if (node == parent->rb_left)	/* 如果当前节点是父节点的左子节点 */
			parent->rb_left = right;		/* 将父节点的左子指针指向右子节点 */
		else
			parent->rb_right = right;		/* 否则将父节点的右子指针指向右子节点 */
	}
	else
		root->rb_node = right;				/* 如果当前节点是根节点，更新根节点指针 */
	rb_set_parent(node, right);			/* 将当前节点的父指针设为右子节点 */
}

/* 静态函数 __rb_rotate_right 用于对指定节点执行右旋操作 */
static void __rb_rotate_right(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *left = node->rb_left;				/* 保存节点左子节点 */
	struct rb_node *parent = rb_parent(node);		/* 获取节点的父节点 */

	if ((node->rb_left = left->rb_right))				/* 如果左子节点的右子节点存在 */
		rb_set_parent(left->rb_right, node);			/* 将左子节点的右子节点的父指针设为当前节点 */
	left->rb_right = node;		/* 将左子节点的右子指针指向当前节点 */

	rb_set_parent(left, parent);		/* 将左子节点的父指针设为当前节点的父节点 */

	if (parent)		/* 如果父节点存在 */
	{
		if (node == parent->rb_right)		/* 如果当前节点是父节点的右子节点 */
			parent->rb_right = left;			/* 将父节点的右子指针指向左子节点 */
		else
			parent->rb_left = left;				/* 否则将父节点的左子指针指向左子节点 */
	}
	else
		root->rb_node = left;						/* 如果当前节点是根节点，更新根节点指针 */
	rb_set_parent(node, left);				/* 将当前节点的父指针设为左子节点 */
}

/* `rb_insert_color` 函数用于在红黑树中插入一个新节点后调整树，以维持红黑树的性质 */
void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *parent, *gparent;

	// 持续检查和调整新插入节点的颜色，直到不再需要调整或达到树的根部
	/* 遍历从当前节点到根节点，只要父节点是红色，就需要进行调整 */
	// 循环检查新插入的节点的父节点的颜色，只有当父节点为红色时才需要调整
	while ((parent = rb_parent(node)) && rb_is_red(parent))	// 父节点不为空且为红色时需要处理
	{
		gparent = rb_parent(parent);	// 获取祖父节点，因为可能需要调整

		/* 判断父节点是祖父节点的左子节点还是右子节点 */
		if (parent == gparent->rb_left)	/* 父节点是祖父节点的左子节点的情况 */
		{
			{
				/* 获取叔叔节点，即祖父节点的右子节点 */
				register struct rb_node *uncle = gparent->rb_right;
				// 如果叔叔节点存在且为红色，需要进行颜色翻转
				if (uncle && rb_is_red(uncle))	/* 如果叔叔节点存在且为红色 */
				{
					/* 父节点和叔叔节点均为红色，按红黑树规则，转变它们为黑色，祖父节点为红色 */
					rb_set_black(uncle); /* 将叔叔节点设为黑色 */
					rb_set_black(parent); /* 将父节点设为黑色 */
					rb_set_red(gparent); /* 将祖父节点设为红色 */
					/* 调整后，继续向上检查祖父节点 */
					node = gparent; /* 将当前节点上移至祖父节点，继续向上调整 */
					continue;	/* 继续循环 */
				}
			}

			/* 处理父节点是左子节点，且当前节点是父节点的右子节点的情况（需要进行两次旋转） */
			// 如果当前节点是父节点的右子节点，进行左旋
			if (parent->rb_right == node)
			{
				/* 先左旋父节点，然后当前节点和父节点的角色互换，为下一步右旋做准备 */
				register struct rb_node *tmp;
				__rb_rotate_left(parent, root);	// 对父节点进行左旋
				tmp = parent;	// 旋转后，交换父节点和当前节点的角色，以保持正确的父子关系
				parent = node;
				node = tmp;
			}

			/* 对祖父节点进行右旋，完成调整，更新父节点和祖父节点的颜色 */
			// 调整完成后，设置正确的颜色，并对祖父节点进行右旋
			rb_set_black(parent);		/* 父节点设置为黑色 */
			rb_set_red(gparent);		/* 祖父节点设置为红色 */
			__rb_rotate_right(gparent, root);	// 对祖父节点进行右旋，以修复红黑树的性质
		} else {	/* 父节点是祖父节点的右子节点的情况 */
			{
				/* 获取叔叔节点，即祖父节点的左子节点 */
				register struct rb_node *uncle = gparent->rb_left;
				if (uncle && rb_is_red(uncle))	/* 如果叔叔节点存在且为红色 */
				{
					/* 父节点和叔叔节点均为红色，转变它们为黑色，祖父节点为红色 */
					rb_set_black(uncle);			/* 将叔叔节点设为黑色 */
					rb_set_black(parent);			/* 将父节点设为黑色 */
					rb_set_red(gparent);			/* 将祖父节点设为红色 */
					/* 继续向上检查祖父节点 */
					node = gparent;						/* 将当前节点上移至祖父节点，继续向上调整 */
					continue;		/* 继续while循环，进一步向上调整 */
				}
			}

			/* 处理父节点是右子节点，且当前节点是父节点的左子节点的情况（需要进行两次旋转） */
			// 如果当前节点是父节点的左子节点，进行右旋
			if (parent->rb_left == node)
			{
				/* 先右旋父节点，然后当前节点和父节点的角色互换，为下一步左旋做准备 */
				register struct rb_node *tmp;
				__rb_rotate_right(parent, root);	/* 右旋父节点 */
				tmp = parent;		/* 旋转后，交换父节点和当前节点的角色，以保持正确的父子关系 */
				parent = node;
				node = tmp;
			}

			/* 对祖父节点进行左旋，完成调整，更新父节点和祖父节点的颜色 */
			/* 调整完成后，设置正确的颜色，并对祖父节点进行左旋 */
			rb_set_black(parent);	/* 父节点设置为黑色 */
			rb_set_red(gparent);	/* 祖父节点设置为红色 */
			__rb_rotate_left(gparent, root);	/* 对祖父节点进行左旋，以修复红黑树的性质 */
		}
	}

	/* 确保根节点总是黑色 */
	rb_set_black(root->rb_node);	// 保证根节点始终为黑色
}
EXPORT_SYMBOL(rb_insert_color);

/* 在红黑树删除节点后调整颜色和结构以保持红黑树的性质 */
static void __rb_erase_color(struct rb_node *node, struct rb_node *parent,
			     struct rb_root *root)
{
	struct rb_node *other;	// 用来引用兄弟节点

	/* 循环条件：node为空或为黑色，并且node不是根节点 */
	while ((!node || rb_is_black(node)) && node != root->rb_node)	// 循环直到node为红或直到node为根节点
	{
		if (parent->rb_left == node)	// 如果node是其父节点的左子节点
		{
			other = parent->rb_right;	// other指向node的兄弟节点，即parent的右子节点
			if (rb_is_red(other))	// 如果兄弟节点是红色
			{
				rb_set_black(other);	// 设置兄弟节点为黑色
				rb_set_red(parent);		// 设置父节点为红色
				__rb_rotate_left(parent, root);	// 对父节点进行左旋
				other = parent->rb_right;	// 更新兄弟节点的引用，因为树结构已改变
			}
			/* 检查兄弟节点的子节点颜色，两个子节点都必须是黑色 */
			if ((!other->rb_left || rb_is_black(other->rb_left)) &&
			    (!other->rb_right || rb_is_black(other->rb_right)))	// 如果兄弟节点的两个子节点都是黑色
			{
				rb_set_red(other);	// 设置兄弟节点为红色
				// 将需要重新检查的节点上移至父节点
				// 将node上移至父节点，以继续向上调整
				node = parent;			// 将当前节点上移至父节点
				parent = rb_parent(node);	// 更新父节点
			}
			else	// 兄弟节点至少有一个红色子节点
			{
				if (!other->rb_right || rb_is_black(other->rb_right))	// 如果兄弟节点的右子节点是黑色
				{
					rb_set_black(other->rb_left);	// 设置兄弟节点的左子节点为黑色
					rb_set_red(other);	// 设置兄弟节点为红色
					__rb_rotate_right(other, root);	// 对兄弟节点进行右旋
					other = parent->rb_right;	// 更新兄弟节点
				}
				rb_set_color(other, rb_color(parent));	// 将兄弟节点设置为父节点的颜色
				rb_set_black(parent);	// 设置父节点为黑色
				rb_set_black(other->rb_right);	// 设置兄弟节点的右子节点为黑色
				__rb_rotate_left(parent, root);	// 对父节点进行左旋
				node = root->rb_node;	// 将当前节点设为根节点，结束循环
				break;
			}
		}
		else	// 如果node是其父节点的右子节点
		{
			other = parent->rb_left;	// other引用node的兄弟节点，此时是父节点的左子节点
			if (rb_is_red(other))			// 如果兄弟节点是红色
			{
				rb_set_black(other);		// 设置兄弟节点为黑色
				rb_set_red(parent);			// 设置父节点为红色
				__rb_rotate_right(parent, root);	// 对父节点进行右旋
				other = parent->rb_left;		// 更新兄弟节点引用，因为旋转改变了树的结构
			}
			/* 检查兄弟节点的子节点颜色，两个子节点都必须是黑色 */
			if ((!other->rb_left || rb_is_black(other->rb_left)) &&
			    (!other->rb_right || rb_is_black(other->rb_right)))	// 如果兄弟节点的两个子节点都是黑色
			{
				rb_set_red(other);	// 设置兄弟节点为红色
				// 将node上移至父节点，继续向上调整
				node = parent;	// 将当前节点上移至父节点
				parent = rb_parent(node);		// 更新父节点
			}
			else	// 兄弟节点至少有一个红色子节点
			{
				if (!other->rb_left || rb_is_black(other->rb_left))	// 如果兄弟节点的左子节点是黑色
				{
					rb_set_black(other->rb_right);	// 设置兄弟节点的右子节点为黑色
					rb_set_red(other);	// 设置兄弟节点为红色
					__rb_rotate_left(other, root);	// 对兄弟节点进行左旋
					other = parent->rb_left;		// 更新兄弟节点
				}
				rb_set_color(other, rb_color(parent));	// 将兄弟节点设置为父节点的颜色
				rb_set_black(parent);	// 设置父节点为黑色
				rb_set_black(other->rb_left);	// 设置兄弟节点的左子节点为黑色
				__rb_rotate_right(parent, root);	// 对父节点进行右旋
				node = root->rb_node;	// 将当前节点设为根节点，结束循环
				break;
			}
		}
	}
	if (node)	// 如果node非空
		rb_set_black(node);	// 最后确保当前节点是黑色
}

/* 从红黑树中删除指定节点 */
void rb_erase(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *child, *parent;
	int color;

	/* 判断节点的子节点情况，以便确定如何调整树 */
	if (!node->rb_left)	// 如果节点没有左子节点
		child = node->rb_right;	// 直接使用右子节点作为替代节点
	else if (!node->rb_right)	// 如果节点没有右子节点
		child = node->rb_left;	// 使用左子节点作为替代节点
	else	// 如果节点同时拥有左右子节点
	{
		/* 处理节点既有左子节点也有右子节点的情况 */
		struct rb_node *old = node, *left;

		// 将node指向其右子节点
		node = node->rb_right;	// 找到删除节点的后继，即右子树中的最小节点
		// 循环直到找到最左边的节点
		while ((left = node->rb_left) != NULL)	// 寻找node的最小后继节点
			node = left;	// 最小后继节点总是在左侧最底部

		/* 替换节点的位置，将node（后继节点）放置到待删除节点的位置 */
		if (rb_parent(old)) {	// 如果待删除节点有父节点
			if (rb_parent(old)->rb_left == old)
				rb_parent(old)->rb_left = node;	// 如果待删除节点是左子节点，更新父节点的左子指向
			else
				rb_parent(old)->rb_right = node;	// 如果待删除节点是右子节点，更新父节点的右子指向
		} else
			// 如果待删除的节点是根节点，更新根节点为node
			root->rb_node = node;	// 如果删除的是根节点，则新的根节点是后继节点

		/* 准备移除old节点，调整其它节点的指向 */
		// node的右子节点成为新的孩子节点，因为node即将被移至old的位置，所以node的右子节点替代node原来的位置，移动到node的父节点的子结点上
		child = node->rb_right;	// 后继节点的右子节点变成新的child
		parent = rb_parent(node);	// 后继节点（node）的父节点
		color = rb_color(node);	// 获取后继节点（node）的颜色，以确定后续是否需要调整

		// 如果后继节点的父节点就是待删除节点，更新parent指向
		if (parent == old) {	// 如果后继节点是待删除节点的直接子节点
			parent = node;	// 如果后继节点的父节点是待删除节点，更新父节点引用为后继节点
		} else {
			if (child)	// 如果后继节点有子节点，更新该子节点的父指针
				// 设置child的新父节点
				rb_set_parent(child, parent);	// 设置后继节点的右子节点的父节点
			// 设置原后继节点的父节点的左子节点指向后继的右子节点，将child放在原后继节点的位置
			parent->rb_left = child;				// 将后继节点原本位置的子节点连接到后继节点的父节点

			// 将待删除节点的右子树给后继节点
			node->rb_right = old->rb_right;	// 将待删除节点的右子节点设置为后继节点的右子节点
			// 更新原右子树的父指针为后继节点
			rb_set_parent(old->rb_right, node);	// 更新父节点信息
		}
		
		node->rb_parent_color = old->rb_parent_color;	// 将待删除节点的父节点信息赋给后继节点
		node->rb_left = old->rb_left;		// 后继节点接管待删除节点的左子树
		rb_set_parent(old->rb_left, node);	// 更新左子树的父指针为后继节点

		goto color;	// 跳转到颜色调整部分
	}

	/* 处理节点只有一个子节点或没有子节点的简单情况 */
	parent = rb_parent(node);	// 获取待删除节点的父节点
	color = rb_color(node);		// 获取待删除节点的颜色

	if (child)
		rb_set_parent(child, parent);	// 如果child存在，更新其父节点
	if (parent)	
	{
		if (parent->rb_left == node)
			// 如果待删除节点是父节点的左子节点，父节点的左指针指向新的子节点
			parent->rb_left = child;	// 断开node，连接child
		else
			// 同上，处理右子节点
			parent->rb_right = child;
	}
	else
		root->rb_node = child;	// 如果删除的是根节点，则更新根节点

 color:	// 标签：用于处理可能需要的颜色调整
	if (color == RB_BLACK)	// 如果原节点是黑色，需要调整以保持红黑树性质
		__rb_erase_color(child, parent, root);	// 调用__rb_erase_color进行进一步处理
}
EXPORT_SYMBOL(rb_erase);

/*
 * This function returns the first node (in sort order) of the tree.
 */
/*
 * 该函数返回树中的第一个节点（按排序顺序）。
 */
struct rb_node *rb_first(const struct rb_root *root)
{
	struct rb_node	*n;

	n = root->rb_node;	// n指向红黑树的根节点
	if (!n)
		return NULL;	// 如果根节点不存在，即树为空，返回NULL
	while (n->rb_left)	// 循环直到找到最左侧的节点
		n = n->rb_left;		// 不断向左移动，因为在二叉搜索树中最小的元素位于最左侧
	return n;				// 返回最左侧的节点，即树中的最小节点
}
EXPORT_SYMBOL(rb_first);

/*
 * 该函数返回树中的最后一个节点（按排序顺序）。
 */
struct rb_node *rb_last(const struct rb_root *root)
{
	struct rb_node	*n;

	n = root->rb_node;	// n指向红黑树的根节点
	if (!n)
		return NULL;			// 如果根节点不存在，即树为空，返回NULL
	while (n->rb_right)	// 循环直到找到最右侧的节点
		n = n->rb_right;	// 不断向右移动，因为在二叉搜索树中最大的元素位于最右侧
	return n;						// 返回最右侧的节点，即树中的最大节点
}
EXPORT_SYMBOL(rb_last);

/*
 * 此函数返回给定节点的下一个节点（按排序顺序）。
 */
struct rb_node *rb_next(const struct rb_node *node)
{
	struct rb_node *parent;

	/* 如果节点的父节点是它自己，则返回 NULL */
	if (rb_parent(node) == node)
		return NULL;		// 这通常是一个错误检查，防止循环引用

	/* If we have a right-hand child, go down and then left as far
	   as we can. */
	/* 如果存在右子节点，那么下一个节点就是右子树中的最小节点 */
	if (node->rb_right) {	// 如果节点有右子节点
		node = node->rb_right;	// 移动到右子节点
		while (node->rb_left)		// 一直找到最左边的节点，即这棵子树中的最小值
			node=node->rb_left;
		return (struct rb_node *)node;	// 返回找到的最小节点
	}

	/* No right-hand children.  Everything down and left is
	   smaller than us, so any 'next' node must be in the general
	   direction of our parent. Go up the tree; any time the
	   ancestor is a right-hand child of its parent, keep going
	   up. First time it's a left-hand child of its parent, said
	   parent is our 'next' node. */
	/* 没有右子节点，需要向上查找 */
	/* 当前节点没有右子节点。向下和左边的所有内容都小于我们，
		 所以任何“下一个”节点必须在我们父节点的方向上。向树上走；
		 任何时候祖先是其父节点的右子节点，继续向上。
		 第一次它是父节点的左子节点，那个父节点就是我们的“下一个”节点。 */
	while ((parent = rb_parent(node)) && node == parent->rb_right)
		node = parent;	// 沿父节点向上移动，直到找到一个是其父节点左子节点的节点

	return parent;		// 返回找到的父节点，它是当前节点的下一个节点
}
EXPORT_SYMBOL(rb_next);

/*
 * 此函数返回给定节点的前一个节点（按排序顺序）。
 */
struct rb_node *rb_prev(const struct rb_node *node)
{
	struct rb_node *parent;

	/* 如果节点的父节点是它自己，返回 NULL，防止循环引用 */
	if (rb_parent(node) == node)
		return NULL;

	/* If we have a left-hand child, go down and then right as far
	   as we can. */
	/* 如果我们有一个左侧子节点，向下走然后尽可能向右走 */
	if (node->rb_left) {	// 如果节点有左子节点
		node = node->rb_left; 	// 移动到左子节点
		while (node->rb_right)	// 一直找到最右边的节点，即这棵子树中的最大值
			node=node->rb_right;
		return (struct rb_node *)node;	// 返回找到的最大节点
	}

	/* No left-hand children. Go up till we find an ancestor which
	   is a right-hand child of its parent */
	/* 没有左侧子节点，向上走直到我们找到一个祖先是其父节点的右侧子节点 */
	/* 当前节点没有左子节点。为了找到前一个节点，我们需要向上移动，直到找到一个是其父节点右子节点的节点 */
	while ((parent = rb_parent(node)) && node == parent->rb_left)
		node = parent;	// 沿父节点向上移动，直到找到一个是其父节点右子节点的节点

	return parent;	// 返回找到的父节点，它是当前节点的前一个节点
}
EXPORT_SYMBOL(rb_prev);

/*
 * 该函数用于替换红黑树中的一个节点，将“victim”节点替换为“new”节点。
 */
void rb_replace_node(struct rb_node *victim, struct rb_node *new,
		     struct rb_root *root)
{
	struct rb_node *parent = rb_parent(victim);	// 获取待替换节点的父节点

	/* Set the surrounding nodes to point to the replacement */
	/* 将周围的节点指向替换节点 */
	if (parent) {	// 如果存在父节点
		if (victim == parent->rb_left)	// 如果待替换节点是左子节点
			parent->rb_left = new;	// 将新节点设置为左子节点
		else	// 如果待替换节点是右子节点
			parent->rb_right = new;	// 将新节点设置为右子节点
	} else {	// 如果待替换节点是根节点
		root->rb_node = new;	// 更新根节点为新节点
	}
	/* 更新待替换节点的子节点的父指针 */
	if (victim->rb_left)	// 如果待替换节点有左子节点
		// 设置左子节点的父指针为新节点
		rb_set_parent(victim->rb_left, new);
	if (victim->rb_right)	// 如果待替换节点有右子节点
		rb_set_parent(victim->rb_right, new);	// 设置右子节点的父指针为新节点

	/* Copy the pointers/colour from the victim to the replacement */
	/* 从待替换节点复制指针和颜色到新节点 */
	*new = *victim;	// 将待替换节点的所有属性（包括颜色和指针）复制到新节点
}
EXPORT_SYMBOL(rb_replace_node);
