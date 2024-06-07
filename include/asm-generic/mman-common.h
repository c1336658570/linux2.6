#ifndef __ASM_GENERIC_MMAN_COMMON_H
#define __ASM_GENERIC_MMAN_COMMON_H

/*
 Author: Michael S. Tsirkin <mst@mellanox.co.il>, Mellanox Technologies Ltd.
 Based on: asm-xxx/mman.h
*/
// 内核使用该函数创建一个新的线性地址空间。如果一个新的地址空间与一个已经存在的地址空间相邻，
// 并且具有相同权限，俩区间将会合并。如果不能合并，才真正创建一个新的。
// file指定文件，具体文件偏移从offset开始，长度为len字节。如果file是NULL并且offset是0，
// 那么就代表这次映射没有和文件相关，这种情况称为匿名映射。如果指定文件和偏移就称为文件映射。
// addr是可选参数，指定搜索空闲区域的起始地址。
// prot参数指定内存区域中页面的访问权限。权限在asm/mman.h中（X86在asm-generic/
// mman-common.h中。不同体系结构定义不同。
// PROT_READ		对应VM_READ
// PROT_WRITE		对应VM_WRITE
// PROT_EXEC		对应VM_EXEC
// PROT_NONE		不可访问
// flag参数指定VMA的标志，这些标志指定类型并改变映射行为。在asm/mman.h中定义
// MAP_SHARED				映射可被共享
// MAP_PRIVATE			映射不能被共享
// MAP_FIXED				新区间必须开始于指定的地址addr
// MAP_ANONYMOUS		映射不是file-backed的，而是匿名的
// MAP_GROWSDOWN		对应于VM_GROWSDOWN
// MAP_DENYWRITE		对应于VM_DENYWRITE
// MAP_EXECUTABLE		对应于VM_EXECUTABLE
// MAP_LOCKED				对应于VM_LOCKED
// MAP_NORESERVE		不需要为映射保留空间
// MAP_POPULATE			填充页表
// MAP_NONBLOCK			在I/O操作上不堵塞
// 如果系统调用do_map中存在无效参数，那么返回一个负值。否则会在虚拟内存中分配一个合适的新区域
// 新区域会和邻近区域合并，否则内核从vm_area_cachep中分配一个新的vm_area_struct
// 并使用vma_link将新分配的内存区域添加到地址空间的内存区域链表和红黑树中，随后更新内存描述符的total_mm域。
// 然后返回新分配的地址空间的初始地址。
// static inline unsigned long do_mmap(struct file *file, unsigned long addr,
// 	unsigned long len, unsigned long prot,
// 	unsigned long flag, unsigned long offset)

// do_mmap的prot参数的取值
// 读，写，执行权限
#define PROT_READ	0x1		/* page can be read */
#define PROT_WRITE	0x2		/* page can be written */
#define PROT_EXEC	0x4		/* page can be executed */
#define PROT_SEM	0x8		/* page may be used for atomic ops */
// 页面不可访问
#define PROT_NONE	0x0		/* page can not be accessed */
#define PROT_GROWSDOWN	0x01000000	/* mprotect flag: extend change to start of growsdown vma */
#define PROT_GROWSUP	0x02000000	/* mprotect flag: extend change to end of growsup vma */

#define MAP_SHARED	0x01		/* Share changes */
#define MAP_PRIVATE	0x02		/* Changes are private */
#define MAP_TYPE	0x0f		/* Mask for type of mapping */
#define MAP_FIXED	0x10		/* Interpret addr exactly */
#define MAP_ANONYMOUS	0x20		/* don't use a file */
#ifdef CONFIG_MMAP_ALLOW_UNINITIALIZED
# define MAP_UNINITIALIZED 0x4000000	/* For anonymous mmap, memory could be uninitialized */
#else
# define MAP_UNINITIALIZED 0x0		/* Don't support this flag */
#endif

#define MS_ASYNC	1		/* sync memory asynchronously */
#define MS_INVALIDATE	2		/* invalidate the caches */
#define MS_SYNC		4		/* synchronous memory sync */

#define MADV_NORMAL	0		/* no further special treatment */
#define MADV_RANDOM	1		/* expect random page references */
#define MADV_SEQUENTIAL	2		/* expect sequential page references */
#define MADV_WILLNEED	3		/* will need these pages */
#define MADV_DONTNEED	4		/* don't need these pages */

/* common parameters: try to keep these consistent across architectures */
#define MADV_REMOVE	9		/* remove these pages & resources */
#define MADV_DONTFORK	10		/* don't inherit across fork */
#define MADV_DOFORK	11		/* do inherit across fork */
#define MADV_HWPOISON	100		/* poison a page for testing */
#define MADV_SOFT_OFFLINE 101		/* soft offline page for testing */

#define MADV_MERGEABLE   12		/* KSM may merge identical pages */
#define MADV_UNMERGEABLE 13		/* KSM may not merge identical pages */

/* compatibility flags */
#define MAP_FILE	0

#endif /* __ASM_GENERIC_MMAN_COMMON_H */
