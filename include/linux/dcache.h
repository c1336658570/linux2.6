#ifndef __LINUX_DCACHE_H
#define __LINUX_DCACHE_H

#include <asm/atomic.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/spinlock.h>
#include <linux/cache.h>
#include <linux/rcupdate.h>

struct nameidata;	// 定义了nameidata结构体，通常用于路径查找操作中
struct path;			// 定义了path结构体，用于表示文件系统中的一个路径
struct vfsmount;	// 定义了vfsmount结构体，表示文件系统的挂载点

/*
 * linux/include/linux/dcache.h
 *
 * Dirent cache data structures
 *
 * (C) Copyright 1997 Thomas Schoebel-Theuer,
 * with heavy changes by Linus Torvalds
 */
/*
 * linux/include/linux/dcache.h
 *
 * 目录项缓存（Dirent cache）数据结构
 *
 * (C) 版权1997 Thomas Schoebel-Theuer,
 * 经过Linus Torvalds的重大修改
 */

// 宏定义，用于判断给定的目录项是否是根目录项
#define IS_ROOT(x) ((x) == (x)->d_parent)

/*
 * "quick string" -- eases parameter passing, but more importantly
 * saves "metadata" about the string (ie length and the hash).
 *
 * hash comes first so it snuggles against d_parent in the
 * dentry.
 */
/*
 * “快速字符串”（quick string）-- 简化参数传递，并且更重要的是，
 * 保存了字符串的“元数据”（即长度和哈希值）。
 *
 * 哈希值位于首位，因此它可以紧靠在dentry的d_parent成员旁边。
 */
struct qstr {
	unsigned int hash;		// 字符串的哈希值，用于快速比较和查找
	unsigned int len;			// 字符串的长度，便于处理和优化存储
	const unsigned char *name;	// 指向字符串的指针，存储实际字符串数据
};

// 目录项统计结构，用于监控目录项缓存的状态和性能
struct dentry_stat_t {
	int nr_dentry;          // 目录项数量，当前缓存中的目录项总数
	int nr_unused;          // 未使用的目录项数量，表示当前未被任何文件系统引用的目录项数
	int age_limit;          // 目录项的年龄限制（秒），超过此时间的目录项可能会被回收
	int want_pages;         // 系统请求的页面数量，表示目录项缓存请求更多内存页面的次数
	int dummy[2];           // 预留字段，为将来可能的扩展保留空间
};
extern struct dentry_stat_t dentry_stat;	// 声明一个全局的目录项统计结构

/* Name hashing routines. Initial hash value */
/* Hash courtesy of the R5 hash in reiserfs modulo sign bits */
/* 名称哈希函数的初始化值 */
#define init_name_hash()		0

/* partial hash update function. Assume roughly 4 bits per character */
/* 部分哈希更新函数。假设每个字符大约占4位 */
static inline unsigned long
partial_name_hash(unsigned long c, unsigned long prevhash)
{
	// 计算部分哈希值，c是当前字符，prevhash是之前的哈希值
	// 通过位操作和乘法更新哈希值，以达到一个分散和快速的哈希算法
	return (prevhash + (c << 4) + (c >> 4)) * 11;
}

/*
 * Finally: cut down the number of bits to a int value (and try to avoid
 * losing bits)
 */
/*
 * 最后：减少位数到一个整数值（尝试避免丢失位）
 */
static inline unsigned long end_name_hash(unsigned long hash)
{
	// 将长整型哈希值转换为无符号整型，尽量减少位的丢失
	return (unsigned int) hash;
}

/* Compute the hash for a name string. */
/* 计算字符串名称的哈希值 */
static inline unsigned int
full_name_hash(const unsigned char *name, unsigned int len)
{
	// 对整个字符串名称计算哈希值
	unsigned long hash = init_name_hash();  // 初始化哈希值
	while (len--)  // 遍历每个字符
		hash = partial_name_hash(*name++, hash);  // 更新哈希值
	return end_name_hash(hash);  // 最后调整哈希值并返回
}

/*
 * Try to keep struct dentry aligned on 64 byte cachelines (this will
 * give reasonable cacheline footprint with larger lines without the
 * large memory footprint increase).
 */

/*
 * 尝试使结构体dentry在64字节缓存行上对齐（这将在不增加
 * 大量内存占用的情况下，对于更大的缓存行提供合理的缓存行足迹）。
 */
#ifdef CONFIG_64BIT
#define DNAME_INLINE_LEN_MIN 32 /* 192 bytes */
#else
#define DNAME_INLINE_LEN_MIN 40 /* 128 bytes */
#endif

/* 目录项对象结构 */
struct dentry {
	/**
	 * 每个目录项对象都有3种状态：被使用，未使用和负状态
	 * 被使用：对应一个有效的索引节点（d_inode指向相应的索引节点），并且该对象由一个或多个使用者(d_count为正值)
	 * 未使用：对应一个有效的索引节点，但是VFS当前并没有使用这个目录项(d_count为0)
	 * 负状态：没有对应的有效索引节点（d_inode为NULL），因为索引节点被删除或者路径不存在了，但目录项仍然保留，以便快速解析以后的路径查询。
	 */
	// 目录项对象引用计数器  
	atomic_t d_count;		/* 使用计数，管理目录项被引用的次数 */
	unsigned int d_flags;		/* protected by d_lock */		/* 目录项标识，受d_lock保护 */
	spinlock_t d_lock;		/* per dentry lock */		/* 单目录项锁，用于保护目录项的并发访问 */
	/* 表示dentry是否是一个挂载点，如果是挂载点，该成员不为0 */
	int d_mounted;		/* 是否是挂载点 */
	// inode节点的指针，便于快速找到对应的索引节点
	struct inode *d_inode;		/* Where the name belongs to - NULL is
					 * negative */		/* 相关联的索引节点，NULL表示此目录项为负 */
	/*
	 * The next three fields are touched by __d_lookup.  Place them here
	 * so they all fit in a cache line.
	 */
	/* 链接到dentry_hashtable的hash链表 */
	// dentry_hashtable哈希表维护在内存中的所有目录项，哈希表中每个元素都是一个双向循环链表，
	// 用于维护哈希值相等的目录项，这个双向循环链表是通过dentry中的d_hash成员来链接在一起的。
	// 利用d_lookup函数查找散列表，如果该函数在dcache中发现了匹配的目录项对象，则匹配对象被返回，
	// 否则，返回NULL指针。
	struct hlist_node d_hash;	/* lookup hash list */		/* lookup hash list，链接到dentry_hashtable的hash链表 */
	/* 指向父dentry结构的指针 */  
	struct dentry *d_parent;	/* parent directory */		/* parent directory，指向父dentry结构的指针 */
	// 文件名  
	struct qstr d_name;		/* 目录项名称，存储文件或目录的名称 */

	struct list_head d_lru;		/* LRU list */	/* LRU list，用于挂接到未使用目录项的链表 */
	/*
	 * d_child and d_rcu can share memory
	 */
	union {
		struct list_head d_child;	/* child of parent list */	/* 目录项内部形成的链表 */
	 	struct rcu_head d_rcu;		/* RCU加锁 */
	} d_u;
	/* 是子项的链表头，子项可能是目录也可能是文件，所有子项都要链接到这个链表， */ 
	// 某目录的d_subdirs与该目录下所有文件的d_child成员一起形成一个双向循环链表，
	// 将该目录下的所有文件连接在一起，目的是保留文件的目录结构，即一个d_subdirs和
	// 多个d_child一起形成链表，d_subdirs对应文件在d_child对应文件的上一层目录。
	struct list_head d_subdirs;	/* our children */		/* 子目录链表，链接该目录下的所有子目录项 */
	// d_alias会插入到对应inode的i_dentry链表中
	struct list_head d_alias;	/* inode alias list */	/* 索引节点别名链表 */
	unsigned long d_time;		/* used by d_revalidate */	/* 重置时间，用于目录项有效性验证的时间戳 */
	// 指向dentry对应的操作函数集
	const struct dentry_operations *d_op;	/* 目录项操作相关函数 */
	// 指向对应超级块的指针
	struct super_block *d_sb;	/* The root of the dentry tree */	/* 文件的超级块 */
	void *d_fsdata;			/* fs-specific data */	/* 文件系统特有数据 */

	unsigned char d_iname[DNAME_INLINE_LEN_MIN];	/* small names */	/* 短文件名 */
};

/*
 * dentry->d_lock spinlock nesting subclasses:
 *
 * 0: normal
 * 1: nested
 */
/*
 * dentry的d_lock自旋锁的嵌套子类：
 *
 * 0: 正常
 * 1: 嵌套
 */
enum dentry_d_lock_class
{
	// 正常模式：通常由简单的spin_lock() API隐式使用。
	// 用于标准的自旋锁操作，没有额外的嵌套或特殊处理。
	DENTRY_D_LOCK_NORMAL, /* implicitly used by plain spin_lock() APIs. */

	// 嵌套模式：用于可能发生锁嵌套的情况。
	// 此类别允许在已经持有一个dentry锁的情况下，再次请求相同的或不同的dentry锁，避免死锁。
	DENTRY_D_LOCK_NESTED
};

/* 目录项相关操作函数 */
// 其中包括内核针对特定目录所能调用的方法,比如d_compare()和d_delete()等方法
struct dentry_operations {
	/* 该函数判断目录项对象是否有效。VFS准备从dcache中使用一个目录项时会调用这个函数，
	 * 大部分文件系统将其置为NULL，因为它们认为dcache(缓存，即dentry_hashtable)目录项对象总是有效的 */
	int (*d_revalidate)(struct dentry *, struct nameidata *);
	/* 为目录项对象生成散列值（hash值），当目录项需要加入到散列表中时，VFS调用该函数 */
	// 散列表为dentry_hashtable，通过该函数计算散列值。
	int (*d_hash) (struct dentry *, struct qstr *);
	/* VFS调用该函数来比较name1和name2这两个文件名。多数文件系统使用VFS的默认操作，仅仅作字符串比较。使用该函数时需加dcache_lock锁 */
	int (*d_compare) (struct dentry *, struct qstr *name1, struct qstr *name2);
	/* 当目录项对象的 d_count 为0时，VFS调用这个函数。使用该函数时需加dcache_lock锁和目录项的d_lock */
	int (*d_delete)(struct dentry *);
	/* 当目录项对象将要被释放时，VFS调用该函数,该函数默认什么也不做 */
	void (*d_release)(struct dentry *);
	/* 
	 * 当目录项对象丢失其索引节点时（也就是磁盘索引节点被删除了），VFS会调用该函数。默认VFS会调用input()释放索引节点。
	 * 如果文件系统重载了该函数，那么除了执行此文件系统特殊的工作外，还必须调用input()函数。
	 */
	void (*d_iput)(struct dentry *, struct inode *);
	/* 自定义函数用于生成目录项的显示名，通常在调试中使用，用来打印目录项的路径等信息。 */
	char *(*d_dname)(struct dentry *, char *, int);
};

/* the dentry parameter passed to d_hash and d_compare is the parent
 * directory of the entries to be compared. It is used in case these
 * functions need any directory specific information for determining
 * equivalency classes.  Using the dentry itself might not work, as it
 * might be a negative dentry which has no information associated with
 * it */
/*
 * 传递给d_hash和d_compare的dentry参数是要比较的条目的父目录。
 * 这是为了在这些函数需要特定于目录的信息来确定等价类时使用。
 * 直接使用dentry本身可能不起作用，因为它可能是一个负dentry，与之关联的信息为零。
 */

/*
locking rules:
		big lock	dcache_lock	d_lock   may block
d_revalidate:	no		no		no       yes
d_hash		no		no		no       yes
d_compare:	no		yes		yes      no
d_delete:	no		yes		no       no
d_release:	no		no		no       yes
d_iput:		no		no		no       yes
 */
/*
 * 锁定规则：
 *             大锁    dcache锁    d锁   可能阻塞
 * d_revalidate:  否      否        否      是
 * d_hash:        否      否        否      是
 * d_compare:     否      是        是      否
 * d_delete:      否      是        否      否
 * d_release:     否      否        否      是
 * d_iput:        否      否        否      是
 */

/* d_flags entries */
/* d_flags 条目定义 */
/* 自动文件系统正在构建中，表示一个自动挂载的文件系统条目正在处理中 */
/* autofs: "正在构建" */
#define DCACHE_AUTOFS_PENDING 0x0001    /* autofs: "under construction" */
#define DCACHE_NFSFS_RENAMED  0x0002    /* this dentry has been "silly
					 * renamed" and has to be
					 * deleted on the last dput()
					 */
/* NFS文件系统中的条目被“愚蠢地重命名”，这种重命名通常是临时的，需要在最后一次对该条目的引用计数减少到0时删除它 */
/* 这个目录项已经被“愚蠢地重命名”，并且必须在最后一次dput()操作时删除 */

#define	DCACHE_DISCONNECTED 0x0004
     /* This dentry is possibly not currently connected to the dcache tree,
      * in which case its parent will either be itself, or will have this
      * flag as well.  nfsd will not use a dentry with this bit set, but will
      * first endeavour to clear the bit either by discovering that it is
      * connected, or by performing lookup operations.   Any filesystem which
      * supports nfsd_operations MUST have a lookup function which, if it finds
      * a directory inode with a DCACHE_DISCONNECTED dentry, will d_move
      * that dentry into place and return that dentry rather than the passed one,
      * typically using d_splice_alias.
      */
/* 
 * 这个目录项可能当前不连接到dcache树上，这种情况下它的父目录项可能是它自己，或者也有同样的标志位。NFS服务器不会使用带有这个标志位的目录项，
 * 而是首先尝试通过发现它是否已连接，或者执行查找操作来清除这个标志位。任何支持nfsd_operations的文件系统都必须有一个查找函数，
 * 如果找到一个带有DCACHE_DISCONNECTED的目录项，它将移动该目录项到位并返回它，而不是传递的那个目录项，通常使用d_splice_alias。
 */
/* 此目录项可能当前未连接到dcache树，
 * 在这种情况下，它的父节点将是它本身，或者也将有这个标志。
 * nfsd不会使用设置了此位的目录项，但会首先尝试通过发现它是否已连接，
 * 或者通过执行查找操作来清除该位。任何支持nfsd_operations的文件系统
 * 必须具有查找函数，该函数在发现具有DCACHE_DISCONNECTED目录项的目录inode时，
 * 将d_move该目录项到位并返回该目录项而不是传递的目录项，通常使用d_splice_alias。
 */

/* 标记为最近使用过，不应被丢弃 */
#define DCACHE_REFERENCED	0x0008  /* Recently used, don't discard. */
/* 此目录项未被哈希，不在哈希表中，通常表示已从目录项缓存中移除 */
#define DCACHE_UNHASHED		0x0010	

/* 父节点inode被inotify监视 */
#define DCACHE_INOTIFY_PARENT_WATCHED	0x0020 /* Parent inode is watched by inotify */

/* 供dcookie子系统使用，通常用于内核与用户空间的通信 */
#define DCACHE_COOKIE		0x0040	/* For use by dcookie subsystem */

/* 父inode被某些fsnotify监听器监视，用于文件系统事件通知 */
#define DCACHE_FSNOTIFY_PARENT_WATCHED	0x0080 /* Parent inode is watched by some fsnotify listener */

/* 此目录项不能被用作挂载点 */
#define DCACHE_CANT_MOUNT	0x0100

/* 外部声明一个自旋锁，用于保护目录项缓存的操作，防止多线程并发访问问题 */
extern spinlock_t dcache_lock;
/* 外部声明的顺序锁，用于重命名操作时的同步，保证重命名操作的原子性 */
extern seqlock_t rename_lock;

/**
 * d_drop - drop a dentry
 * @dentry: dentry to drop
 *
 * d_drop() unhashes the entry from the parent dentry hashes, so that it won't
 * be found through a VFS lookup any more. Note that this is different from
 * deleting the dentry - d_delete will try to mark the dentry negative if
 * possible, giving a successful _negative_ lookup, while d_drop will
 * just make the cache lookup fail.
 *
 * d_drop() is used mainly for stuff that wants to invalidate a dentry for some
 * reason (NFS timeouts or autofs deletes).
 *
 * __d_drop requires dentry->d_lock.
 */
/* d_drop - 删除一个目录项
 * @dentry: 要删除的目录项
 *
 * d_drop() 函数会从父目录项的哈希表中取消哈希这个目录项，因此它不会再通过VFS查找被找到。
 * 注意，这与删除目录项不同——d_delete会尝试将目录项标记为负（如果可能），从而使负查找成功，
 * 而d_drop只会使缓存查找失败。
 *
 * d_drop() 主要用于某些原因需要使目录项无效的情况（如NFS超时或autofs删除）。
 *
 * __d_drop 要求拥有 dentry->d_lock。
 */
static inline void __d_drop(struct dentry *dentry)
{
	if (!(dentry->d_flags & DCACHE_UNHASHED)) {  // 检查目录项是否已被取消哈希
		dentry->d_flags |= DCACHE_UNHASHED;     // 设置目录项的DCACHE_UNHASHED标志位
		hlist_del_rcu(&dentry->d_hash);         // 使用RCU（读-复制-更新）安全地从哈希表中删除目录项
	}
}

// d_drop 用于安全地从目录项哈希表中移除一个目录项
static inline void d_drop(struct dentry *dentry)
{
	spin_lock(&dcache_lock);             // 获取dcache全局锁，确保目录项缓存的线程安全
	spin_lock(&dentry->d_lock);          // 获取特定目录项的锁，保护该目录项的修改操作
	__d_drop(dentry);                    // 调用__d_drop函数来取消哈希目录项
	spin_unlock(&dentry->d_lock);        // 释放目录项的锁
	spin_unlock(&dcache_lock);           // 释放dcache全局锁
}

// dname_external 用于检查目录项的名字是否存储在外部（即非内嵌在目录项结构本身中）
static inline int dname_external(struct dentry *dentry)
{
	/* 检查目录项的名称是否存储在外部
   * dentry->d_name.name 是目录项名称的指针
   * dentry->d_iname 是目录项内部的固定大小的名称存储空间
   * 如果名称指针不等于内部存储的地址，那么名称被存储在外部
   */
	return dentry->d_name.name != dentry->d_iname;
}

/*
 * These are the low-level FS interfaces to the dcache..
 */
/* 这些是文件系统与dcache（目录项缓存）交互的低级接口 */

/* 将inode关联到未关联inode的dentry */
extern void d_instantiate(struct dentry *, struct inode *);
/* 在给定的superblock中为inode寻找一个唯一的dentry，如果不存在则创建一个新的 */
extern struct dentry * d_instantiate_unique(struct dentry *, struct inode *);
/* 类似于d_instantiate_unique，但用于特殊情况，如网络文件系统 */
/* 为特定的inode创建或寻找一个唯一的dentry */
extern struct dentry * d_materialise_unique(struct dentry *, struct inode *);
/* 标记dentry为已删除 */
extern void d_delete(struct dentry *);

/* allocate/de-allocate */
/* 分配/释放 */
/* 分配一个新的dentry */
extern struct dentry * d_alloc(struct dentry *, const struct qstr *);
/* 将inode和dentry关联，如果inode已有别名dentry，使用已有的 */
extern struct dentry * d_splice_alias(struct inode *, struct dentry *);
/* 将inode与一个大小写敏感的名称关联 */
extern struct dentry * d_add_ci(struct dentry *, struct inode *, struct qstr *);
/* 为孤立的inode生成一个新的dentry */
extern struct dentry * d_obtain_alias(struct inode *);
/* 减小指定superblock的dcache大小 */
extern void shrink_dcache_sb(struct super_block *);
/* 收缩一个dentry的所有子dentry */
extern void shrink_dcache_parent(struct dentry *);
/* 卸载时清除指定superblock的dentry缓存 */
extern void shrink_dcache_for_umount(struct super_block *);
/* 使一个dentry无效 */
extern int d_invalidate(struct dentry *);

/* only used at mount-time */
/* 为给定的inode分配一个根目录项 */
extern struct dentry * d_alloc_root(struct inode *);

/* <clickety>-<click> the ramfs-type tree */
/* 删除指定dentry及其所有子dentry，用于清除ramfs类型的树 */
extern void d_genocide(struct dentry *);

/* 查找给定inode的一个别名dentry */
extern struct dentry *d_find_alias(struct inode *);
/* 清理inode的所有别名dentry */
extern void d_prune_aliases(struct inode *);

/* test whether we have any submounts in a subdir tree */
/* 检查给定的目录项下是否有挂载点 */
extern int have_submounts(struct dentry *);

/*
 * This adds the entry to the hash queues.
 */
/* 将条目添加到哈希队列 */
/* 将dentry重新加入哈希表，使其可通过路径查找 */
extern void d_rehash(struct dentry *);

/**
 * d_add - add dentry to hash queues
 * @entry: dentry to add
 * @inode: The inode to attach to this dentry
 *
 * This adds the entry to the hash queues and initializes @inode.
 * The entry was actually filled in earlier during d_alloc().
 */
 /* d_add - 将dentry添加到哈希队列
 * @entry: 要添加的dentry
 * @inode: 要附加到此dentry的inode
 *
 * 这将条目添加到哈希队列并初始化@inode。
 * 该条目实际上是在之前的d_alloc()过程中填充的。
 */
static inline void d_add(struct dentry *entry, struct inode *inode)
{
	d_instantiate(entry, inode);  // 实例化dentry与inode的关联
	d_rehash(entry);              // 将dentry添加到哈希队列中，使其可以被查找
}

/**
 * d_add_unique - add dentry to hash queues without aliasing
 * @entry: dentry to add
 * @inode: The inode to attach to this dentry
 *
 * This adds the entry to the hash queues and initializes @inode.
 * The entry was actually filled in earlier during d_alloc().
 */
/* d_add_unique - 将dentry添加到哈希队列中，不允许别名
 * @entry: 要添加的dentry
 * @inode: 要附加到此dentry的inode
 *
 * 这将条目添加到哈希队列并初始化@inode。
 * 该条目实际上是在之前的d_alloc()过程中填充的。
 */
static inline struct dentry *d_add_unique(struct dentry *entry, struct inode *inode)
{
	struct dentry *res; // 用于存储返回结果

	res = d_instantiate_unique(entry, inode); // 实例化一个唯一的dentry，确保无别名
	d_rehash(res != NULL ? res : entry);      // 重新哈希处理，如果res非空，则处理res，否则处理entry
	return res;                               // 返回处理结果
}

/* used for rename() and baskets */
/* 用于rename()和移动目录项操作 */
extern void d_move(struct dentry *, struct dentry *);
/* 查找dentry的祖先，如果找到匹配则返回，否则返回NULL */
extern struct dentry *d_ancestor(struct dentry *, struct dentry *);

/* appendix may either be NULL or be used for transname suffixes */
/* 如果在dcache（缓存，即dentry_hashtable）中发现了与其匹配的目录项对象，则匹配的对象被返回，否则返回NULL。 */
// appendix可以是NULL，也可以用于transname后缀
extern struct dentry * d_lookup(struct dentry *, struct qstr *);
/* 在目录项哈希表中查找与给定名字匹配的目录项，使用优化的查找方法 */
extern struct dentry * __d_lookup(struct dentry *, struct qstr *);
/* 先对给定名字进行哈希操作，然后在目录项哈希表中查找 */
extern struct dentry * d_hash_and_lookup(struct dentry *, struct qstr *);

/* validate "insecure" dentry pointer */
/* 验证一个可能不安全的dentry指针，确保它指向有效的内存 */
extern int d_validate(struct dentry *, struct dentry *);

/*
 * helper function for dentry_operations.d_dname() members
 */
/*
 * 辅助函数，用于dentry_operations.d_dname()成员
 */
extern char *dynamic_dname(struct dentry *, char *, int, const char *, ...);

/* 从给定的根目录开始，生成并返回完整路径字符串 */
extern char *__d_path(const struct path *path, struct path *root, char *, int);
/* 生成并返回完整的路径字符串 */
extern char *d_path(const struct path *, char *, int);
/* 生成并返回给定目录项的路径字符串 */
extern char *dentry_path(struct dentry *, char *, int);

/* Allocation counts.. */

/**
 *	dget, dget_locked	-	get a reference to a dentry
 *	@dentry: dentry to get a reference to
 *
 *	Given a dentry or %NULL pointer increment the reference count
 *	if appropriate and return the dentry. A dentry will not be 
 *	destroyed when it has references. dget() should never be
 *	called for dentries with zero reference counter. For these cases
 *	(preferably none, functions in dcache.c are sufficient for normal
 *	needs and they take necessary precautions) you should hold dcache_lock
 *	and call dget_locked() instead of dget().
 */
/* 获取一个目录项的引用
 * @dentry: 要获取引用的目录项
 *
 * 给定一个目录项或NULL指针，如果适当，增加引用计数并返回目录项。
 * 当目录项有引用时不会被销毁。dget()不应该被用于引用计数为零的目录项。
 * 在这些情况下（最好没有这种情况，dcache.c中的函数通常足够正常使用并且已经采取了必要的预防措施），
 * 你应该持有dcache_lock并调用dget_locked()而不是dget()。
 */
 
static inline struct dentry *dget(struct dentry *dentry)
{
	if (dentry) {
		BUG_ON(!atomic_read(&dentry->d_count)); // 确保dentry的引用计数不为零
		atomic_inc(&dentry->d_count);           // 原子地增加引用计数
	}
	return dentry;                            // 返回目录项
}

/* dget_locked: 在持有dcache_lock的情况下安全地获取一个目录项的引用 */
extern struct dentry * dget_locked(struct dentry *);

/**
 *	d_unhashed -	is dentry hashed
 *	@dentry: entry to check
 *
 *	Returns true if the dentry passed is not currently hashed.
 */
/* d_unhashed - 检查目录项是否已取消哈希
 * @dentry: 要检查的目录项
 *
 * 如果传入的目录项当前未被哈希，则返回true。
 */
static inline int d_unhashed(struct dentry *dentry)
{
	return (dentry->d_flags & DCACHE_UNHASHED); // 检查目录项的标志位中是否设置了DCACHE_UNHASHED
}

/* 检查目录项是否未链接到任何目录并且不是根目录 */
static inline int d_unlinked(struct dentry *dentry)
{
	return d_unhashed(dentry) && !IS_ROOT(dentry); // 如果目录项未哈希并且不是根目录则返回true
}

/* 检查是否不能在目录项上进行挂载操作 */
static inline int cant_mount(struct dentry *dentry)
{
	return (dentry->d_flags & DCACHE_CANT_MOUNT); // 检查目录项的标志位中是否设置了DCACHE_CANT_MOUNT
}

/* 设置目录项的状态为不能挂载 */
static inline void dont_mount(struct dentry *dentry)
{
	spin_lock(&dentry->d_lock);                  // 锁定目录项以进行线程安全的操作
	dentry->d_flags |= DCACHE_CANT_MOUNT;        // 设置DCACHE_CANT_MOUNT标志位
	spin_unlock(&dentry->d_lock);                // 解锁目录项
}

/* 安全地获取目录项的父目录项的引用 */
static inline struct dentry *dget_parent(struct dentry *dentry)
{
	struct dentry *ret;

	spin_lock(&dentry->d_lock);                  // 锁定目录项以进行线程安全的操作
	ret = dget(dentry->d_parent);                // 获取父目录项的引用
	spin_unlock(&dentry->d_lock);                // 解锁目录项
	return ret;                                  // 返回父目录项
}

/* 释放一个目录项的引用 */
extern void dput(struct dentry *);

/* 检查目录项是否是一个挂载点 */
static inline int d_mountpoint(struct dentry *dentry)
{
	return dentry->d_mounted; // 返回目录项的挂载点标志
}

/* 在给定的路径上查找挂载点 */
extern struct vfsmount *lookup_mnt(struct path *);

/* 在给定的上下文中查找或创建一个目录项，用于文件或目录的创建 */
extern struct dentry *lookup_create(struct nameidata *nd, int is_dir);

/* 文件系统缓存压力的系统控制变量，调整其值可以影响系统缓存行为 */
extern int sysctl_vfs_cache_pressure;

#endif	/* __LINUX_DCACHE_H */
