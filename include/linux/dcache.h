#ifndef __LINUX_DCACHE_H
#define __LINUX_DCACHE_H

#include <asm/atomic.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/spinlock.h>
#include <linux/cache.h>
#include <linux/rcupdate.h>

struct nameidata;
struct path;
struct vfsmount;

/*
 * linux/include/linux/dcache.h
 *
 * Dirent cache data structures
 *
 * (C) Copyright 1997 Thomas Schoebel-Theuer,
 * with heavy changes by Linus Torvalds
 */

#define IS_ROOT(x) ((x) == (x)->d_parent)

/*
 * "quick string" -- eases parameter passing, but more importantly
 * saves "metadata" about the string (ie length and the hash).
 *
 * hash comes first so it snuggles against d_parent in the
 * dentry.
 */
struct qstr {
	unsigned int hash;
	unsigned int len;
	const unsigned char *name;
};

struct dentry_stat_t {
	int nr_dentry;
	int nr_unused;
	int age_limit;          /* age in seconds */
	int want_pages;         /* pages requested by system */
	int dummy[2];
};
extern struct dentry_stat_t dentry_stat;

/* Name hashing routines. Initial hash value */
/* Hash courtesy of the R5 hash in reiserfs modulo sign bits */
#define init_name_hash()		0

/* partial hash update function. Assume roughly 4 bits per character */
static inline unsigned long
partial_name_hash(unsigned long c, unsigned long prevhash)
{
	return (prevhash + (c << 4) + (c >> 4)) * 11;
}

/*
 * Finally: cut down the number of bits to a int value (and try to avoid
 * losing bits)
 */
static inline unsigned long end_name_hash(unsigned long hash)
{
	return (unsigned int) hash;
}

/* Compute the hash for a name string. */
static inline unsigned int
full_name_hash(const unsigned char *name, unsigned int len)
{
	unsigned long hash = init_name_hash();
	while (len--)
		hash = partial_name_hash(*name++, hash);
	return end_name_hash(hash);
}

/*
 * Try to keep struct dentry aligned on 64 byte cachelines (this will
 * give reasonable cacheline footprint with larger lines without the
 * large memory footprint increase).
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
	atomic_t d_count;		/* 使用计数 */
	unsigned int d_flags;		/* protected by d_lock */		/* 目录项标识 */
	spinlock_t d_lock;		/* per dentry lock */		/* 单目录项锁 */
	/* 表示dentry是否是一个挂载点，如果是挂载点，该成员不为0 */
	int d_mounted;		/* 是否是挂载点 */
	// inode节点的指针，便于快速找到对应的索引节点
	struct inode *d_inode;		/* Where the name belongs to - NULL is
					 * negative */		/* 相关联的索引节点 */
	/*
	 * The next three fields are touched by __d_lookup.  Place them here
	 * so they all fit in a cache line.
	 */
	/* 链接到dentry_hashtable的hash链表 */
	// dentry_hashtable哈希表维护在内存中的所有目录项，哈希表中每个元素都是一个双向循环链表，
	// 用于维护哈希值相等的目录项，这个双向循环链表是通过dentry中的d_hash成员来链接在一起的。
	struct hlist_node d_hash;	/* lookup hash list */		/* 散列表 */
	/* 指向父dentry结构的指针 */  
	struct dentry *d_parent;	/* parent directory */		/* 父目录的目录项对象 */
	// 文件名  
	struct qstr d_name;		/* 目录项名称 */

	struct list_head d_lru;		/* LRU list */	/* 未使用的链表 */
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
	struct list_head d_subdirs;	/* our children */		/* 子目录链表 */
	struct list_head d_alias;	/* inode alias list */	/* 索引节点别名链表 */
	unsigned long d_time;		/* used by d_revalidate */	/* 重置时间 */
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
enum dentry_d_lock_class
{
	DENTRY_D_LOCK_NORMAL, /* implicitly used by plain spin_lock() APIs. */
	DENTRY_D_LOCK_NESTED
};

/* 目录项相关操作函数 */
// 其中包括内核针对特定目录所能调用的方法,比如d_compare()和d_delete()等方法
struct dentry_operations {
	/* 该函数判断目录项对象是否有效。VFS准备从dcache中使用一个目录项时会调用这个函数，
	 * 大部分文件系统将其置为NULL，因为它们认为dcache(缓存，即dentry_hashtable)目录项对象总是有效的 */
	int (*d_revalidate)(struct dentry *, struct nameidata *);
	/* 为目录项对象生成散列值（hash值），当目录项需要加入到散列表中时，VFS调用该函数 */
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
locking rules:
		big lock	dcache_lock	d_lock   may block
d_revalidate:	no		no		no       yes
d_hash		no		no		no       yes
d_compare:	no		yes		yes      no
d_delete:	no		yes		no       no
d_release:	no		no		no       yes
d_iput:		no		no		no       yes
 */

/* d_flags entries */
#define DCACHE_AUTOFS_PENDING 0x0001    /* autofs: "under construction" */
#define DCACHE_NFSFS_RENAMED  0x0002    /* this dentry has been "silly
					 * renamed" and has to be
					 * deleted on the last dput()
					 */
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

#define DCACHE_REFERENCED	0x0008  /* Recently used, don't discard. */
#define DCACHE_UNHASHED		0x0010	

#define DCACHE_INOTIFY_PARENT_WATCHED	0x0020 /* Parent inode is watched by inotify */

#define DCACHE_COOKIE		0x0040	/* For use by dcookie subsystem */

#define DCACHE_FSNOTIFY_PARENT_WATCHED	0x0080 /* Parent inode is watched by some fsnotify listener */

#define DCACHE_CANT_MOUNT	0x0100

extern spinlock_t dcache_lock;
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

static inline void __d_drop(struct dentry *dentry)
{
	if (!(dentry->d_flags & DCACHE_UNHASHED)) {
		dentry->d_flags |= DCACHE_UNHASHED;
		hlist_del_rcu(&dentry->d_hash);
	}
}

static inline void d_drop(struct dentry *dentry)
{
	spin_lock(&dcache_lock);
	spin_lock(&dentry->d_lock);
 	__d_drop(dentry);
	spin_unlock(&dentry->d_lock);
	spin_unlock(&dcache_lock);
}

static inline int dname_external(struct dentry *dentry)
{
	return dentry->d_name.name != dentry->d_iname;
}

/*
 * These are the low-level FS interfaces to the dcache..
 */
extern void d_instantiate(struct dentry *, struct inode *);
extern struct dentry * d_instantiate_unique(struct dentry *, struct inode *);
extern struct dentry * d_materialise_unique(struct dentry *, struct inode *);
extern void d_delete(struct dentry *);

/* allocate/de-allocate */
extern struct dentry * d_alloc(struct dentry *, const struct qstr *);
extern struct dentry * d_splice_alias(struct inode *, struct dentry *);
extern struct dentry * d_add_ci(struct dentry *, struct inode *, struct qstr *);
extern struct dentry * d_obtain_alias(struct inode *);
extern void shrink_dcache_sb(struct super_block *);
extern void shrink_dcache_parent(struct dentry *);
extern void shrink_dcache_for_umount(struct super_block *);
extern int d_invalidate(struct dentry *);

/* only used at mount-time */
extern struct dentry * d_alloc_root(struct inode *);

/* <clickety>-<click> the ramfs-type tree */
extern void d_genocide(struct dentry *);

extern struct dentry *d_find_alias(struct inode *);
extern void d_prune_aliases(struct inode *);

/* test whether we have any submounts in a subdir tree */
extern int have_submounts(struct dentry *);

/*
 * This adds the entry to the hash queues.
 */
extern void d_rehash(struct dentry *);

/**
 * d_add - add dentry to hash queues
 * @entry: dentry to add
 * @inode: The inode to attach to this dentry
 *
 * This adds the entry to the hash queues and initializes @inode.
 * The entry was actually filled in earlier during d_alloc().
 */
 
static inline void d_add(struct dentry *entry, struct inode *inode)
{
	d_instantiate(entry, inode);
	d_rehash(entry);
}

/**
 * d_add_unique - add dentry to hash queues without aliasing
 * @entry: dentry to add
 * @inode: The inode to attach to this dentry
 *
 * This adds the entry to the hash queues and initializes @inode.
 * The entry was actually filled in earlier during d_alloc().
 */
static inline struct dentry *d_add_unique(struct dentry *entry, struct inode *inode)
{
	struct dentry *res;

	res = d_instantiate_unique(entry, inode);
	d_rehash(res != NULL ? res : entry);
	return res;
}

/* used for rename() and baskets */
extern void d_move(struct dentry *, struct dentry *);
extern struct dentry *d_ancestor(struct dentry *, struct dentry *);

/* appendix may either be NULL or be used for transname suffixes */
extern struct dentry * d_lookup(struct dentry *, struct qstr *);
extern struct dentry * __d_lookup(struct dentry *, struct qstr *);
extern struct dentry * d_hash_and_lookup(struct dentry *, struct qstr *);

/* validate "insecure" dentry pointer */
extern int d_validate(struct dentry *, struct dentry *);

/*
 * helper function for dentry_operations.d_dname() members
 */
extern char *dynamic_dname(struct dentry *, char *, int, const char *, ...);

extern char *__d_path(const struct path *path, struct path *root, char *, int);
extern char *d_path(const struct path *, char *, int);
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
 
static inline struct dentry *dget(struct dentry *dentry)
{
	if (dentry) {
		BUG_ON(!atomic_read(&dentry->d_count));
		atomic_inc(&dentry->d_count);
	}
	return dentry;
}

extern struct dentry * dget_locked(struct dentry *);

/**
 *	d_unhashed -	is dentry hashed
 *	@dentry: entry to check
 *
 *	Returns true if the dentry passed is not currently hashed.
 */
 
static inline int d_unhashed(struct dentry *dentry)
{
	return (dentry->d_flags & DCACHE_UNHASHED);
}

static inline int d_unlinked(struct dentry *dentry)
{
	return d_unhashed(dentry) && !IS_ROOT(dentry);
}

static inline int cant_mount(struct dentry *dentry)
{
	return (dentry->d_flags & DCACHE_CANT_MOUNT);
}

static inline void dont_mount(struct dentry *dentry)
{
	spin_lock(&dentry->d_lock);
	dentry->d_flags |= DCACHE_CANT_MOUNT;
	spin_unlock(&dentry->d_lock);
}

static inline struct dentry *dget_parent(struct dentry *dentry)
{
	struct dentry *ret;

	spin_lock(&dentry->d_lock);
	ret = dget(dentry->d_parent);
	spin_unlock(&dentry->d_lock);
	return ret;
}

extern void dput(struct dentry *);

static inline int d_mountpoint(struct dentry *dentry)
{
	return dentry->d_mounted;
}

extern struct vfsmount *lookup_mnt(struct path *);
extern struct dentry *lookup_create(struct nameidata *nd, int is_dir);

extern int sysctl_vfs_cache_pressure;

#endif	/* __LINUX_DCACHE_H */
