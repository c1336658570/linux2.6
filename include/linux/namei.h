#ifndef _LINUX_NAMEI_H
#define _LINUX_NAMEI_H

#include <linux/dcache.h>
#include <linux/linkage.h>
#include <linux/path.h>

struct vfsmount;	// 前向声明，表示一个文件系统的挂载点

// open_intent 结构用于描述打开文件时的意图
struct open_intent {
	int	flags;       // 打开文件时的标志，如 O_RDONLY、O_WRONLY 等
	int	create_mode;  // 如果正在创建文件，这是用于设置文件模式（权限）的参数
	struct file *file;  // 与打开的文件关联的 file 结构指针
};

// 最大嵌套链接数的定义
enum { MAX_NESTED_LINKS = 8 };

// nameidata 结构用于储存文件路径查找期间的信息
struct nameidata {
	struct path	path;           // 包含当前查找路径的 vnode 和 vfsmount
	struct qstr	last;           // 最后一个组件的名称和长度
	struct path	root;           // 查找操作的根路径
	unsigned int	flags;          // 查找相关的标志，如 LOOKUP_DIRECTORY
	int		last_type;      // 上一个路径组件的类型，如 LAST_ROOT
	unsigned	depth;          // 符号链接跟随的当前深度
	// 每个元素都是指针，指向一块内存
	char *saved_names[MAX_NESTED_LINKS + 1];  // 用于保存嵌套符号链接名称的数组

	/* Intent data */
	// 用于存储不同类型操作的特定数据
	union {
		struct open_intent open;  // 如果是打开文件操作，使用此结构
	} intent;
};

/*
 * Type of the last component on LOOKUP_PARENT
 */
/*
 * LOOKUP_PARENT 搜索时最后一个组件的类型
 */
enum {
	LAST_NORM,       // 普通文件或目录
	LAST_ROOT,       // 根目录
	LAST_DOT,        // 当前目录 "."
	LAST_DOTDOT,    // 父目录 ".."
	LAST_BIND        // 用于绑定挂载点
};

/*
 * The bitmask for a lookup event:
 *  - follow links at the end
 *  - require a directory
 *  - ending slashes ok even for nonexistent files
 *  - internal "there are more path components" flag
 *  - locked when lookup done with dcache_lock held
 *  - dentry cache is untrusted; force a real lookup
 */
/*
 * 文件路径查找时的位掩码：
 *  - 在最后跟随符号链接
 *  - 要求是一个目录
 *  - 即使是不存在的文件也接受结尾斜杠
 *  - 内部“还有更多路径组件”的标志
 *  - 使用 dcache_lock 锁定完成查找
 *  - 目录项缓存不受信任；强制进行实际查找
 */
#define LOOKUP_FOLLOW		 1	// 跟随符号链接
#define LOOKUP_DIRECTORY	 2	// 要求路径必须是目录
#define LOOKUP_CONTINUE		 4	// 查找操作需要继续
#define LOOKUP_PARENT		16		// 查找操作目标是父目录
#define LOOKUP_REVAL		64		// 重新验证路径的有效性
/*
 * Intent data
 */
/*
 * 查找意图数据
 */
#define LOOKUP_OPEN		0x0100	// 打开文件
#define LOOKUP_CREATE		0x0200	// 创建文件
#define LOOKUP_EXCL		0x0400		// 独占方式打开或创建
#define LOOKUP_RENAME_TARGET	0x0800	// 重命名操作的目标

/*
 * 通过用户提供的路径字符串获取其对应的内核路径结构体
 */
extern int user_path_at(int, const char __user *, unsigned, struct path *);

// 获取指定用户空间路径的内核路径表示，跟随符号链接
#define user_path(name, path) user_path_at(AT_FDCWD, name, LOOKUP_FOLLOW, path)
// 获取指定用户空间路径的内核路径表示，不跟随符号链接
#define user_lpath(name, path) user_path_at(AT_FDCWD, name, 0, path)
// 获取指定用户空间路径的内核路径表示，要求路径必须是目录，并跟随符号链接
#define user_path_dir(name, path) \
	user_path_at(AT_FDCWD, name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, path)

// 通过路径字符串获取内核的路径结构体
extern int kern_path(const char *, unsigned, struct path *);

// 根据路径字符串和标志进行路径查找，填充 nameidata 结构
extern int path_lookup(const char *, unsigned, struct nameidata *);
// 进行虚拟文件系统层的路径查找
extern int vfs_path_lookup(struct dentry *, struct vfsmount *,
			   const char *, unsigned int, struct nameidata *);

// 根据 nameidata 和 dentry 实例化一个文件，可能调用提供的 open 函数
extern struct file *lookup_instantiate_filp(struct nameidata *nd, struct dentry *dentry,
		int (*open)(struct inode *, struct file *));

// 在给定的目录项和长度下查找单个名称
extern struct dentry *lookup_one_len(const char *, struct dentry *, int);

// 跟踪到更低一级的路径
extern int follow_down(struct path *);
// 跟踪到更高一级的路径
extern int follow_up(struct path *);

// 为重命名操作加锁两个目录项
extern struct dentry *lock_rename(struct dentry *, struct dentry *);
// 解锁两个为重命名操作加锁的目录项
extern void unlock_rename(struct dentry *, struct dentry *);

// 设置 nameidata 结构中的链接路径
static inline void nd_set_link(struct nameidata *nd, char *path)
{
	// 在递归深度处保存路径
	nd->saved_names[nd->depth] = path;
}

// 获取 nameidata 结构中当前深度的链接路径
static inline char *nd_get_link(struct nameidata *nd)
{
	// 返回保存在当前深度的路径
	return nd->saved_names[nd->depth];
}

// 确保链接字符串被正确终止
static inline void nd_terminate_link(void *name, size_t len, size_t maxlen)
{
	// 根据最小长度终止字符串
	((char *) name)[min(len, maxlen)] = '\0';
}

#endif /* _LINUX_NAMEI_H */
