#ifndef _LINUX_FS_STRUCT_H
#define _LINUX_FS_STRUCT_H

#include <linux/path.h>

// 有三个将VFS层和系统的进程紧密联系在一起，它们分别是file_struct, fs_struct, namespace(mnt_namespace)结构体。
// 和进程相关的结构体，描述了文件系统
// 由进程描述符中的 fs 域指向，包含文件系统和进程相关的信息。
struct fs_struct {
	int users;  // 使用该结构体的用户数目（引用计数），用于管理多线程访问
	rwlock_t lock;  // 读写锁，用于保护 fs_struct 结构体中的数据，确保并发访问时的数据一致性
	int umask;  // 掩码，用于设置新创建文件的默认权限
	int in_exec;  // 当前正在执行的文件的标志，防止并发执行导致的不一致
	// 当前工作目录 (pwd) 和根目录 (root) 的路径
	struct path root, pwd;  // 根目录路径和当前工作目录路径，path 结构体包含 dentry 和 vfsmount
};

// 内核内存缓存池，用于分配和管理 fs_struct 实例的内存
extern struct kmem_cache *fs_cachep;


// 退出时释放进程的 fs_struct 结构
extern void exit_fs(struct task_struct *);
// 设置进程的根目录路径
extern void set_fs_root(struct fs_struct *, struct path *);
// 设置进程的当前工作目录路径
extern void set_fs_pwd(struct fs_struct *, struct path *);
// 复制进程的 fs_struct 结构，返回指向新结构的指针
extern struct fs_struct *copy_fs_struct(struct fs_struct *);
// 释放 fs_struct 结构的内存
extern void free_fs_struct(struct fs_struct *);
// 将 fs_struct 结构标记为守护进程使用，确保其不被频繁修改
extern void daemonize_fs_struct(void);
// 为当前进程取消共享 fs_struct 结构，创建一个新的副本
extern int unshare_fs_struct(void);

#endif /* _LINUX_FS_STRUCT_H */
