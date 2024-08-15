/*
 * Wrapper functions for accessing the file_struct fd array.
 */

#ifndef __LINUX_FILE_H
#define __LINUX_FILE_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/posix_types.h>

// 定义结构体 file，表示一个打开的文件或者其他文件对象。
struct file;

// 声明函数 __fput，用于递减文件对象的引用计数，如果引用计数为0，则释放文件对象。
extern void __fput(struct file *);
// 声明函数 fput，通常用于释放文件对象的引用。在引用计数减至0时释放文件。
extern void fput(struct file *);
// 声明函数 drop_file_write_access，用于撤销对文件的写访问权限。
extern void drop_file_write_access(struct file *file);

// 声明结构体 file_operations，包含指向文件操作函数的指针，如打开、读取、写入等。
struct file_operations;
// 声明结构体 vfsmount，表示一个文件系统挂载点。
struct vfsmount;
// 声明结构体 dentry，表示目录项，是文件名和实际文件之间的联系。
struct dentry;
// 声明结构体 path，包含一个文件系统中的路径，由 dentry 和 vfsmount 结构体组成。
struct path;
// 声明函数 alloc_file，用于分配并初始化一个 file 结构体。
extern struct file *alloc_file(struct path *, fmode_t mode,
	const struct file_operations *fop);

// 定义内联函数 fput_light，用于在需要时释放文件对象的引用。
static inline void fput_light(struct file *file, int fput_needed)
{
	// 如果需要释放文件对象，则调用 fput。
	if (unlikely(fput_needed))
		fput(file);
}

// 声明函数 fget，根据文件描述符获取文件对象的引用。
extern struct file *fget(unsigned int fd);
// 声明函数 fget_light，它是 fget 的轻量版本，用于高性能场景，并返回是否需要执行 fput 的标志。
extern struct file *fget_light(unsigned int fd, int *fput_needed);
// 声明函数 set_close_on_exec，设置文件描述符在 exec 系统调用执行时是否自动关闭。
extern void set_close_on_exec(unsigned int fd, int flag);
// 声明函数 put_filp，用于释放文件对象的引用，通常在文件描述符关闭时调用。
extern void put_filp(struct file *);
// 声明函数 alloc_fd，从指定的起始位置和标志位开始分配一个未使用的文件描述符。
extern int alloc_fd(unsigned start, unsigned flags);
// 声明函数 get_unused_fd，获取一个未使用的文件描述符。
extern int get_unused_fd(void);
// 定义宏 get_unused_fd_flags，使用指定的标志分配一个未使用的文件描述符。
#define get_unused_fd_flags(flags) alloc_fd(0, (flags))
// 声明函数 put_unused_fd，释放一个未使用的文件描述符。
extern void put_unused_fd(unsigned int fd);

// 声明函数 fd_install，将文件对象安装到指定的文件描述符上。
extern void fd_install(unsigned int fd, struct file *file);

#endif /* __LINUX_FILE_H */
