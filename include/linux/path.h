#ifndef _LINUX_PATH_H
#define _LINUX_PATH_H

struct dentry;
struct vfsmount;

// 定义 path 结构体，表示一个路径
struct path {
	struct vfsmount *mnt;  // 指向挂载点的指针
	struct dentry *dentry; // 指向目录项的指针
};

// 函数声明，用于增加 path 结构体的引用计数
extern void path_get(struct path *);
// 函数声明，用于减少 path 结构体的引用计数
extern void path_put(struct path *);

#endif  /* _LINUX_PATH_H */
