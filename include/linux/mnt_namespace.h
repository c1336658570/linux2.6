#ifndef _NAMESPACE_H_
#define _NAMESPACE_H_
#ifdef __KERNEL__

#include <linux/path.h>
#include <linux/seq_file.h>
#include <linux/wait.h>

// 有三个将VFS层和系统的进程紧密联系在一起，它们分别是file_struct, fs_struct, namespace结构体。
/**
 *  上述这些数据结构都是通过进程描述符连接起来的，对多数进程来说，它们的描述符都指向唯一的
 * files_struct和fs_struct结构体。但对于那些使用CLONE_FILES或CLONE_FS创建的进程，
 * 会共享这两个结构体。所以多进程描述符可能指向同一个files_struct或fs_struct结构体，
 * 每个结构体都维护一个count域作为引用计数，防止在进程正使用时，该结构被撤销。
 */
struct mnt_namespace {
	atomic_t		count;	// 结构的使用计数
	struct vfsmount *	root;	// 根目录的安装点对象
	// list域是连接已安装文件系统的双向链表，它包含的元素组成了全体命名空间。
	struct list_head	list;	// 安装点链表
	wait_queue_head_t poll;	// 轮询的等待队列
	int event;							// 事件计数
};

struct proc_mounts {
	struct seq_file m; /* must be the first element */
	struct mnt_namespace *ns;
	struct path root;
	int event;
};

struct fs_struct;

extern struct mnt_namespace *create_mnt_ns(struct vfsmount *mnt);
extern struct mnt_namespace *copy_mnt_ns(unsigned long, struct mnt_namespace *,
		struct fs_struct *);
extern void put_mnt_ns(struct mnt_namespace *ns);
static inline void get_mnt_ns(struct mnt_namespace *ns)
{
	atomic_inc(&ns->count);
}

extern const struct seq_operations mounts_op;
extern const struct seq_operations mountinfo_op;
extern const struct seq_operations mountstats_op;
extern int mnt_had_events(struct proc_mounts *);

#endif
#endif
