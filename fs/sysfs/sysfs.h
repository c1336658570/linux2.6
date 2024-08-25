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

// 定义了一个用于偏移量计算的最小整数值，通常用于标记一个 sysfs_dirent 为非活跃状态
#define SD_DEACTIVATED_BIAS        INT_MIN

// 用于掩码的定义，这是用来从 s_flags 字段中提取出类型信息的位掩码
#define SYSFS_TYPE_MASK            0x00ff
// 目录项类型的标识符
#define SYSFS_DIR                  0x0001
// 普通属性文件类型的标识符
#define SYSFS_KOBJ_ATTR            0x0002
// 二进制属性文件类型的标识符
#define SYSFS_KOBJ_BIN_ATTR        0x0004
// 链接类型的标识符
#define SYSFS_KOBJ_LINK            0x0008
// 需要复制名称的类型组合
#define SYSFS_COPY_NAME            (SYSFS_DIR | SYSFS_KOBJ_LINK)
// 标记为活跃引用的类型组合
#define SYSFS_ACTIVE_REF           (SYSFS_KOBJ_ATTR | SYSFS_KOBJ_BIN_ATTR)

// 用于从 s_flags 字段中清除类型信息以外的其他标志位的掩码
#define SYSFS_FLAG_MASK            ~SYSFS_TYPE_MASK
// 标记 sysfs 目录项已被删除的标志位
#define SYSFS_FLAG_REMOVED         0x0200

// 内联函数，用于获取 sysfs 目录项的类型
static inline unsigned int sysfs_type(struct sysfs_dirent *sd)
{
	// 使用位与操作和 SYSFS_TYPE_MASK 掩码从 s_flags 字段中提取出类型信息
	return sd->s_flags & SYSFS_TYPE_MASK;
}

// 如果启用了锁依赖调试（lockdep）
#ifdef CONFIG_DEBUG_LOCK_ALLOC
#define sysfs_dirent_init_lockdep(sd)                \
do {                                                 \
	struct attribute *attr = sd->s_attr.attr;        \  /* 从 sysfs 目录项中获取关联的属性 */
	struct lock_class_key *key = attr->key;          \  /* 获取属性关联的主锁类关键字 */
	if (!key)                                        \  /* 如果没有设置主锁类关键字 */
		key = &attr->skey;                           \  /* 使用备用的锁类关键字 */
																										\
	lockdep_init_map(&sd->dep_map, "s_active", key, 0);  /* 初始化锁依赖映射 */
} while(0)
#else
// 如果没有启用锁依赖调试
#define sysfs_dirent_init_lockdep(sd) do {} while(0)  /* 定义为空操作 */
#endif

/*
 * Context structure to be used while adding/removing nodes.
 */
// 添加或删除节点时使用的上下文结构。
struct sysfs_addrm_cxt {
	struct sysfs_dirent	*parent_sd;  // 指向父目录项的指针
	struct sysfs_dirent	*removed;    // 指向被移除的目录项的指针
};

/*
 * mount.c
 */
extern struct sysfs_dirent sysfs_root;   // sysfs的根目录项，定义在mount.c文件中
extern struct kmem_cache *sysfs_dir_cachep;  // 指向sysfs目录项缓存的指针，同样定义在mount.c文件中

/*
 * dir.c
 */
extern struct mutex sysfs_mutex;        // 用于sysfs的全局互斥锁，定义在dir.c文件中
extern spinlock_t sysfs_assoc_lock;     // 用于sysfs关联操作的自旋锁，定义在dir.c文件中

// 文件操作和inode操作的函数表，用于sysfs目录。
extern const struct file_operations sysfs_dir_operations;
extern const struct inode_operations sysfs_dir_inode_operations;

// 获取给定sysfs_dirent的dentry。
struct dentry *sysfs_get_dentry(struct sysfs_dirent *sd);
// 增加sysfs_dirent的活跃引用计数。
struct sysfs_dirent *sysfs_get_active(struct sysfs_dirent *sd);
// 减少sysfs_dirent的活跃引用计数。
void sysfs_put_active(struct sysfs_dirent *sd);
// 开始添加或删除操作的上下文，设置父目录项。
void sysfs_addrm_start(struct sysfs_addrm_cxt *acxt,
		       struct sysfs_dirent *parent_sd);
// 在指定的上下文中添加一个目录项。
int __sysfs_add_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd);
// 在指定的上下文中添加一个目录项，处理错误。
int sysfs_add_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd);
// 在指定的上下文中删除一个目录项。
void sysfs_remove_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd);
// 结束添加或删除操作的上下文。
void sysfs_addrm_finish(struct sysfs_addrm_cxt *acxt);

// 在指定父目录项下寻找名称匹配的目录项。
struct sysfs_dirent *sysfs_find_dirent(struct sysfs_dirent *parent_sd,
				       const unsigned char *name);
// 获取指定父目录项下的子目录项，增加其引用计数。
struct sysfs_dirent *sysfs_get_dirent(struct sysfs_dirent *parent_sd,
				      const unsigned char *name);
// 创建一个新的sysfs目录项。
struct sysfs_dirent *sysfs_new_dirent(const char *name, umode_t mode, int type);

// 释放一个sysfs目录项
void release_sysfs_dirent(struct sysfs_dirent *sd);

// 在指定的kobject下创建一个子目录，创建成功后将目录项指针保存在p_sd中
int sysfs_create_subdir(struct kobject *kobj, const char *name,
			struct sysfs_dirent **p_sd);

// 移除一个子目录
void sysfs_remove_subdir(struct sysfs_dirent *sd);

// 重命名一个sysfs目录项，将其移动到新的父目录下并改为新的名称
int sysfs_rename(struct sysfs_dirent *sd,
	struct sysfs_dirent *new_parent_sd, const char *new_name);

// 从内存中获取一个sysfs目录项的引用，增加其引用计数
static inline struct sysfs_dirent *__sysfs_get(struct sysfs_dirent *sd)
{
	if (sd) {
		WARN_ON(!atomic_read(&sd->s_count));  // 检查引用计数是否为0，为0则警告
		atomic_inc(&sd->s_count);  // 增加引用计数
	}
	return sd;
}
#define sysfs_get(sd) __sysfs_get(sd)  // 定义宏简化函数调用

// 释放对sysfs目录项的引用，如果引用计数为0，则释放该目录项
static inline void __sysfs_put(struct sysfs_dirent *sd)
{
	if (sd && atomic_dec_and_test(&sd->s_count))  // 减少引用计数，并检查是否为0
		release_sysfs_dirent(sd);  // 如果为0，则释放该目录项
}
#define sysfs_put(sd) __sysfs_put(sd)  // 定义宏简化函数调用

/*
 * inode.c
 */
// 获取与给定sysfs_dirent关联的inode，如果inode不存在，则创建一个新的inode。
struct inode *sysfs_get_inode(struct super_block *sb, struct sysfs_dirent *sd);
// 删除指定的inode，通常在inode的引用计数为零时调用。
void sysfs_delete_inode(struct inode *inode);
// 设置sysfs_dirent关联的inode的属性，比如修改文件的权限、时间戳等。
int sysfs_sd_setattr(struct sysfs_dirent *sd, struct iattr *iattr);
// 检查对sysfs中的inode的访问权限。
int sysfs_permission(struct inode *inode, int mask);
// 设置与dentry关联的inode的属性。
int sysfs_setattr(struct dentry *dentry, struct iattr *iattr);
// 获取与dentry关联的inode的属性，并填充kstat结构体。
int sysfs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat);
// 为sysfs中的inode设置扩展属性。
int sysfs_setxattr(struct dentry *dentry, const char *name, const void *value,
		size_t size, int flags);
// 在sysfs目录中根据名称查找并移除一个目录项。
int sysfs_hash_and_remove(struct sysfs_dirent *dir_sd, const char *name);
// 初始化sysfs inode子系统。
int sysfs_inode_init(void);

/*
 * file.c
 */
// sysfs文件操作的函数指针表，定义了可以对sysfs文件执行的所有操作。
extern const struct file_operations sysfs_file_operations;

// 向sysfs中的指定目录添加一个文件条目，`attr`指定了文件的属性，`type`指定了文件类型。
int sysfs_add_file(struct sysfs_dirent *dir_sd,
                   const struct attribute *attr, int type);

// 与sysfs_add_file类似，但额外允许指定文件的访问模式（权限）。
int sysfs_add_file_mode(struct sysfs_dirent *dir_sd,
                        const struct attribute *attr, int type, mode_t amode);

/*
 * bin.c
 */
// 二进制文件操作的函数指针表，定义了可以对二进制sysfs文件执行的所有操作。
extern const struct file_operations bin_fops;
// 解除映射二进制文件，通常在二进制文件不再被使用时调用。
void unmap_bin_file(struct sysfs_dirent *attr_sd);

/*
 * symlink.c
 */
// sysfs符号链接inode的操作函数指针表，定义了可以对sysfs符号链接执行的所有inode操作。
extern const struct inode_operations sysfs_symlink_inode_operations;
