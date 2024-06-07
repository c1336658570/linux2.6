/*
 * include/asm-xtensa/page.h
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version2 as
 * published by the Free Software Foundation.
 *
 * Copyright (C) 2001 - 2007 Tensilica Inc.
 */

#ifndef _XTENSA_PAGE_H
#define _XTENSA_PAGE_H

#include <asm/processor.h>
#include <asm/types.h>
#include <asm/cache.h>
#include <platform/hardware.h>

/*
 * Fixed TLB translations in the processor.
 */
/*
 * 在处理器中固定了 TLB 转换。
 */

/* 缓存的虚拟地址的起始地址 */
#define XCHAL_KSEG_CACHED_VADDR 0xd0000000
/* 绕过缓存的虚拟地址的起始地址 */
#define XCHAL_KSEG_BYPASS_VADDR 0xd8000000
/* 对应的物理地址起始位置 */
#define XCHAL_KSEG_PADDR        0x00000000
/* KSEG 区域的大小 */
#define XCHAL_KSEG_SIZE         0x08000000

/*
 * PAGE_SHIFT determines the page size
 */
/*
 * PAGE_SHIFT 决定了页面大小。
 */

/* 页面移位量，用于定义页面大小 */
#define PAGE_SHIFT		12
/* 页面大小，通过左移 PAGE_SHIFT 位得到 */
#define PAGE_SIZE		(__XTENSA_UL_CONST(1) << PAGE_SHIFT)
/* 页面掩码，用于地址对齐 */
#define PAGE_MASK		(~(PAGE_SIZE-1))

/* 根据是否定义了 MMU（内存管理单元），设置不同的宏 */
#ifdef CONFIG_MMU
/* 如果定义了 MMU，页面偏移设置为缓存的虚拟地址 */
#define PAGE_OFFSET		XCHAL_KSEG_CACHED_VADDR
/* 最大物理帧编号设置为 KSEG 区域的大小 */
#define MAX_MEM_PFN		XCHAL_KSEG_SIZE
#else
/* 如果没有定义 MMU，页面偏移设置为 0 */
#define PAGE_OFFSET		0
/* 最大物理帧编号设置为平台默认内存起始地址加上大小 */
#define MAX_MEM_PFN		(PLATFORM_DEFAULT_MEM_START + PLATFORM_DEFAULT_MEM_SIZE)
#endif

/* 页表开始的地址 */
#define PGTABLE_START		0x80000000

/*
 * Cache aliasing:
 *
 * If the cache size for one way is greater than the page size, we have to
 * deal with cache aliasing. The cache index is wider than the page size:
 *
 * |    |cache| cache index
 * | pfn  |off|	virtual address
 * |xxxx:X|zzz|
 * |    : |   |
 * | \  / |   |
 * |trans.|   |
 * | /  \ |   |
 * |yyyy:Y|zzz|	physical address
 *
 * When the page number is translated to the physical page address, the lowest
 * bit(s) (X) that are part of the cache index are also translated (Y).
 * If this translation changes bit(s) (X), the cache index is also afected,
 * thus resulting in a different cache line than before.
 * The kernel does not provide a mechanism to ensure that the page color
 * (represented by this bit) remains the same when allocated or when pages
 * are remapped. When user pages are mapped into kernel space, the color of
 * the page might also change.
 *
 * We use the address space VMALLOC_END ... VMALLOC_END + DCACHE_WAY_SIZE * 2
 * to temporarily map a patch so we can match the color.
 */
/*
 * 缓存别名问题：
 * 
 * 如果单向缓存的大小大于页面大小，我们需要处理缓存别名问题。缓存索引比页面大小宽：
 * 
 * 在页面编号转换为物理页面地址时，缓存索引的最低位（X）也会被转换（Y）。
 * 如果这种转换改变了位（X），缓存索引也会受到影响，从而导致缓存行改变。
 * 内核没有提供机制确保在分配或重映射页面时页面颜色（由这个位表示）保持不变。
 * 当用户页面映射到内核空间时，页面的颜色也可能改变。
 * 我们使用地址空间 VMALLOC_END ... VMALLOC_END + DCACHE_WAY_SIZE * 2
 * 来临时映射一个区域，以便匹配颜色。
 * 
 * 如果数据缓存方式的大小大于页面大小，定义相关的宏。
 */

#if DCACHE_WAY_SIZE > PAGE_SIZE
// 定义缓存别名的位移量
# define DCACHE_ALIAS_ORDER	(DCACHE_WAY_SHIFT - PAGE_SHIFT)
// 定义缓存别名的掩码
# define DCACHE_ALIAS_MASK	(PAGE_MASK & (DCACHE_WAY_SIZE - 1))
// 定义获取缓存别名的宏
# define DCACHE_ALIAS(a)	(((a) & DCACHE_ALIAS_MASK) >> PAGE_SHIFT)
// 定义比较两个地址缓存别名是否相等的宏
# define DCACHE_ALIAS_EQ(a,b)	((((a) ^ (b)) & DCACHE_ALIAS_MASK) == 0)
#else
// 如果不处理缓存别名，将相关宏设置为0
# define DCACHE_ALIAS_ORDER	0
#endif

/*
 * 如果指令缓存方式的大小大于页面大小，定义相关的宏。
 *
 */
#if ICACHE_WAY_SIZE > PAGE_SIZE
// 定义缓存别名的位移量
# define ICACHE_ALIAS_ORDER	(ICACHE_WAY_SHIFT - PAGE_SHIFT)
// 定义缓存别名的掩码
# define ICACHE_ALIAS_MASK	(PAGE_MASK & (ICACHE_WAY_SIZE - 1))
// 定义获取缓存别名的宏
# define ICACHE_ALIAS(a)	(((a) & ICACHE_ALIAS_MASK) >> PAGE_SHIFT)
// 定义比较两个地址缓存别名是否相等的宏
# define ICACHE_ALIAS_EQ(a,b)	((((a) ^ (b)) & ICACHE_ALIAS_MASK) == 0)
#else
// 如果不处理缓存别名，将相关宏设置为0
# define ICACHE_ALIAS_ORDER	0
#endif


#ifdef __ASSEMBLY__

// 如果是在汇编语言中，定义__pgprot宏直接返回x
#define __pgprot(x)	(x)

#else

/*
 * These are used to make use of C type-checking..
 */
/*
 * 这些用于进行C语言类型检查。
 */

/* 页表条目 */
typedef struct { unsigned long pte; } pte_t;		/* page table entry */
/* 页全局目录表条目 */
typedef struct { unsigned long pgd; } pgd_t;		/* PGD table entry */
/* 页面保护类型 */
typedef struct { unsigned long pgprot; } pgprot_t;
/* 页表类型的指针 */
typedef struct page *pgtable_t;

/* 获取pte_t结构体中的pte成员 */
#define pte_val(x)	((x).pte)
/* 获取pgd_t结构体中的pgd成员 */
#define pgd_val(x)	((x).pgd)
/* 获取pgprot_t结构体中的pgprot成员 */
#define pgprot_val(x)	((x).pgprot)

/* 将x值封装为pte_t类型 */
#define __pte(x)	((pte_t) { (x) } )
/* 将x值封装为pgd_t类型 */
#define __pgd(x)	((pgd_t) { (x) } )
/* 将x值封装为pgprot_t类型 */
#define __pgprot(x)	((pgprot_t) { (x) } )

/*
 * Pure 2^n version of get_order
 * Use 'nsau' instructions if supported by the processor or the generic version.
 */
/*
 * 纯2^n版本的get_order
 * 如果处理器支持nsau指令则使用，否则使用通用版本。
 */

#if XCHAL_HAVE_NSA

static inline __attribute_const__ int get_order(unsigned long size)
{
	int lz;
	// 使用内嵌汇编获取size参数减1后右移PAGE_SHIFT位的最高非零位的位置
	asm ("nsau %0, %1" : "=r" (lz) : "r" ((size - 1) >> PAGE_SHIFT));
	// 返回32减去最高非零位的位置，即得到所需的order值
	return 32 - lz;
}

#else

# include <asm-generic/getorder.h>

#endif

struct page;
extern void clear_page(void *page);
extern void copy_page(void *to, void *from);

/*
 * If we have cache aliasing and writeback caches, we might have to do
 * some extra work
 */

#if DCACHE_WAY_SIZE > PAGE_SIZE
extern void clear_user_page(void*, unsigned long, struct page*);
extern void copy_user_page(void*, void*, unsigned long, struct page*);
#else
# define clear_user_page(page, vaddr, pg)	clear_page(page)
# define copy_user_page(to, from, vaddr, pg)	copy_page(to, from)
#endif

/*
 * This handles the memory map.  We handle pages at
 * XCHAL_KSEG_CACHED_VADDR for kernels with 32 bit address space.
 * These macros are for conversion of kernel address, not user
 * addresses.
 */

#define ARCH_PFN_OFFSET		(PLATFORM_DEFAULT_MEM_START >> PAGE_SHIFT)

#define __pa(x)			((unsigned long) (x) - PAGE_OFFSET)
#define __va(x)			((void *)((unsigned long) (x) + PAGE_OFFSET))
#define pfn_valid(pfn)		((pfn) >= ARCH_PFN_OFFSET && ((pfn) - ARCH_PFN_OFFSET) < max_mapnr)
#ifdef CONFIG_DISCONTIGMEM
# error CONFIG_DISCONTIGMEM not supported
#endif

#define virt_to_page(kaddr)	pfn_to_page(__pa(kaddr) >> PAGE_SHIFT)
#define page_to_virt(page)	__va(page_to_pfn(page) << PAGE_SHIFT)
#define virt_addr_valid(kaddr)	pfn_valid(__pa(kaddr) >> PAGE_SHIFT)
#define page_to_phys(page)	(page_to_pfn(page) << PAGE_SHIFT)

#ifdef CONFIG_MMU
#define WANT_PAGE_VIRTUAL
#endif

#endif /* __ASSEMBLY__ */

#define VM_DATA_DEFAULT_FLAGS	(VM_READ | VM_WRITE | VM_EXEC | \
				 VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC)

#include <asm-generic/memory_model.h>
#endif /* _XTENSA_PAGE_H */
