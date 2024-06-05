/*
 * fs/sysfs/sysfs.h - sysfs internal header file
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 *
 * This file is released under the GPLv2.
 */

#include <linux/lockdep.h>
#include <linux/fs.h>

struct sysfs_open_dirent;

/* type-specific structures for sysfs_dirent->s_* union members */
/* 特定类型的结构体，用于 sysfs_dirent->s_* 联合成员 */
struct sysfs_elem_dir {
	struct kobject		*kobj;	// 指向 kobject 对象的指针
	/* children list starts here and goes through sd->s_sibling */
	/* 子目录列表从这里开始，通过 sd->s_sibling 链接 */
	struct sysfs_dirent	*children;	// 子目录项
};

struct sysfs_elem_symlink {
	struct sysfs_dirent	*target_sd;	// 目标 sysfs_dirent 的符号链接
};

struct sysfs_elem_attr {
	struct attribute	*attr;		// 属性
	struct sysfs_open_dirent *open;	// 打开的 dirent
};

struct sysfs_elem_bin_attr {
	struct bin_attribute	*bin_attr;	// 二进制属性
	struct hlist_head	buffers;	// 缓冲区链表头
};

struct sysfs_inode_attrs {
	struct iattr	ia_iattr;	// inode 属性
	void		*ia_secdata;		// 安全模块使用的数据
	u32		ia_secdata_len;		// 安全数据的长度
};

/*
 * sysfs_dirent - the building block of sysfs hierarchy.  Each and
 * every sysfs node is represented by single sysfs_dirent.
 *
 * As long as s_count reference is held, the sysfs_dirent itself is
 * accessible.  Dereferencing s_elem or any other outer entity
 * requires s_active reference.
 */
/*
 * sysfs_dirent - sysfs层级结构的构建块。每一个 sysfs 节点都由单个 sysfs_dirent 表示。
 *
 * 只要持有 s_count 引用，sysfs_dirent 本身就是可访问的。要解引用 s_elem 或任何其他外部实体，
 * 需要 s_active 引用。
 */
struct sysfs_dirent {
	atomic_t		s_count;		// 引用计数
	atomic_t		s_active;		// 活跃引用计数
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;	// 锁依赖映射，用于调试
#endif
	struct sysfs_dirent	*s_parent;	// 父目录项
	struct sysfs_dirent	*s_sibling;	// 兄弟目录项
	const char		*s_name;					// 目录项名称

	union {
		struct sysfs_elem_dir		s_dir;				// 目录类型
		struct sysfs_elem_symlink	s_symlink;	// 符号链接类型
		struct sysfs_elem_attr		s_attr;			// 属性类型
		struct sysfs_elem_bin_attr	s_bin_attr;	// 二进制属性类型
	};

	unsigned int		s_flags;		// 标志
	unsigned short		s_mode;		// 文件模式 (权限)
	ino_t			s_ino;						// inode 号
	struct sysfs_inode_attrs *s_iattr;	// inode 属性
};

#define SD_DEACTIVATED_BIAS		INT_MIN

#define SYSFS_TYPE_MASK			0x00ff
#define SYSFS_DIR			0x0001
#define SYSFS_KOBJ_ATTR			0x0002
#define SYSFS_KOBJ_BIN_ATTR		0x0004
#define SYSFS_KOBJ_LINK			0x0008
#define SYSFS_COPY_NAME			(SYSFS_DIR | SYSFS_KOBJ_LINK)
#define SYSFS_ACTIVE_REF		(SYSFS_KOBJ_ATTR | SYSFS_KOBJ_BIN_ATTR)

#define SYSFS_FLAG_MASK			~SYSFS_TYPE_MASK
#define SYSFS_FLAG_REMOVED		0x0200

static inline unsigned int sysfs_type(struct sysfs_dirent *sd)
{
	return sd->s_flags & SYSFS_TYPE_MASK;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
#define sysfs_dirent_init_lockdep(sd)				\
do {								\
	struct attribute *attr = sd->s_attr.attr;		\
	struct lock_class_key *key = attr->key;			\
	if (!key)						\
		key = &attr->skey;				\
								\
	lockdep_init_map(&sd->dep_map, "s_active", key, 0);	\
} while(0)
#else
#define sysfs_dirent_init_lockdep(sd) do {} while(0)
#endif

/*
 * Context structure to be used while adding/removing nodes.
 */
struct sysfs_addrm_cxt {
	struct sysfs_dirent	*parent_sd;
	struct sysfs_dirent	*removed;
};

/*
 * mount.c
 */
extern struct sysfs_dirent sysfs_root;
extern struct kmem_cache *sysfs_dir_cachep;

/*
 * dir.c
 */
extern struct mutex sysfs_mutex;
extern spinlock_t sysfs_assoc_lock;

extern const struct file_operations sysfs_dir_operations;
extern const struct inode_operations sysfs_dir_inode_operations;

struct dentry *sysfs_get_dentry(struct sysfs_dirent *sd);
struct sysfs_dirent *sysfs_get_active(struct sysfs_dirent *sd);
void sysfs_put_active(struct sysfs_dirent *sd);
void sysfs_addrm_start(struct sysfs_addrm_cxt *acxt,
		       struct sysfs_dirent *parent_sd);
int __sysfs_add_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd);
int sysfs_add_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd);
void sysfs_remove_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd);
void sysfs_addrm_finish(struct sysfs_addrm_cxt *acxt);

struct sysfs_dirent *sysfs_find_dirent(struct sysfs_dirent *parent_sd,
				       const unsigned char *name);
struct sysfs_dirent *sysfs_get_dirent(struct sysfs_dirent *parent_sd,
				      const unsigned char *name);
struct sysfs_dirent *sysfs_new_dirent(const char *name, umode_t mode, int type);

void release_sysfs_dirent(struct sysfs_dirent *sd);

int sysfs_create_subdir(struct kobject *kobj, const char *name,
			struct sysfs_dirent **p_sd);
void sysfs_remove_subdir(struct sysfs_dirent *sd);

int sysfs_rename(struct sysfs_dirent *sd,
	struct sysfs_dirent *new_parent_sd, const char *new_name);

static inline struct sysfs_dirent *__sysfs_get(struct sysfs_dirent *sd)
{
	if (sd) {
		WARN_ON(!atomic_read(&sd->s_count));
		atomic_inc(&sd->s_count);
	}
	return sd;
}
#define sysfs_get(sd) __sysfs_get(sd)

static inline void __sysfs_put(struct sysfs_dirent *sd)
{
	if (sd && atomic_dec_and_test(&sd->s_count))
		release_sysfs_dirent(sd);
}
#define sysfs_put(sd) __sysfs_put(sd)

/*
 * inode.c
 */
struct inode *sysfs_get_inode(struct super_block *sb, struct sysfs_dirent *sd);
void sysfs_delete_inode(struct inode *inode);
int sysfs_sd_setattr(struct sysfs_dirent *sd, struct iattr *iattr);
int sysfs_permission(struct inode *inode, int mask);
int sysfs_setattr(struct dentry *dentry, struct iattr *iattr);
int sysfs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat);
int sysfs_setxattr(struct dentry *dentry, const char *name, const void *value,
		size_t size, int flags);
int sysfs_hash_and_remove(struct sysfs_dirent *dir_sd, const char *name);
int sysfs_inode_init(void);

/*
 * file.c
 */
extern const struct file_operations sysfs_file_operations;

int sysfs_add_file(struct sysfs_dirent *dir_sd,
		   const struct attribute *attr, int type);

int sysfs_add_file_mode(struct sysfs_dirent *dir_sd,
			const struct attribute *attr, int type, mode_t amode);
/*
 * bin.c
 */
extern const struct file_operations bin_fops;
void unmap_bin_file(struct sysfs_dirent *attr_sd);

/*
 * symlink.c
 */
extern const struct inode_operations sysfs_symlink_inode_operations;
