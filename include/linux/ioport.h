/*
 * ioport.h	Definitions of routines for detecting, reserving and
 *		allocating system resources.
 *
 * Authors:	Linus Torvalds
 */

#ifndef _LINUX_IOPORT_H
#define _LINUX_IOPORT_H

#ifndef __ASSEMBLY__
#include <linux/compiler.h>
#include <linux/types.h>
/*
 * Resources are tree-like, allowing
 * nesting etc..
 */
/*
 * 资源具有树形结构，允许嵌套等操作。
 */
struct resource {
	resource_size_t start;  // 资源的起始地址
	resource_size_t end;    // 资源的结束地址
	const char *name;       // 资源的名称
	unsigned long flags;    // 资源标志，用于表示资源类型或状态
	struct resource *parent, *sibling, *child;  // 指向父资源、兄弟资源和子资源的指针
};

struct resource_list {
	struct resource_list *next;  // 指向下一个资源列表项的指针
	struct resource *res;        // 指向资源的指针
	struct pci_dev *dev;         // 指向PCI设备的指针，如果资源与特定的PCI设备相关联
};

/*
 * IO resources have these defined flags.
 */
/*
 * I/O资源拥有这些定义的标志。
 */
// 用于总线特定位
#define IORESOURCE_BITS		0x000000ff	/* Bus-specific bits */

// 资源类型
#define IORESOURCE_TYPE_BITS	0x00001f00	/* Resource type */
#define IORESOURCE_IO		0x00000100	/* I/O 端口资源 */
#define IORESOURCE_MEM		0x00000200	/* 内存资源 */
#define IORESOURCE_IRQ		0x00000400	/* 中断资源 */
#define IORESOURCE_DMA		0x00000800	/* DMA 资源 */
#define IORESOURCE_BUS		0x00001000	/* 总线资源 */

// 不引起副作用
#define IORESOURCE_PREFETCH	0x00002000	/* No side effects */
#define IORESOURCE_READONLY	0x00004000	/* 只读资源 */
#define IORESOURCE_CACHEABLE	0x00008000	/* 可缓存资源 */
#define IORESOURCE_RANGELENGTH	0x00010000	/* 范围长度 */
#define IORESOURCE_SHADOWABLE	0x00020000	/* 可阴影资源 */

// kernel/resource.c中resource_alignment函数使用，IORESOURCE_SIZEALIGN表示按资源结构体对齐，IORESOURCE_STARTALIGN表示按资源起始地址对齐
// size 指定对齐
#define IORESOURCE_SIZEALIGN	0x00040000	/* size indicates alignment */
// start 字段是对齐基准
#define IORESOURCE_STARTALIGN	0x00080000	/* start field is alignment */

#define IORESOURCE_MEM_64	0x00100000	/* 64位内存资源 */
// 由桥接转发的资源
#define IORESOURCE_WINDOW	0x00200000	/* forwarded by bridge */

// 用户空间不可映射此资源
#define IORESOURCE_EXCLUSIVE	0x08000000	/* Userland may not map this resource */
#define IORESOURCE_DISABLED	0x10000000	/* 资源被禁用 */
#define IORESOURCE_UNSET	0x20000000	/* 资源未设置 */
#define IORESOURCE_AUTO		0x40000000	/* 自动配置资源 */
// 驱动程序已标记此资源为忙碌
#define IORESOURCE_BUSY		0x80000000	/* Driver has marked this resource busy */

/* 解释：
 * IORESOURCE_IRQ_HIGHEDGE - 中断请求在信号的上升沿（从低电平跳变到高电平）被触发。
 * IORESOURCE_IRQ_LOWEDGE - 中断请求在信号的下降沿（从高电平跳变到低电平）被触发。
 * IORESOURCE_IRQ_HIGHLEVEL - 中断请求在信号持续为高电平时被持续触发。
 * IORESOURCE_IRQ_LOWLEVEL - 中断请求在信号持续为低电平时被持续触发。
 * IORESOURCE_IRQ_SHAREABLE - 表明此中断可以被多个设备共享，不是专用于单一设备。
 * IORESOURCE_IRQ_OPTIONAL - 表明此中断对于系统的运作不是必须的，可能在某些配置或硬件上不可用。
 */
/* PnP IRQ specific bits (IORESOURCE_BITS) */
/* PnP IRQ特定位 (IORESOURCE_BITS) */
#define IORESOURCE_IRQ_HIGHEDGE		(1<<0)  // 高电平触发边沿
#define IORESOURCE_IRQ_LOWEDGE		(1<<1)  // 低电平触发边沿
#define IORESOURCE_IRQ_HIGHLEVEL	(1<<2)  // 高电平触发水平
#define IORESOURCE_IRQ_LOWLEVEL		(1<<3)  // 低电平触发水平
#define IORESOURCE_IRQ_SHAREABLE	(1<<4)  // 中断可以共享
#define IORESOURCE_IRQ_OPTIONAL 	(1<<5)  // 中断是可选的，即不是所有系统都需要这种中断

/* 解释：
 * IORESOURCE_DMA_TYPE_MASK - 用于从配置值中提取DMA类型的掩码。
 * IORESOURCE_DMA_8BIT - 指定DMA传输应为8位宽。
 * IORESOURCE_DMA_8AND16BIT - 指定DMA控制器支持8位和16位宽的传输。
 * IORESOURCE_DMA_16BIT - 指定DMA传输应为16位宽。
 * 
 * IORESOURCE_DMA_MASTER - 指定DMA控制器在传输时作为主控。
 * IORESOURCE_DMA_BYTE - 指定DMA传输的基本单位为字节。
 * IORESOURCE_DMA_WORD - 指定DMA传输的基本单位为字（通常是16位或32位，取决于架构）。
 * 
 * IORESOURCE_DMA_SPEED_MASK - 用于从配置值中提取DMA速度的掩码。
 * IORESOURCE_DMA_COMPATIBLE - 指定DMA控制器应工作在标准的、与老设备兼容的模式下。
 * IORESOURCE_DMA_TYPEA - 指定DMA控制器应工作在较高性能的A类模式下。
 * IORESOURCE_DMA_TYPEB - 指定DMA控制器应工作在更高性能的B类模式下。
 * IORESOURCE_DMA_TYPEF - 指定DMA控制器应工作在最高性能的F类模式下。
 */
/* PnP DMA specific bits (IORESOURCE_BITS) */
/* PnP DMA特定位 (IORESOURCE_BITS) */
#define IORESOURCE_DMA_TYPE_MASK	(3<<0)  // DMA类型掩码，用于提取DMA类型相关的两位
#define IORESOURCE_DMA_8BIT		(0<<0)  // 8位DMA
#define IORESOURCE_DMA_8AND16BIT	(1<<0)  // 8位和16位共用DMA
#define IORESOURCE_DMA_16BIT		(2<<0)  // 16位DMA

#define IORESOURCE_DMA_MASTER		(1<<2)  // DMA控制器作为主控
#define IORESOURCE_DMA_BYTE		(1<<3)  // 字节宽度的DMA传输
#define IORESOURCE_DMA_WORD		(1<<4)  // 字宽的DMA传输

#define IORESOURCE_DMA_SPEED_MASK	(3<<6)  // DMA速度掩码，用于提取DMA速度类型相关的两位
#define IORESOURCE_DMA_COMPATIBLE	(0<<6)  // 标准速度，兼容模式
#define IORESOURCE_DMA_TYPEA		(1<<6)  // A类速度，增强模式
#define IORESOURCE_DMA_TYPEB		(2<<6)  // B类速度，进一步增强
#define IORESOURCE_DMA_TYPEF		(3<<6)  // F类速度，最高性能

/* 解释：
 * IORESOURCE_MEM_WRITEABLE - 指定内存区域是否可写。
 * IORESOURCE_MEM_CACHEABLE - 指定内存区域是否可以被缓存。
 * IORESOURCE_MEM_RANGELENGTH - 指定是否使用特定的内存范围长度。
 * IORESOURCE_MEM_TYPE_MASK - 用于从配置值中提取内存类型的掩码。
 * IORESOURCE_MEM_8BIT - 指定内存宽度为8位。
 * IORESOURCE_MEM_16BIT - 指定内存宽度为16位。
 * IORESOURCE_MEM_8AND16BIT - 指定内存宽度可以是8位或16位。
 * IORESOURCE_MEM_32BIT - 指定内存宽度为32位。
 * IORESOURCE_MEM_SHADOWABLE - 指定内存区域可以被影子复制，常用于BIOS。
 * IORESOURCE_MEM_EXPANSIONROM - 指定内存区域是用于存放扩展ROM的。
 */
/* PnP memory I/O specific bits (IORESOURCE_BITS) */
/* PnP内存I/O特定位 (IORESOURCE_BITS) */
//  重复：IORESOURCE_READONLY，表示内存是否可写
#define IORESOURCE_MEM_WRITEABLE	(1<<0)	/* dup: IORESOURCE_READONLY */
// 重复：IORESOURCE_CACHEABLE，表示内存是否可缓存
#define IORESOURCE_MEM_CACHEABLE	(1<<1)	/* dup: IORESOURCE_CACHEABLE */
// 重复：IORESOURCE_RANGELENGTH，表示内存长度信息
#define IORESOURCE_MEM_RANGELENGTH	(1<<2)	/* dup: IORESOURCE_RANGELENGTH */
#define IORESOURCE_MEM_TYPE_MASK	(3<<3)	/* 内存类型掩码，用于提取内存宽度类型相关的两位 */
#define IORESOURCE_MEM_8BIT		(0<<3)	/* 8位内存 */
#define IORESOURCE_MEM_16BIT		(1<<3)	/* 16位内存 */
#define IORESOURCE_MEM_8AND16BIT	(2<<3)	/* 8位和16位共用内存 */
#define IORESOURCE_MEM_32BIT		(3<<3)	/* 32位内存 */
// 重复：IORESOURCE_SHADOWABLE，表示内存是否可以被影子复制
#define IORESOURCE_MEM_SHADOWABLE	(1<<5)	/* dup: IORESOURCE_SHADOWABLE */
#define IORESOURCE_MEM_EXPANSIONROM	(1<<6)	/* 表示内存区域是一个扩展ROM */

/* 解释：
 * IORESOURCE_IO_16BIT_ADDR - 用于标识资源地址宽度为16位的I/O端口。
 * IORESOURCE_IO_FIXED - 表明I/O资源位置是固定的，通常用于设备需要固定地址访问的情况。
 * 
 * IORESOURCE_ROM_ENABLE - 启用ROM资源的标志，常用于PCI设备的固件存储。
 * IORESOURCE_ROM_SHADOW - 指示ROM内容被复制到指定的内存地址，通常用于快速访问。
 * IORESOURCE_ROM_COPY - 表明该ROM资源有一个分配的复制，通常用于修改不安全或不方便直接修改的原始ROM内容。
 * IORESOURCE_ROM_BIOS_COPY - 表示ROM是BIOS的复制，用于在不影响原始BIOS的情况下进行测试或其他目的。
 *
 * IORESOURCE_PCI_FIXED - 用于PCI设备，标识资源位置不应被操作系统动态修改或移动。
 * 
 * ioport_resource 和 iomem_resource - 这两个资源是系统范围内的I/O端口和内存资源的代表，所有的I/O和内存资源分配都通过这两个对象进行管理。
 */
/* PnP I/O specific bits (IORESOURCE_BITS) */
/* PnP I/O特定位 (IORESOURCE_BITS) */
#define IORESOURCE_IO_16BIT_ADDR	(1<<0)	// 指示地址是16位的
#define IORESOURCE_IO_FIXED		(1<<1)	// I/O资源固定不变

/* PCI ROM control bits (IORESOURCE_BITS) */
/* PCI ROM控制位 (IORESOURCE_BITS) */
// 启用ROM，与PCI_ROM_ADDRESS_ENABLE相同
#define IORESOURCE_ROM_ENABLE		(1<<0)	/* ROM is enabled, same as PCI_ROM_ADDRESS_ENABLE */
// ROM内容被复制到C000:0
#define IORESOURCE_ROM_SHADOW		(1<<1)	/* ROM is copy at C000:0 */
// ROM有一个分配的复制，资源字段被覆盖
#define IORESOURCE_ROM_COPY		(1<<2)	/* ROM is alloc'd copy, resource field overlaid */
// ROM是BIOS的复制，资源字段被覆盖
#define IORESOURCE_ROM_BIOS_COPY	(1<<3)	/* ROM is BIOS copy, resource field overlaid */

/* PCI control bits.  Shares IORESOURCE_BITS with above PCI ROM.  */
/* PCI控制位。与上面的PCI ROM共享IORESOURCE_BITS。 */
// 资源位置固定，不可移动
#define IORESOURCE_PCI_FIXED		(1<<4)	/* Do not move resource */

/* PC/ISA/whatever - the normal PC address spaces: IO and memory */
/* PC/ISA/其他 - 正常PC地址空间：I/O和内存 */
extern struct resource ioport_resource;	// 全局I/O端口资源对象
extern struct resource iomem_resource;	// 全局内存资源对象

/* 解释：
 * request_resource_conflict - 请求资源，返回可能的冲突资源。
 * request_resource - 请求资源，成功返回0，失败返回负值。
 * release_resource - 释放资源，成功返回0，失败返回负值。
 * release_child_resources - 释放指定资源的所有子资源。
 * reserve_region_with_split - 预留一块区域，如果该区域内已有部分资源被占用，则进行分割处理。
 * insert_resource_conflict - 尝试在父资源下插入一个新资源，如果有冲突返回冲突的资源。
 * insert_resource - 在父资源下插入一个新资源，成功返回0，失败返回负值。
 * insert_resource_expand_to_fit - 插入资源，如有必要，扩展父资源以确保新资源可以完全适应。
 * allocate_resource - 分配资源，考虑大小、边界、对齐等条件，alignf为自定义对齐函数，alignf_data为对齐函数的额外数据。
 * adjust_resource - 调整资源的位置和大小。
 * resource_alignment - 用于获取资源的对齐方式。
 * 
 * 这些函数是资源管理的基础，保证了系统资源的有效分配和使用，防止资源冲突，提高系统的稳定性和效率。
 */
/* 请求资源，可能会存在冲突 */
extern struct resource *request_resource_conflict(struct resource *root, struct resource *new);
/* 请求系统资源 */
extern int request_resource(struct resource *root, struct resource *new);
/* 释放已分配的资源 */
extern int release_resource(struct resource *new);
/* 释放子资源 */
void release_child_resources(struct resource *new);
/* 在特定区域内预留资源，并且在需要时进行分割 */
extern void reserve_region_with_split(struct resource *root,
			     resource_size_t start, resource_size_t end,
			     const char *name);
/* 插入资源并处理冲突 */
extern struct resource *insert_resource_conflict(struct resource *parent, struct resource *new);
/* 插入一个资源 */
extern int insert_resource(struct resource *parent, struct resource *new);
/* 扩展并插入资源以适应指定的大小 */
extern void insert_resource_expand_to_fit(struct resource *root, struct resource *new);
/* 分配资源，可以指定大小、最小/最大范围、对齐等 */
extern int allocate_resource(struct resource *root, struct resource *new,
			     resource_size_t size, resource_size_t min,
			     resource_size_t max, resource_size_t align,
			     resource_size_t (*alignf)(void *,
						       const struct resource *,
						       resource_size_t,
						       resource_size_t),
			     void *alignf_data);
/* 调整资源的起始位置和大小 */
int adjust_resource(struct resource *res, resource_size_t start,
		    resource_size_t size);
/* 计算资源的对齐方式 */
resource_size_t resource_alignment(struct resource *res);
/* 解释：
 * resource_size - 计算并返回资源占用的大小。
 * resource_type - 返回资源的类型，如内存或I/O。
 * request_region - 请求一段I/O端口资源。
 * __request_mem_region - 请求一段内存资源，可以指定是否排他。
 * request_mem_region - 请求内存资源，不指定排他性。
 * request_mem_region_exclusive - 请求内存资源，指定排他性。
 * rename_region - 重命名一个资源。
 * __request_region - 实现资源请求的底层函数，支持更细致的控制，如起始地址、长度和名称。
 * 
 * 这些宏和函数对于管理和配置系统中的硬件资源非常关键，特别是在驱动程序和系统初始化代码中广泛使用。
 */
/* 计算资源的大小 */
static inline resource_size_t resource_size(const struct resource *res)
{
	return res->end - res->start + 1;
}
/* 返回资源的类型 */
static inline unsigned long resource_type(const struct resource *res)
{
	return res->flags & IORESOURCE_TYPE_BITS;
}

/* Convenience shorthand with allocation */
/* 快捷方式来分配I/O端口资源 */
#define request_region(start,n,name)	__request_region(&ioport_resource, (start), (n), (name), 0)
/* 快捷方式来请求内存资源，带有排他性标志 */
#define __request_mem_region(start,n,name, excl) __request_region(&iomem_resource, (start), (n), (name), excl)
/* 请求内存资源，不包含排他性 */
#define request_mem_region(start,n,name) __request_region(&iomem_resource, (start), (n), (name), 0)
/* 请求内存资源，包含排他性 */
#define request_mem_region_exclusive(start,n,name) \
	__request_region(&iomem_resource, (start), (n), (name), IORESOURCE_EXCLUSIVE)
	/* 重命名资源 */
#define rename_region(region, newname) do { (region)->name = (newname); } while (0)

/* 请求一个资源区间，这可能涉及到硬件I/O端口或内存资源 */
extern struct resource * __request_region(struct resource *,
					resource_size_t start,
					resource_size_t n,
					const char *name, int flags);

/* 解释：
 * release_region - 释放一定范围内的I/O端口资源。
 * check_mem_region - 检查一定范围内的内存资源是否被占用，用于确保资源使用不冲突。
 * release_mem_region - 释放一定范围内的内存资源。
 * __check_region - 底层函数，用于检查资源是否被占用。
 * __release_region - 底层函数，用于释放资源。
 * check_region - 过时的函数，用于检查I/O端口资源，现在推荐使用更新的函数。
 *
 * 这些定义和函数是系统资源管理的基础，确保资源能被正确地分配和释放，避免资源冲突和泄漏。
 */
/* Compatibility cruft */
/* 宏定义用于释放I/O端口资源 */
#define release_region(start,n)	__release_region(&ioport_resource, (start), (n))
/* 宏定义用于检查内存区域是否已被占用 */
#define check_mem_region(start,n)	__check_region(&iomem_resource, (start), (n))
/* 宏定义用于释放内存区域资源 */
#define release_mem_region(start,n)	__release_region(&iomem_resource, (start), (n))

/* 检查资源是否被占用的底层实现函数 */
extern int __check_region(struct resource *, resource_size_t, resource_size_t);
/* 释放资源的底层实现函数 */
extern void __release_region(struct resource *, resource_size_t,
				resource_size_t);

/* 用于检查I/O端口资源是否被占用的过时函数，已标记为不推荐使用 */
static inline int __deprecated check_region(resource_size_t s,
						resource_size_t n)
{
	return __check_region(&ioport_resource, s, n);
}

/* 解释：
 * devm_request_region 和 devm_request_mem_region 用于请求和分配资源，并自动在设备卸载时释放资源，简化了驱动开发人员处理资源管理的复杂性。
 * __devm_request_region 和 __devm_release_region 是底层实现，通常不直接由驱动程序调用。
 * iomem_map_sanity_check 和 iomem_is_exclusive 用于内存资源的检查和安全性验证，确保资源的合理分配。
 * walk_system_ram_range 为系统内存操作提供了遍历功能，允许对系统内存执行复杂的查询或修改操作。
 */
/* Wrappers for managed devices */
struct device;
/* 设备管理的资源请求宏，请求I/O端口资源 */
#define devm_request_region(dev,start,n,name) \
	__devm_request_region(dev, &ioport_resource, (start), (n), (name))
/* 设备管理的资源请求宏，请求内存资源 */
#define devm_request_mem_region(dev,start,n,name) \
	__devm_request_region(dev, &iomem_resource, (start), (n), (name))

/* 实际处理设备管理资源请求的函数 */
extern struct resource * __devm_request_region(struct device *dev,
				struct resource *parent, resource_size_t start,
				resource_size_t n, const char *name);

/* 设备管理的资源释放宏，释放I/O端口资源 */
#define devm_release_region(dev, start, n) \
	__devm_release_region(dev, &ioport_resource, (start), (n))
/* 设备管理的资源释放宏，释放内存资源 */
#define devm_release_mem_region(dev, start, n) \
	__devm_release_region(dev, &iomem_resource, (start), (n))

/* 实际处理设备管理资源释放的函数 */
extern void __devm_release_region(struct device *dev, struct resource *parent,
				  resource_size_t start, resource_size_t n);
/* 内存映射有效性检查 */
extern int iomem_map_sanity_check(resource_size_t addr, unsigned long size);
/* 检查内存区域是否被独占 */
extern int iomem_is_exclusive(u64 addr);

/* 遍历系统内存范围并执行特定功能 */
extern int
walk_system_ram_range(unsigned long start_pfn, unsigned long nr_pages,
		void *arg, int (*func)(unsigned long, unsigned long, void *));

#endif /* __ASSEMBLY__ */
#endif	/* _LINUX_IOPORT_H */
