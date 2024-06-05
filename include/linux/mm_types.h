#ifndef _LINUX_MM_TYPES_H
#define _LINUX_MM_TYPES_H

#include <linux/auxvec.h>
#include <linux/types.h>
#include <linux/threads.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/prio_tree.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/page-debug-flags.h>
#include <asm/page.h>
#include <asm/mmu.h>

#ifndef AT_VECTOR_SIZE_ARCH
#define AT_VECTOR_SIZE_ARCH 0
#endif
#define AT_VECTOR_SIZE (2*(AT_VECTOR_SIZE_ARCH + AT_VECTOR_SIZE_BASE + 1))

struct address_space;

#define USE_SPLIT_PTLOCKS	(NR_CPUS >= CONFIG_SPLIT_PTLOCK_CPUS)

/*
 * Each physical page in the system has a struct page associated with
 * it to keep track of whatever it is we are using the page for at the
 * moment. Note that we have no way to track which tasks are using
 * a page, though if it is a pagecache page, rmap structures can tell us
 * who is mapping it.
 */
/*
 * 系统中的每个物理页面都有一个与之关联的 struct page 结构，用于跟踪页面当前的使用情况。
 * 需要注意的是，我们没有办法跟踪哪个任务正在使用页面，但是如果它是一个页面缓存页，rmap 结构可以告诉我们谁在映射它。
 */
// struct page结构用于描述每个物理页
struct page {
	// 存放页的状态。包括页是不是脏的，时不是被锁定在内存中。每一位表示一种状态，共32种，标志定义在include/linux/page-flags.h
	unsigned long flags;		/* 原子标志，一些可能会被异步更新 */
	// _count存放页的引用计数——即这个页被引用了多少次（使用次数）。当计数值变为-1时，就说明当前内核没引用它，于是在新分配中就可以使用它。
	// 内核代码通过page_count()函数来检查这个变量，返回0表示空闲，返回一个正整数表示页在使用。
	// 一个页可以由页缓存使用（这时，mapping域指向和这个页关联的address_space对象），或者作为私有数据（由private指向），或者作为进程中的映射。
	atomic_t _count;		/* 使用计数，见下面的说明 */
	union {
		/**
		 * _mapcount表示这个页面被进程映射的个数，即已经映射了多少个用于pte页表：
		 * _mapcount == -1表示没有pte映射到页面；
		 * _mapcount == 0表示只有父进程映射了页面；
		 * _mapcount > 0表示除了父进程外还有其他进程映射了这个页面。
		 * 如果该page处于伙伴系统中，该值为PAGE_BUDDY_MAPCOUNT_VALUE（-128），内核通过判断该值是否为PAGE_BUDDY_MAPCOUNT_VALUE来确定该page是否属于伙伴系统。
		 */
		atomic_t _mapcount;	/* Count of ptes mapped in mms,
					 * to show when page is mapped
					 * & limit reverse map searches.
					 */
		struct {		/* SLUB */
			u16 inuse;			/* 使用中的对象数 */
			u16 objects;		/* 对象总数 */
		};
	};
	union {
	    struct {
		/**
		 * private ：私有数据指针，由应用场景确定其具体的含义：
		 * a：如果设置了PG_private标志，表示buffer_heads；
		 * b：如果设置了PG_swapcache标志，private存储了该page在交换分区中对应的位置信息swp_entry_t。
		 * c：如果_mapcount = PAGE_BUDDY_MAPCOUNT_VALUE，说明该page位于伙伴系统，private存储该伙伴的阶。
		 */
		// page中的private指向第一个buffer_head
		unsigned long private;		/* Mapping-private opaque data:
					 	 * usually used for buffer_heads
						 * if PagePrivate set; used for
						 * swp_entry_t if PageSwapCache;
						 * indicates order in the buddy
						 * system if PG_buddy is set.
						 */
						/* 映射私有的不透明数据：
						 * 如果设置了 PagePrivate，则通常用于 buffer_heads；
						 * 如果设置了 PageSwapCache，则用于 swp_entry_t；
						 * 如果设置了 PG_buddy，则表示在伙伴系统中的顺序。
						 */
		/**
		 * mapping成员表示页面所指向的地址空间。内核中的地址空间通常有两个不通的地址空间，一个用于文件映射页面，
		 * 例如在读取文件时，地址空间用于将文件内容数据与装载数据的存储介质区关联起来。另一个用于匿名映射。
		 * 内核使用了一个简单直接的方式实现了『一个指针，两种用途』，mapping指针地址的最后两位用于判断是
		 * 否指匿名映射或KSM页面的地址空间，如果是匿名页面，那么mapping指向匿名页面的地址空间数据结构struct anon_vma。
		 */
		/**
		 * mapping ：有三种含义
		 * a: 如果mapping = 0，说明该page属于交换缓存（swap cache）；当需要使用地址空间时会指定交换分区的地址空间swapper_space。
		 * b: 如果mapping != 0，bit[0] = 0，说明该page属于页缓存或文件映射，mapping指向文件的地址空间address_space。
		 * c: 如果mapping != 0，bit[0] != 0，说明该page为匿名映射，mapping指向struct anon_vma对象。
		 * 通过mapping恢复anon_vma的方法：anon_vma = (struct anon_vma *)(mapping - PAGE_MAPPING_ANON)。
		*/
		struct address_space *mapping;	/* If low bit clear, points to
						 * inode address_space, or NULL.
						 * If page mapped as anonymous
						 * memory, low bit is set, and
						 * it points to anon_vma object:
						 * see PAGE_MAPPING_ANON below.
						 */
						/* 如果最低位清零，指向 inode 的 address_space，或为 NULL。
						 * 如果页面被映射为匿名内存，则最低位被设置，并且指向 anon_vma 对象：
						 * 见下面的 PAGE_MAPPING_ANON。
						 */
	    };
#if USE_SPLIT_PTLOCKS
	    spinlock_t ptl;	// 于分割页表锁（ptl）的自旋锁。如果定义了 USE_SPLIT_PTLOCKS，则使用该字段。
#endif
	    struct kmem_cache *slab;	/* SLUB：指向 slab 的指针 */	// 指向的是slab缓存描述符
	    struct page *first_page;	/* Compound tail pages */		/* 复合尾页 */
	};
	union {
		/**
		 * index ：在映射的虚拟空间（vma_area）内的偏移；一个文件可能只映射一部分，假设映射了1M的空间，
		 * index指的是在1M空间内的偏移，而不是在整个文件内的偏移。
		 */
		pgoff_t index;		/* Our offset within mapping. */			/* 页面在映射中的偏移量 */
		void *freelist;		/* SLUB: freelist req. slab lock */
	};
	/**
	 * lru ：链表头，主要有3个用途：
	 * a：page处于伙伴系统中时，用于链接相同阶的伙伴（只使用伙伴中的第一个page的lru即可达到目的）。
	 * b：page属于slab时，page->lru.next指向page驻留的的缓存的管理结构，page->lru.prec指向保存该page的slab的管理结构。
	 * c：page被用户态使用或被当做页缓存使用时，用于将该page连入zone中相应的lru链表，供内存回收时使用。
	 */
	struct list_head lru;		/* Pageout list, eg. active_list
					 * protected by zone->lru_lock !
					 */
					/* 页面回写列表，例如 active_list，由 zone->lru_lock 保护！ */
	/*
	 * On machines where all RAM is mapped into kernel address space,
	 * we can simply calculate the virtual address. On machines with
	 * highmem some memory is mapped into kernel virtual memory
	 * dynamically, so we need a place to store that address.
	 * Note that this field could be 16 bits on x86 ... ;)
	 *
	 * Architectures with slow multiplication can define
	 * WANT_PAGE_VIRTUAL in asm/page.h
	 */
	/*
	 * 在将所有 RAM 映射到内核地址空间的机器上，我们可以直接计算虚拟地址。
	 * 在具有高内存的机器上，一些内存是动态映射到内核虚拟内存的，因此我们需要一个地方来存储那个地址。
	 * 注意，在 x86 上，该字段可以是 16 位的... ;)
	 *
	 * 具有缓慢乘法的体系结构可以在 asm/page.h 中定义 WANT_PAGE_VIRTUAL
	 */
#if defined(WANT_PAGE_VIRTUAL)
	// virtual域是页的虚拟地址。通常情况下，它就是页在虚拟内存中的地址。有些内存（即所谓的高端内存）并不永久的映射到内核
	// 地址空间上。这种情况下，这个域为NULL，需要的时候，必须动态地映射这些页。
	void *virtual;			/* Kernel virtual address (NULL if
					   not kmapped, ie. highmem) */	/* 内核虚拟地址（如果没有 kmapped，则为 NULL，即 highmem） */
#endif /* WANT_PAGE_VIRTUAL */
#ifdef CONFIG_WANT_PAGE_DEBUG_FLAGS
	unsigned long debug_flags;	/* Use atomic bitops on this */		/* 在此上执行原子位操作 */
#endif

#ifdef CONFIG_KMEMCHECK
	/*
	 * kmemcheck wants to track the status of each byte in a page; this
	 * is a pointer to such a status block. NULL if not tracked.
	 */
	/*
	 * kmemcheck 希望跟踪页面中每个字节的状态；这是指向这样一个状态块的指针。
	 * 如果没有跟踪，则为 NULL。
	 */
	void *shadow;
#endif
};

/*
 * A region containing a mapping of a non-memory backed file under NOMMU
 * conditions.  These are held in a global tree and are pinned by the VMAs that
 * map parts of them.
 */
/*
 * vm_region 结构描述在无MMU条件下映射非内存后备文件的区域。
 * 这些区域在全局树中维护，并且被映射它们部分的 VMA 所固定。
 */

struct vm_region {
	/* 链接到全球区域树 */
	struct rb_node	vm_rb;		/* link in global region tree */
	/* VMA vm_flags */
	unsigned long	vm_flags;	/* VMA vm_flags */
	/* 区域的起始地址 */
	unsigned long	vm_start;	/* start address of region */
	/* 区域初始化到这里 */
	unsigned long	vm_end;		/* region initialised to here */
	/* 区域分配到这里 */
	unsigned long	vm_top;		/* region allocated to here */
	/* vm_file中对应vm_start的偏移 */
	unsigned long	vm_pgoff;	/* the offset in vm_file corresponding to vm_start */
	/* 后备文件或NULL */
	struct file	*vm_file;	/* the backing file or NULL */

	/* 区域使用计数（在 nommu_region_sem 下访问） */
	int		vm_usage;	/* region usage count (access under nommu_region_sem) */
	/* 如果为此区域刷新了指令缓存，则为true */
	bool		vm_icache_flushed : 1; /* true if the icache has been flushed for
						* this region */
};

/*
 * This struct defines a memory VMM memory area. There is one of these
 * per VM-area/task.  A VM area is any part of the process virtual memory
 * space that has a special rule for the page-fault handlers (ie a shared
 * library, the executable area etc).
 */
/*
 * 这个结构定义了一个内存 VMM 内存区域。每个VM区域/任务有一个这样的结构。
 * VM区域是进程虚拟内存空间的任何部分，它对页面错误处理程序有特殊规则（例如共享库、可执行区域等）。
 */
struct vm_area_struct {
	// 相关的mm_struct
	struct mm_struct * vm_mm;	/* The address space we belong to. */
	// 区间的首地址
	unsigned long vm_start;		/* Our start address within vm_mm. */
	// 区间的尾地址
	unsigned long vm_end;		/* The first byte after our end address
					   within vm_mm. */

	/* linked list of VM areas per task, sorted by address */
	/* 每个任务的 VM 区域的链表，按地址排序 */
	struct vm_area_struct *vm_next;	// VMA 链表

	// 访问控制权限
	pgprot_t vm_page_prot;		/* Access permissions of this VMA. */
	// 标志，在mm.h中
	unsigned long vm_flags;		/* Flags, see mm.h. */

	// 红黑树上该VMA节点
	struct rb_node vm_rb;

	/*
	 * For areas with an address space and backing store,
	 * linkage into the address_space->i_mmap prio tree, or
	 * linkage to the list of like vmas hanging off its node, or
	 * linkage of vma in the address_space->i_mmap_nonlinear list.
	 */
	union {
		// 要么关联address_space->i_mmap字段，要么关联address_space->i_mmap_nonlinear字段。
		struct {
			struct list_head list;
			void *parent;	/* aligns with prio_tree_node parent */
			struct vm_area_struct *head;
		} vm_set;

		struct raw_prio_tree_node prio_tree_node;
	} shared;

	/*
	 * A file's MAP_PRIVATE vma can be in both i_mmap tree and anon_vma
	 * list, after a COW of one of the file pages.	A MAP_SHARED vma
	 * can only be in the i_mmap tree.  An anonymous MAP_PRIVATE, stack
	 * or brk vma (with NULL file) can only be in an anon_vma list.
	 */
	// anon_vma项
	struct list_head anon_vma_chain; /* Serialized by mmap_sem &
					  * page_table_lock */
	// 匿名 VMA 对象
	struct anon_vma *anon_vma;	/* Serialized by page_table_lock */

	/* Function pointers to deal with this struct. */
	// 相关的操作表
	const struct vm_operations_struct *vm_ops;

	/* Information about our backing store: */
	// 文件中的偏移量
	unsigned long vm_pgoff;		/* Offset (within vm_file) in PAGE_SIZE
					   units, *not* PAGE_CACHE_SIZE */
	// 被映射的文件
	struct file * vm_file;		/* File we map to (can be NULL). */
	// 私有数据
	void * vm_private_data;		/* was vm_pte (shared mem) */
	unsigned long vm_truncate_count;/* truncate_count or restart_addr */

#ifndef CONFIG_MMU
	struct vm_region *vm_region;	/* NOMMU mapping region */
#endif
#ifdef CONFIG_NUMA
	struct mempolicy *vm_policy;	/* NUMA policy for the VMA */
#endif
};

/* 核心线程结构，用于管理任务和线程列表 */
struct core_thread {
	// 指向任务结构的指针
	struct task_struct *task;
	// 指向下一个核心线程的指针
	struct core_thread *next;
};

/* 核心状态结构，用于同步线程启动和跟踪线程数量 */
struct core_state {
	// 原子操作的线程数
	atomic_t nr_threads;
	// dumper线程
	struct core_thread dumper;
	// 启动同步结构
	struct completion startup;
};

/* 内存计数器枚举，用于分类不同类型的内存页 */
enum {
	MM_FILEPAGES,		// 文件映射页
	MM_ANONPAGES,		// 匿名页
	MM_SWAPENTS,		// 交换空间实体
	NR_MM_COUNTERS	// 计数器总数
};

#if USE_SPLIT_PTLOCKS && defined(CONFIG_MMU)
#define SPLIT_RSS_COUNTING
/* 多线程安全的内存统计结构 */
struct mm_rss_stat {
	atomic_long_t count[NR_MM_COUNTERS];	// 使用原子长整型进行计数
};
/* per-thread cached information, */
/* 每线程缓存的内存统计信息 */
struct task_rss_stat {
	// 同步阈值事件数
	int events;	/* for synchronization threshold */
	// 内存计数
	int count[NR_MM_COUNTERS];
};
#else  /* !USE_SPLIT_PTLOCKS */
/* 未使用分离页表锁时的内存统计结构 */
struct mm_rss_stat {
	unsigned long count[NR_MM_COUNTERS];	// 内存计数
};
#endif /* !USE_SPLIT_PTLOCKS */

/* 内存管理结构，每个进程有一个 */
struct mm_struct {
	// 内存区域链表
	struct vm_area_struct * mmap;		/* list of VMAs */
	struct rb_root mm_rb;		// VMA形成的红黑树
	// 最近使用的内存区域
	struct vm_area_struct * mmap_cache;	/* last find_vma result */
#ifdef CONFIG_MMU
	unsigned long (*get_unmapped_area) (struct file *filp,
				unsigned long addr, unsigned long len,
				unsigned long pgoff, unsigned long flags);
	void (*unmap_area) (struct mm_struct *mm, unsigned long addr);
#endif
	// 内存映射基地址
	unsigned long mmap_base;		/* base of mmap area */
	// 进程虚拟内存大小
	unsigned long task_size;		/* size of task vm space */
	// 缓存的最大空洞大小
	unsigned long cached_hole_size; 	/* if non-zero, the largest hole below free_area_cache */
	// 地址空间的第一个空洞
	unsigned long free_area_cache;		/* first hole of size cached_hole_size or larger */
	// 页全局目录
	pgd_t * pgd;	// 指向进程的页全局目录
	// 使用地址空间的用户数
	atomic_t mm_users;			/* How many users with user space? */
	// 主使用计数器
	atomic_t mm_count;			/* How many references to "struct mm_struct" (users count as 1) */
	// 内存区域的个数
	int map_count;				/* number of VMAs */
	// 内存区域的信号量
	struct rw_semaphore mmap_sem;
	// 页表锁，操作和检索页表时必须使用该锁。
	spinlock_t page_table_lock;		/* Protects page tables and some counters */

	// 所有mm_struct形成的链表
	struct list_head mmlist;		/* List of maybe swapped mm's.	These are globally strung
						 * together off init_mm.mmlist, and are protected
						 * by mmlist_lock
						 */

	// 所分配的物理页
	unsigned long hiwater_rss;	/* High-watermark of RSS usage */	// 最高水位线的常驻集大小
	unsigned long hiwater_vm;	/* High-water virtual memory usage */	// 最高虚拟内存使用量
	// 全部页面数和上锁页面数
	unsigned long total_vm, locked_vm, shared_vm, exec_vm;
	unsigned long stack_vm, reserved_vm, def_flags, nr_ptes;
	// 代码段开始和结束，数据段的首位地址
	unsigned long start_code, end_code, start_data, end_data;
	// 堆的首尾地址，进程栈的首地址
	unsigned long start_brk, brk, start_stack;
	// 命令行参数的首尾地址，环境变量的首尾地址
	unsigned long arg_start, arg_end, env_start, env_end;

	// 所保存的auxv
	unsigned long saved_auxv[AT_VECTOR_SIZE]; /* for /proc/PID/auxv */

	/*
	 * Special counters, in some configurations protected by the
	 * page_table_lock, in other configurations by being atomic.
	 */
	struct mm_rss_stat rss_stat;	// 内存统计信息

	struct linux_binfmt *binfmt;	// 二进制格式信息

	// 懒惰的TLB交换掩码
	cpumask_t cpu_vm_mask;

	/* Architecture-specific MM context */
	mm_context_t context;	// 体系结构特殊数据

	/* Swap token stuff */
	/*
	 * Last value of global fault stamp as seen by this process.
	 * In other words, this value gives an indication of how long
	 * it has been since this task got the token.
	 * Look at mm/thrash.c
	 */
	unsigned int faultstamp;			// 最后一次错误时间戳
	unsigned int token_priority;	// 令牌优先级
	unsigned int last_interval;		// 最后间隔

	// 状态标志
	unsigned long flags; /* Must use atomic bitops to access the bits */

	// 核心转储的支持
	struct core_state *core_state; /* coredumping support */
#ifdef CONFIG_AIO
	// AIO I/O 链表锁
	spinlock_t		ioctx_lock;
	// AIO I/O 链表
	struct hlist_head	ioctx_list;
#endif
#ifdef CONFIG_MM_OWNER
	/*
	 * "owner" points to a task that is regarded as the canonical
	 * user/owner of this mm. All of the following must be true in
	 * order for it to be changed:
	 *
	 * current == mm->owner
	 * current->mm != mm
	 * new_owner->mm == mm
	 * new_owner->alloc_lock is held
	 */
	struct task_struct *owner;		// 内存管理结构的所有者
#endif

#ifdef CONFIG_PROC_FS
	/* store ref to file /proc/<pid>/exe symlink points to */
	struct file *exe_file;							// proc/<pid>/exe 链接到的文件
	unsigned long num_exe_file_vmas;		// exe文件关联的内存区数量
#endif
#ifdef CONFIG_MMU_NOTIFIER
	struct mmu_notifier_mm *mmu_notifier_mm;	// MMU通知器
#endif
};

/* Future-safe accessor for struct mm_struct's cpu_vm_mask. */
/* 安全访问 mm_struct 中的 cpu_vm_mask 的宏定义 */
#define mm_cpumask(mm) (&(mm)->cpu_vm_mask)

#endif /* _LINUX_MM_TYPES_H */
