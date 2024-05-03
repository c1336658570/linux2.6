/*
 * Written by Mark Hemment, 1996 (markhe@nextd.demon.co.uk).
 *
 * (C) SGI 2006, Christoph Lameter
 * 	Cleaned up and restructured to ease the addition of alternative
 * 	implementations of SLAB allocators.
 */

#ifndef _LINUX_SLAB_H
#define	_LINUX_SLAB_H

#include <linux/gfp.h>
#include <linux/types.h>

/*
 * Flags to pass to kmem_cache_create().
 * The ones marked DEBUG are only valid if CONFIG_SLAB_DEBUG is set.
 */
#define SLAB_DEBUG_FREE		0x00000100UL	/* DEBUG: Perform (expensive) checks on free */
// 导致slab层在已分配的内存周围插入“红色警戒区”以探测缓冲越界
#define SLAB_RED_ZONE		0x00000400UL	/* DEBUG: Red zone objs in a cache */
// 使slab层用已知的值（a5a5a5a5）填充slab。这就是所谓的中毒，有利于对未初始化内存的访问
#define SLAB_POISON		0x00000800UL	/* DEBUG: Poison objects */
// 所有对象按高速缓存行对齐。
#define SLAB_HWCACHE_ALIGN	0x00002000UL	/* Align objs on cache lines */
// slab层使用可以执行DMA的内存给每个slab分配空间。
#define SLAB_CACHE_DMA		0x00004000UL	/* Use GFP_DMA memory */
#define SLAB_STORE_USER		0x00010000UL	/* DEBUG: Store the last owner for bug hunting */
// 当分配失败时提醒slab层
#define SLAB_PANIC		0x00040000UL	/* Panic if kmem_cache_create() fails */
/*
 * SLAB_DESTROY_BY_RCU - **WARNING** READ THIS!
 *
 * This delays freeing the SLAB page by a grace period, it does _NOT_
 * delay object freeing. This means that if you do kmem_cache_free()
 * that memory location is free to be reused at any time. Thus it may
 * be possible to see another object there in the same RCU grace period.
 *
 * This feature only ensures the memory location backing the object
 * stays valid, the trick to using this is relying on an independent
 * object validation pass. Something like:
 *
 *  rcu_read_lock()
 * again:
 *  obj = lockless_lookup(key);
 *  if (obj) {
 *    if (!try_get_ref(obj)) // might fail for free objects
 *      goto again;
 *
 *    if (obj->key != key) { // not the object we expected
 *      put_ref(obj);
 *      goto again;
 *    }
 *  }
 *  rcu_read_unlock();
 *
 * See also the comment on struct slab_rcu in mm/slab.c.
 */
#define SLAB_DESTROY_BY_RCU	0x00080000UL	/* Defer freeing slabs to RCU */
#define SLAB_MEM_SPREAD		0x00100000UL	/* Spread some memory over cpuset */
#define SLAB_TRACE		0x00200000UL	/* Trace allocations and frees */

/* Flag to prevent checks on free */
#ifdef CONFIG_DEBUG_OBJECTS
# define SLAB_DEBUG_OBJECTS	0x00400000UL
#else
# define SLAB_DEBUG_OBJECTS	0x00000000UL
#endif

#define SLAB_NOLEAKTRACE	0x00800000UL	/* Avoid kmemleak tracing */

/* Don't track use of uninitialized memory */
#ifdef CONFIG_KMEMCHECK
# define SLAB_NOTRACK		0x01000000UL
#else
# define SLAB_NOTRACK		0x00000000UL
#endif
#ifdef CONFIG_FAILSLAB
# define SLAB_FAILSLAB		0x02000000UL	/* Fault injection mark */
#else
# define SLAB_FAILSLAB		0x00000000UL
#endif

/* The following flags affect the page allocator grouping pages by mobility */
#define SLAB_RECLAIM_ACCOUNT	0x00020000UL		/* Objects are reclaimable */
#define SLAB_TEMPORARY		SLAB_RECLAIM_ACCOUNT	/* Objects are short-lived */
/*
 * ZERO_SIZE_PTR will be returned for zero sized kmalloc requests.
 *
 * Dereferencing ZERO_SIZE_PTR will lead to a distinct access fault.
 *
 * ZERO_SIZE_PTR can be passed to kfree though in the same way that NULL can.
 * Both make kfree a no-op.
 */
#define ZERO_SIZE_PTR ((void *)16)

#define ZERO_OR_NULL_PTR(x) ((unsigned long)(x) <= \
				(unsigned long)ZERO_SIZE_PTR)

/*
 * struct kmem_cache related prototypes
 */
void __init kmem_cache_init(void);
int slab_is_available(void);

/**
 * 
 *
 * Interface to system's page allocator. No need to hold the cache-lock.
 * 系统页面分配器的接口。无需持有缓存锁。
 * 
 * If we requested dmaable memory, we will get it. Even if we
 * did not request dmaable memory, we might get it, but that
 * would be relatively rare and ignorable.
 * 如果我们请求了可DMA的内存，我们将得到它。
 * 即使我们没有请求DMA内存，我们也可能得到它，但这是相对罕见且可以忽略的。

// 下面函数被slab层调用。
// 用于创建新的slab对象，一个高速缓存有多个slab，当高速缓存中slab对象用完了就需要创建新的slab对象
// 第一个参数指向需要很多页的特定高速缓存。第二个参数是内存分配标志。
// 当nodeid非负时，分配器尝试从相同的内存节点给发出的请求进行分配。
static void *kmem_getpages(struct kmem_cache *cachep, gfp_t flags, int nodeid)
*/

// 函数用于创建一个新的slab缓存（高速缓存）。第一个参数是字符串，存放高速缓存的名字，第二个参数是高速缓存中每个元素的大小，
// 第三个参数是slab内第一个对象的偏移，用来确保在页内进行特定的对齐。通常0就可以满足，也就是标准对齐。
// flags参数是可选的设置项，用来控制高速缓存的行为。可以为0,表示没有特殊行为，或者与以下标志进行或运算，在slab.h中存在定义
// SLAB_HWCACHE_ALIGN
// SLAB_POISON
// SLAB_RED_ZONE
// SLAB_PANIC
// SLAB_CACHE_DMA
// 最后一个参数ctor是高速缓存的构造函数。只有在新的页主加到高速缓存，构造函数才被调用。实际上Linux内核的高速缓存不适用构造函数，赋值为NULL即可。
// 函数成功时返回一个指向所创建的高速缓存的指针，否则返回NULL。该函数可能睡眠，不应在中断上下文调用。
struct kmem_cache *kmem_cache_create(const char *, size_t, size_t,
			unsigned long,
			void (*)(void *));
// 撤销一个给定高速缓存。该函数不能在中断中调用，因为可能睡眠。该函数调用需要两个条件：
// 1.高速缓存中所有slab都为空。
// 2.在调用kmem_cache_destroy()过程中（后）不再访问这个高速缓存。
// 成功返回0,否则返回非0。
void kmem_cache_destroy(struct kmem_cache *);
int kmem_cache_shrink(struct kmem_cache *);
// 释放一个对象，并把它返回给原先的slab，这样就能把cachep中的对象objp标记为空闲。和kmem_cache_alloc相反。
void kmem_cache_free(struct kmem_cache *, void *objp);
unsigned int kmem_cache_size(struct kmem_cache *);
const char *kmem_cache_name(struct kmem_cache *);
int kern_ptr_validate(const void *ptr, unsigned long size);
int kmem_ptr_validate(struct kmem_cache *cachep, const void *ptr);

/*
 * Please use this macro to create slab caches. Simply specify the
 * name of the structure and maybe some flags that are listed above.
 *
 * The alignment of the struct determines object alignment. If you
 * f.e. add ____cacheline_aligned_in_smp to the struct declaration
 * then the objects will be properly aligned in SMP configurations.
 */
#define KMEM_CACHE(__struct, __flags) kmem_cache_create(#__struct,\
		sizeof(struct __struct), __alignof__(struct __struct),\
		(__flags), NULL)

/*
 * The largest kmalloc size supported by the slab allocators is
 * 32 megabyte (2^25) or the maximum allocatable page order if that is
 * less than 32 MB.
 *
 * WARNING: Its not easy to increase this value since the allocators have
 * to do various tricks to work around compiler limitations in order to
 * ensure proper constant folding.
 */
#define KMALLOC_SHIFT_HIGH	((MAX_ORDER + PAGE_SHIFT - 1) <= 25 ? \
				(MAX_ORDER + PAGE_SHIFT - 1) : 25)

#define KMALLOC_MAX_SIZE	(1UL << KMALLOC_SHIFT_HIGH)
#define KMALLOC_MAX_ORDER	(KMALLOC_SHIFT_HIGH - PAGE_SHIFT)

/*
 * Common kmalloc functions provided by all allocators
 */
void * __must_check __krealloc(const void *, size_t, gfp_t);
void * __must_check krealloc(const void *, size_t, gfp_t);
// 获得以字节为单位的一块内核内存。返回一个指向内存块的指针，内存块至少有size大小。所分配的内存区在物理上是连续的。出错返回NULL。
// static __always_inline void *kmalloc(size_t size, gfp_t flags)
// 释放kmalloc分配的内存，kfree(NULL)是安全的
void kfree(const void *);
void kzfree(const void *);
size_t ksize(const void *);

/*
 * Allocator specific definitions. These are mainly used to establish optimized
 * ways to convert kmalloc() calls to kmem_cache_alloc() invocations by
 * selecting the appropriate general cache at compile time.
 *
 * Allocators must define at least:
 *
 *	kmem_cache_alloc()
 *	__kmalloc()
 *	kmalloc()
 *
 * Those wishing to support NUMA must also define:
 *
 *	kmem_cache_alloc_node()
 *	kmalloc_node()
 *
 * See each allocator definition file for additional comments and
 * implementation notes.
 */
#ifdef CONFIG_SLUB
#include <linux/slub_def.h>
#elif defined(CONFIG_SLOB)
#include <linux/slob_def.h>
#else
#include <linux/slab_def.h>
#endif

/**
 * kcalloc - allocate memory for an array. The memory is set to zero.
 * @n: number of elements.
 * @size: element size.
 * @flags: the type of memory to allocate.
 *
 * The @flags argument may be one of:
 *
 * %GFP_USER - Allocate memory on behalf of user.  May sleep.
 *
 * %GFP_KERNEL - Allocate normal kernel ram.  May sleep.
 *
 * %GFP_ATOMIC - Allocation will not sleep.  May use emergency pools.
 *   For example, use this inside interrupt handlers.
 *
 * %GFP_HIGHUSER - Allocate pages from high memory.
 *
 * %GFP_NOIO - Do not do any I/O at all while trying to get memory.
 *
 * %GFP_NOFS - Do not make any fs calls while trying to get memory.
 *
 * %GFP_NOWAIT - Allocation will not sleep.
 *
 * %GFP_THISNODE - Allocate node-local memory only.
 *
 * %GFP_DMA - Allocation suitable for DMA.
 *   Should only be used for kmalloc() caches. Otherwise, use a
 *   slab created with SLAB_DMA.
 *
 * Also it is possible to set different flags by OR'ing
 * in one or more of the following additional @flags:
 *
 * %__GFP_COLD - Request cache-cold pages instead of
 *   trying to return cache-warm pages.
 *
 * %__GFP_HIGH - This allocation has high priority and may use emergency pools.
 *
 * %__GFP_NOFAIL - Indicate that this allocation is in no way allowed to fail
 *   (think twice before using).
 *
 * %__GFP_NORETRY - If memory is not immediately available,
 *   then give up at once.
 *
 * %__GFP_NOWARN - If allocation fails, don't issue any warnings.
 *
 * %__GFP_REPEAT - If allocation fails initially, try once more before failing.
 *
 * There are other flags available as well, but these are not intended
 * for general use, and so are not documented here. For a full list of
 * potential flags, always refer to linux/gfp.h.
 */
static inline void *kcalloc(size_t n, size_t size, gfp_t flags)
{
	if (size != 0 && n > ULONG_MAX / size)
		return NULL;
	return __kmalloc(n * size, flags | __GFP_ZERO);
}

#if !defined(CONFIG_NUMA) && !defined(CONFIG_SLOB)
/**
 * kmalloc_node - allocate memory from a specific node
 * @size: how many bytes of memory are required.
 * @flags: the type of memory to allocate (see kcalloc).
 * @node: node to allocate from.
 *
 * kmalloc() for non-local nodes, used to allocate from a specific node
 * if available. Equivalent to kmalloc() in the non-NUMA single-node
 * case.
 */
static inline void *kmalloc_node(size_t size, gfp_t flags, int node)
{
	return kmalloc(size, flags);
}

static inline void *__kmalloc_node(size_t size, gfp_t flags, int node)
{
	return __kmalloc(size, flags);
}

// 创建高速缓存之后，通过如下函数获取对象。该函数从给定的高速缓存中返回一个指向对象的指针。如果高速缓存的所有slab中
// 都没有空闲对象，那么slab层必须通过kmem_getpages()获取新的页，gfp_t类型的参数传递给页面分配函数。应该是GFP_KERNEL或GFP_ATOMIC。
void *kmem_cache_alloc(struct kmem_cache *, gfp_t);

static inline void *kmem_cache_alloc_node(struct kmem_cache *cachep,
					gfp_t flags, int node)
{
	return kmem_cache_alloc(cachep, flags);
}
#endif /* !CONFIG_NUMA && !CONFIG_SLOB */

/*
 * kmalloc_track_caller is a special version of kmalloc that records the
 * calling function of the routine calling it for slab leak tracking instead
 * of just the calling function (confusing, eh?).
 * It's useful when the call to kmalloc comes from a widely-used standard
 * allocator where we care about the real place the memory allocation
 * request comes from.
 */
#if defined(CONFIG_DEBUG_SLAB) || defined(CONFIG_SLUB)
extern void *__kmalloc_track_caller(size_t, gfp_t, unsigned long);
#define kmalloc_track_caller(size, flags) \
	__kmalloc_track_caller(size, flags, _RET_IP_)
#else
#define kmalloc_track_caller(size, flags) \
	__kmalloc(size, flags)
#endif /* DEBUG_SLAB */

#ifdef CONFIG_NUMA
/*
 * kmalloc_node_track_caller is a special version of kmalloc_node that
 * records the calling function of the routine calling it for slab leak
 * tracking instead of just the calling function (confusing, eh?).
 * It's useful when the call to kmalloc_node comes from a widely-used
 * standard allocator where we care about the real place the memory
 * allocation request comes from.
 */
#if defined(CONFIG_DEBUG_SLAB) || defined(CONFIG_SLUB)
extern void *__kmalloc_node_track_caller(size_t, gfp_t, int, unsigned long);
#define kmalloc_node_track_caller(size, flags, node) \
	__kmalloc_node_track_caller(size, flags, node, \
			_RET_IP_)
#else
#define kmalloc_node_track_caller(size, flags, node) \
	__kmalloc_node(size, flags, node)
#endif

#else /* CONFIG_NUMA */

#define kmalloc_node_track_caller(size, flags, node) \
	kmalloc_track_caller(size, flags)

#endif /* CONFIG_NUMA */

/*
 * Shortcuts
 */
static inline void *kmem_cache_zalloc(struct kmem_cache *k, gfp_t flags)
{
	return kmem_cache_alloc(k, flags | __GFP_ZERO);
}

/**
 * kzalloc - allocate memory. The memory is set to zero.
 * @size: how many bytes of memory are required.
 * @flags: the type of memory to allocate (see kmalloc).
 */
static inline void *kzalloc(size_t size, gfp_t flags)
{
	return kmalloc(size, flags | __GFP_ZERO);
}

/**
 * kzalloc_node - allocate zeroed memory from a particular memory node.
 * @size: how many bytes of memory are required.
 * @flags: the type of memory to allocate (see kmalloc).
 * @node: memory node from which to allocate
 */
static inline void *kzalloc_node(size_t size, gfp_t flags, int node)
{
	return kmalloc_node(size, flags | __GFP_ZERO, node);
}

void __init kmem_cache_init_late(void);

#endif	/* _LINUX_SLAB_H */
