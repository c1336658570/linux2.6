/*
 * kref.c - library routines for handling generic reference counted objects
 *
 * Copyright (C) 2004 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (C) 2004 IBM Corp.
 *
 * based on lib/kobject.c which was:
 * Copyright (C) 2002-2003 Patrick Mochel <mochel@osdl.org>
 *
 * This file is released under the GPLv2.
 *
 */

#include <linux/kref.h>
#include <linux/module.h>
#include <linux/slab.h>

/**
 * kref_set - initialize object and set refcount to requested number.
 * @kref: object in question.
 * @num: initial reference counter
 */
/**
 * kref_set - 初始化对象并将引用计数设置为请求的数值。
 * @kref: 相关对象。
 * @num: 初始引用计数
 */

void kref_set(struct kref *kref, int num)
{
	atomic_set(&kref->refcount, num);
	smp_mb();		// 在多处理器系统中添加内存屏障，确保操作的顺序性
}

/**
 * kref_init - initialize object.
 * @kref: object in question.
 */

/**
 * kref_init - 初始化对象。
 * @kref: 相关对象。
 */
void kref_init(struct kref *kref)
{
	kref_set(kref, 1);
}

/**
 * kref_get - increment refcount for object.
 * @kref: object.
 */
/**
 * kref_get - 为对象增加引用计数。
 * @kref: 对象。
 */
void kref_get(struct kref *kref)
{
	// 如果引用计数为0，则发出警告
	WARN_ON(!atomic_read(&kref->refcount));
	atomic_inc(&kref->refcount);
	smp_mb__after_atomic_inc();	// 在增加后添加一个内存屏障
}

/**
 * kref_put - decrement refcount for object.
 * @kref: object.
 * @release: pointer to the function that will clean up the object when the
 *	     last reference to the object is released.
 *	     This pointer is required, and it is not acceptable to pass kfree
 *	     in as this function.
 *
 * Decrement the refcount, and if 0, call release().
 * Return 1 if the object was removed, otherwise return 0.  Beware, if this
 * function returns 0, you still can not count on the kref from remaining in
 * memory.  Only use the return value if you want to see if the kref is now
 * gone, not present.
 */
/**
 * kref_put - 为对象减少引用计数。
 * @kref: 对象。
 * @release: 函数指针，指向当对象的最后一个引用被释放时将清理该对象的函数。
 *           传递这个指针是必须的，不接受传递 kfree 作为这个函数。
 *
 * 减少引用计数，如果为0，则调用 release()。
 * 如果对象被移除则返回 1，否则返回 0。注意，如果这个函数返回 0，
 * 你仍然不能保证 kref 仍在内存中。只有当你想要确认 kref 是否已经消失时，
 * 才使用返回值。
 */
int kref_put(struct kref *kref, void (*release)(struct kref *kref))
{
	// 如果 release 为 NULL，则发出警告
	WARN_ON(release == NULL);
	// 如果 release 是 kfree，则发出警告
	WARN_ON(release == (void (*)(struct kref *))kfree);

	if (atomic_dec_and_test(&kref->refcount)) {
		release(kref);	// 如果为0，调用释放函数
		return 1;				// 返回1表示对象已经被移除
	}
	return 0;					// 返回0表示对象尚未被移除
}

EXPORT_SYMBOL(kref_set);
EXPORT_SYMBOL(kref_init);
EXPORT_SYMBOL(kref_get);
EXPORT_SYMBOL(kref_put);
