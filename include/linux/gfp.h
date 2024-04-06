#ifndef __LINUX_GFP_H
#define __LINUX_GFP_H

#include <linux/mmzone.h>
#include <linux/stddef.h>
#include <linux/linkage.h>
#include <linux/topology.h>
#include <linux/mmdebug.h>

struct vm_area_struct;

// 内存分配时的标志，分为三类，行为修饰符、区修饰符和类型。
// 行为修饰符表示内核应该如何分配内存，某些特定情况，只能使用特定方法分配内存，如中断中，分配内存不能睡眠
// 区修饰符表示从哪分配内存
// 类型标志组合了行为修饰符和区修饰符，便于使用。

/*
 * GFP bitmasks..
 *
 * Zone modifiers (see linux/mmzone.h - low three bits)
 *
 * Do not put any conditional on these. If necessary modify the definitions
 * without the underscores and use the consistently. The definitions here may
 * be used in bit comparisons.
 */
/*
 * GFP 位掩码..
 *
 * 区域修饰符（参见 linux/mmzone.h - 低三位）
 *
 * 不要对这些进行任何条件判断。如有必要，修改下划线开头的定义
 * 并一致地使用。这里的定义可能用于位比较。
 */

// 区域修饰符，不存在__GFP_NORMAL，因为如果不指定任何标志，默认是从ZONE_NORMAL（优先）和ZONE_DMA分配内存

// 从ZONE_DMA分配内存
#define __GFP_DMA	((__force gfp_t)0x01u)
// 从ZONE_HIGHMEM（优先）或ZONE_NORMAL分配
#define __GFP_HIGHMEM	((__force gfp_t)0x02u)
// 只在ZONE_DMA32分配
#define __GFP_DMA32	((__force gfp_t)0x04u)
/* 页面是可移动的，意味着可以被页面迁移机制移动或回收 */
#define __GFP_MOVABLE	((__force gfp_t)0x08u)  /* Page is movable */
/* 区域掩码，包含所有前面定义的区域修饰符 */
#define GFP_ZONEMASK	(__GFP_DMA|__GFP_HIGHMEM|__GFP_DMA32|__GFP_MOVABLE)
/*
 * Action modifiers - doesn't change the zoning
 *
 * __GFP_REPEAT: Try hard to allocate the memory, but the allocation attempt
 * _might_ fail.  This depends upon the particular VM implementation.
 *
 * __GFP_NOFAIL: The VM implementation _must_ retry infinitely: the caller
 * cannot handle allocation failures.  This modifier is deprecated and no new
 * users should be added.
 *
 * __GFP_NORETRY: The VM implementation must not retry indefinitely.
 *
 * __GFP_MOVABLE: Flag that this page will be movable by the page migration
 * mechanism or reclaimed
 */
/*
 * 行为修饰符 - 不改变区域设置
 *
 * __GFP_REPEAT: 努力尝试分配内存，但分配尝试
 * _可能_会失败。这取决于特定的虚拟内存实现。
 *
 * __GFP_NOFAIL: 虚拟内存实现 _必须_ 无限重试：调用者
 * 无法处理分配失败。此修饰符已废弃，不应添加新用户。
 *
 * __GFP_NORETRY: 虚拟内存实现不得无限重试。
 *
 * __GFP_MOVABLE: 标记该页面将通过页面迁移机制移动或被回收
 */

// 行为修饰符

// 分配器可以睡眠
#define __GFP_WAIT	((__force gfp_t)0x10u)	/* Can wait and reschedule? */
// 分配器可以访问紧急事件缓冲池
#define __GFP_HIGH	((__force gfp_t)0x20u)	/* Should access emergency pools? */
// 分配器可以启动磁盘IO
#define __GFP_IO	((__force gfp_t)0x40u)	/* Can start physical IO? */
// 分配器可以启动文件系统IO
#define __GFP_FS	((__force gfp_t)0x80u)	/* Can call down to low-level FS? */
// 分配器应该使用高速缓存中快要淘汰出去的页
#define __GFP_COLD	((__force gfp_t)0x100u)	/* Cache-cold page required */
// 分配器将不打印失败警告
#define __GFP_NOWARN	((__force gfp_t)0x200u)	/* Suppress page allocation failure warning */
// 分配器在分配失败时重复进行分配，但是这次分配还存在失败可能
#define __GFP_REPEAT	((__force gfp_t)0x400u)	/* See above */
// 分配器将无限期地重复进行分配。分配不能失败
#define __GFP_NOFAIL	((__force gfp_t)0x800u)	/* See above */
// 分配器在分配失败时绝对不会重新分配
#define __GFP_NORETRY	((__force gfp_t)0x1000u)/* See above */
// 添加混合页元数据，在hugetlb的代码内部使用
#define __GFP_COMP	((__force gfp_t)0x4000u)/* Add compound page metadata */
/* 成功返回时返回零页 */
#define __GFP_ZERO	((__force gfp_t)0x8000u)/* Return zeroed page on success */
/* 不使用紧急储备 */
#define __GFP_NOMEMALLOC ((__force gfp_t)0x10000u) /* Don't use emergency reserves */
/* 强制执行硬墙内存分配 */
#define __GFP_HARDWALL   ((__force gfp_t)0x20000u) /* Enforce hardwall cpuset memory allocs */
/* 无后备策略，仅此节点 */
#define __GFP_THISNODE	((__force gfp_t)0x40000u)/* No fallback, no policies */
/* 页面是可回收的 */
#define __GFP_RECLAIMABLE ((__force gfp_t)0x80000u) /* Page is reclaimable */

#ifdef CONFIG_KMEMCHECK
/* 不要用 kmemcheck 跟踪 */
#define __GFP_NOTRACK	((__force gfp_t)0x200000u)  /* Don't track with kmemcheck */
#else
#define __GFP_NOTRACK	((__force gfp_t)0)
#endif

/*
 * This may seem redundant, but it's a way of annotating false positives vs.
 * allocations that simply cannot be supported (e.g. page tables).
 */
/*
 * 这可能看起来多余，但它是一种标注的方式，区分误报与
 * 纯粹无法支持的分配（例如，页表）。
 */

// 类型标志

#define __GFP_NOTRACK_FALSE_POSITIVE (__GFP_NOTRACK)

#define __GFP_BITS_SHIFT 22	/* Room for 22 __GFP_FOO bits */
#define __GFP_BITS_MASK ((__force gfp_t)((1 << __GFP_BITS_SHIFT) - 1))

/* This equals 0, but use constants in case they ever change */
// 与GFP_ATOMIC类似，不同之处在于调用不会退给紧急内存池。这就增加了内存分配失败的可能性。
#define GFP_NOWAIT	(GFP_ATOMIC & ~__GFP_HIGH)
/* GFP_ATOMIC means both !wait (__GFP_WAIT not set) and use emergency pool */
/* GFP_ATOMIC 表示既不等待（未设置 __GFP_WAIT）又使用紧急池 */
// 此标志在中断处理程序、下半部、持有自旋锁以及其他不能睡眠的地方
#define GFP_ATOMIC	(__GFP_HIGH)
// 这种分配可以阻塞，但不 会启动磁盘I/O。这个标志在不能引发更多磁盘I/O时能阻塞I/O代码，可能导致令人不愉快的递归。
#define GFP_NOIO	(__GFP_WAIT)
// 这种地分配在必要时可以阻塞，也可以启动磁盘I/O，但不会启动文件系统操作。这个标志在你不能再启动另一个文件系统的操作时，用在文件系统的部分代码中
#define GFP_NOFS	(__GFP_WAIT | __GFP_IO)
// 常规分配方式，可能阻塞。这个标志在睡眠安全时用在进程上下文代码中。为了获得调用者所需内存，内核会尽力而为。
#define GFP_KERNEL	(__GFP_WAIT | __GFP_IO | __GFP_FS)
#define GFP_TEMPORARY	(__GFP_WAIT | __GFP_IO | __GFP_FS | \
			 __GFP_RECLAIMABLE)
// 这是一种常规分配方式，可能阻塞。用于为用户空间进程分配内存时。
#define GFP_USER	(__GFP_WAIT | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
// 这是从ZONE_HIGHMEM进行分配，可能会阻塞。这个标志为用户空间进程分配内存。
#define GFP_HIGHUSER	(__GFP_WAIT | __GFP_IO | __GFP_FS | __GFP_HARDWALL | \
			 __GFP_HIGHMEM)
#define GFP_HIGHUSER_MOVABLE	(__GFP_WAIT | __GFP_IO | __GFP_FS | \
				 __GFP_HARDWALL | __GFP_HIGHMEM | \
				 __GFP_MOVABLE)
#define GFP_IOFS	(__GFP_IO | __GFP_FS)

#ifdef CONFIG_NUMA
#define GFP_THISNODE	(__GFP_THISNODE | __GFP_NOWARN | __GFP_NORETRY)
#else
#define GFP_THISNODE	((__force gfp_t)0)
#endif

/* This mask makes up all the page movable related flags */
#define GFP_MOVABLE_MASK (__GFP_RECLAIMABLE|__GFP_MOVABLE)

/* Control page allocator reclaim behavior */
#define GFP_RECLAIM_MASK (__GFP_WAIT|__GFP_HIGH|__GFP_IO|__GFP_FS|\
			__GFP_NOWARN|__GFP_REPEAT|__GFP_NOFAIL|\
			__GFP_NORETRY|__GFP_NOMEMALLOC)

/* Control slab gfp mask during early boot */
#define GFP_BOOT_MASK __GFP_BITS_MASK & ~(__GFP_WAIT|__GFP_IO|__GFP_FS)

/* Control allocation constraints */
#define GFP_CONSTRAINT_MASK (__GFP_HARDWALL|__GFP_THISNODE)

/* Do not use these with a slab allocator */
#define GFP_SLAB_BUG_MASK (__GFP_DMA32|__GFP_HIGHMEM|~__GFP_BITS_MASK)

/* Flag - indicates that the buffer will be suitable for DMA.  Ignored on some
   platforms, used as appropriate on others */

// 这是从ZONE_DMA进行分配。需要获取能提供DMA使用的内存的设备驱动程序使用这个标志，通常与某个标志一起使用（GFP_ATOMIC和GFP_KERNEL）。
#define GFP_DMA		__GFP_DMA

/* 4GB DMA on some platforms */
#define GFP_DMA32	__GFP_DMA32

/* Convert GFP flags to their corresponding migrate type */
static inline int allocflags_to_migratetype(gfp_t gfp_flags)
{
	WARN_ON((gfp_flags & GFP_MOVABLE_MASK) == GFP_MOVABLE_MASK);

	if (unlikely(page_group_by_mobility_disabled))
		return MIGRATE_UNMOVABLE;

	/* Group based on mobility */
	return (((gfp_flags & __GFP_MOVABLE) != 0) << 1) |
		((gfp_flags & __GFP_RECLAIMABLE) != 0);
}

#ifdef CONFIG_HIGHMEM
#define OPT_ZONE_HIGHMEM ZONE_HIGHMEM
#else
#define OPT_ZONE_HIGHMEM ZONE_NORMAL
#endif

#ifdef CONFIG_ZONE_DMA
#define OPT_ZONE_DMA ZONE_DMA
#else
#define OPT_ZONE_DMA ZONE_NORMAL
#endif

#ifdef CONFIG_ZONE_DMA32
#define OPT_ZONE_DMA32 ZONE_DMA32
#else
#define OPT_ZONE_DMA32 ZONE_NORMAL
#endif

/*
 * GFP_ZONE_TABLE is a word size bitstring that is used for looking up the
 * zone to use given the lowest 4 bits of gfp_t. Entries are ZONE_SHIFT long
 * and there are 16 of them to cover all possible combinations of
 * __GFP_DMA, __GFP_DMA32, __GFP_MOVABLE and __GFP_HIGHMEM
 *
 * The zone fallback order is MOVABLE=>HIGHMEM=>NORMAL=>DMA32=>DMA.
 * But GFP_MOVABLE is not only a zone specifier but also an allocation
 * policy. Therefore __GFP_MOVABLE plus another zone selector is valid.
 * Only 1bit of the lowest 3 bit (DMA,DMA32,HIGHMEM) can be set to "1".
 *
 *       bit       result
 *       =================
 *       0x0    => NORMAL
 *       0x1    => DMA or NORMAL
 *       0x2    => HIGHMEM or NORMAL
 *       0x3    => BAD (DMA+HIGHMEM)
 *       0x4    => DMA32 or DMA or NORMAL
 *       0x5    => BAD (DMA+DMA32)
 *       0x6    => BAD (HIGHMEM+DMA32)
 *       0x7    => BAD (HIGHMEM+DMA32+DMA)
 *       0x8    => NORMAL (MOVABLE+0)
 *       0x9    => DMA or NORMAL (MOVABLE+DMA)
 *       0xa    => MOVABLE (Movable is valid only if HIGHMEM is set too)
 *       0xb    => BAD (MOVABLE+HIGHMEM+DMA)
 *       0xc    => DMA32 (MOVABLE+HIGHMEM+DMA32)
 *       0xd    => BAD (MOVABLE+DMA32+DMA)
 *       0xe    => BAD (MOVABLE+DMA32+HIGHMEM)
 *       0xf    => BAD (MOVABLE+DMA32+HIGHMEM+DMA)
 *
 * ZONES_SHIFT must be <= 2 on 32 bit platforms.
 */

#if 16 * ZONES_SHIFT > BITS_PER_LONG
#error ZONES_SHIFT too large to create GFP_ZONE_TABLE integer
#endif

#define GFP_ZONE_TABLE ( \
	(ZONE_NORMAL << 0 * ZONES_SHIFT)				\
	| (OPT_ZONE_DMA << __GFP_DMA * ZONES_SHIFT) 			\
	| (OPT_ZONE_HIGHMEM << __GFP_HIGHMEM * ZONES_SHIFT)		\
	| (OPT_ZONE_DMA32 << __GFP_DMA32 * ZONES_SHIFT)			\
	| (ZONE_NORMAL << __GFP_MOVABLE * ZONES_SHIFT)			\
	| (OPT_ZONE_DMA << (__GFP_MOVABLE | __GFP_DMA) * ZONES_SHIFT)	\
	| (ZONE_MOVABLE << (__GFP_MOVABLE | __GFP_HIGHMEM) * ZONES_SHIFT)\
	| (OPT_ZONE_DMA32 << (__GFP_MOVABLE | __GFP_DMA32) * ZONES_SHIFT)\
)

/*
 * GFP_ZONE_BAD is a bitmap for all combination of __GFP_DMA, __GFP_DMA32
 * __GFP_HIGHMEM and __GFP_MOVABLE that are not permitted. One flag per
 * entry starting with bit 0. Bit is set if the combination is not
 * allowed.
 */
#define GFP_ZONE_BAD ( \
	1 << (__GFP_DMA | __GFP_HIGHMEM)				\
	| 1 << (__GFP_DMA | __GFP_DMA32)				\
	| 1 << (__GFP_DMA32 | __GFP_HIGHMEM)				\
	| 1 << (__GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM)		\
	| 1 << (__GFP_MOVABLE | __GFP_HIGHMEM | __GFP_DMA)		\
	| 1 << (__GFP_MOVABLE | __GFP_DMA32 | __GFP_DMA)		\
	| 1 << (__GFP_MOVABLE | __GFP_DMA32 | __GFP_HIGHMEM)		\
	| 1 << (__GFP_MOVABLE | __GFP_DMA32 | __GFP_DMA | __GFP_HIGHMEM)\
)

static inline enum zone_type gfp_zone(gfp_t flags)
{
	enum zone_type z;
	int bit = flags & GFP_ZONEMASK;

	z = (GFP_ZONE_TABLE >> (bit * ZONES_SHIFT)) &
					 ((1 << ZONES_SHIFT) - 1);

	if (__builtin_constant_p(bit))
		MAYBE_BUILD_BUG_ON((GFP_ZONE_BAD >> bit) & 1);
	else {
#ifdef CONFIG_DEBUG_VM
		BUG_ON((GFP_ZONE_BAD >> bit) & 1);
#endif
	}
	return z;
}

/*
 * There is only one page-allocator function, and two main namespaces to
 * it. The alloc_page*() variants return 'struct page *' and as such
 * can allocate highmem pages, the *get*page*() variants return
 * virtual kernel addresses to the allocated page(s).
 */

static inline int gfp_zonelist(gfp_t flags)
{
	if (NUMA_BUILD && unlikely(flags & __GFP_THISNODE))
		return 1;

	return 0;
}

/*
 * We get the zone list from the current node and the gfp_mask.
 * This zone list contains a maximum of MAXNODES*MAX_NR_ZONES zones.
 * There are two zonelists per node, one for all zones with memory and
 * one containing just zones from the node the zonelist belongs to.
 *
 * For the normal case of non-DISCONTIGMEM systems the NODE_DATA() gets
 * optimized to &contig_page_data at compile-time.
 */
static inline struct zonelist *node_zonelist(int nid, gfp_t flags)
{
	return NODE_DATA(nid)->node_zonelists + gfp_zonelist(flags);
}

#ifndef HAVE_ARCH_FREE_PAGE
static inline void arch_free_page(struct page *page, int order) { }
#endif
#ifndef HAVE_ARCH_ALLOC_PAGE
static inline void arch_alloc_page(struct page *page, int order) { }
#endif

struct page *
__alloc_pages_nodemask(gfp_t gfp_mask, unsigned int order,
		       struct zonelist *zonelist, nodemask_t *nodemask);

static inline struct page *
__alloc_pages(gfp_t gfp_mask, unsigned int order,
		struct zonelist *zonelist)
{
	return __alloc_pages_nodemask(gfp_mask, order, zonelist, NULL);
}

static inline struct page *alloc_pages_node(int nid, gfp_t gfp_mask,
						unsigned int order)
{
	/* Unknown node is current node */
	if (nid < 0)
		nid = numa_node_id();

	return __alloc_pages(gfp_mask, order, node_zonelist(nid, gfp_mask));
}

static inline struct page *alloc_pages_exact_node(int nid, gfp_t gfp_mask,
						unsigned int order)
{
	VM_BUG_ON(nid < 0 || nid >= MAX_NUMNODES);

	return __alloc_pages(gfp_mask, order, node_zonelist(nid, gfp_mask));
}

#ifdef CONFIG_NUMA
extern struct page *alloc_pages_current(gfp_t gfp_mask, unsigned order);

// 以页为单位分配内存，分配2的order(1 << order)个连续的物理页，并返回一个指针，该指针指向第一个页的page结构体，如果出错，返回NULL
// 可以使用page_address将返回的指定页转换成它的逻辑地址，page_address返回一个指针，指向给定物理页当前所在的逻辑地址。
// 如果无需使用struct page，则可以调用__get_free_pages，该函数与alloc_pages()作用相同，它直接返回请求的第一个页的逻辑地址，因为页是连续，其他页会紧随其后。
static inline struct page *
alloc_pages(gfp_t gfp_mask, unsigned int order)
{
	return alloc_pages_current(gfp_mask, order);
}
extern struct page *alloc_page_vma(gfp_t gfp_mask,
			struct vm_area_struct *vma, unsigned long addr);
#else
// 以页为单位分配内存，分配2的order(1 << order)个连续的物理页，并返回一个指针，该指针指向第一个页的page结构体，如果出错，返回NULL
// 可以使用page_address将返回的指定页转换成它的逻辑地址，page_address返回一个指针，指向给定物理页当前所在的逻辑地址。
// 如果无需使用struct page，则可以调用__get_free_pages，该函数与alloc_pages()作用相同，它直接返回请求的第一个页的逻辑地址，因为页是连续，其他页会紧随其后。
#define alloc_pages(gfp_mask, order) \
		alloc_pages_node(numa_node_id(), gfp_mask, order)
#define alloc_page_vma(gfp_mask, vma, addr) alloc_pages(gfp_mask, 0)
#endif
// 如果只需要一页，可以调用该函数，传递给order的值是0(2的0次方 == 1)，该函数分配一个页，返回指向页结构的指针
#define alloc_page(gfp_mask) alloc_pages(gfp_mask, 0)

// 该函数与alloc_pages()作用相同，它直接返回请求的第一个页的逻辑地址，因为页是连续，其他页会紧随其后。
extern unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order);
// 分配一个页，并且页的内容被置为0
extern unsigned long get_zeroed_page(gfp_t gfp_mask);

void *alloc_pages_exact(size_t size, gfp_t gfp_mask);
void free_pages_exact(void *virt, size_t size);

// 如果只需要一页，可以调用该函数(2的0次方 == 1)，返回指向其逻辑地址的指针
#define __get_free_page(gfp_mask) \
		__get_free_pages((gfp_mask),0)

#define __get_dma_pages(gfp_mask, order) \
		__get_free_pages((gfp_mask) | GFP_DMA,(order))

// 释放页
extern void __free_pages(struct page *page, unsigned int order);
// 释放页
extern void free_pages(unsigned long addr, unsigned int order);
extern void free_hot_cold_page(struct page *page, int cold);

// 释放页
#define __free_page(page) __free_pages((page), 0)
#define free_page(addr) free_pages((addr),0)

void page_alloc_init(void);
void drain_zone_pages(struct zone *zone, struct per_cpu_pages *pcp);
void drain_all_pages(void);
void drain_local_pages(void *dummy);

extern gfp_t gfp_allowed_mask;

extern void set_gfp_allowed_mask(gfp_t mask);
extern gfp_t clear_gfp_allowed_mask(gfp_t mask);

#endif /* __LINUX_GFP_H */
