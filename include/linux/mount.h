/*
 *
 * Definitions for mount interface. This describes the in the kernel build 
 * linkedlist with mounted filesystems.
 *
 * Author:  Marco van Wieringen <mvw@planets.elm.net>
 *
 */
#ifndef _LINUX_MOUNT_H
#define _LINUX_MOUNT_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/nodemask.h>
#include <linux/spinlock.h>
#include <asm/atomic.h>

struct super_block;
struct vfsmount;
struct dentry;
struct mnt_namespace;

#define MNT_NOSUID	0x01
#define MNT_NODEV	0x02
#define MNT_NOEXEC	0x04
#define MNT_NOATIME	0x08
#define MNT_NODIRATIME	0x10
#define MNT_RELATIME	0x20
#define MNT_READONLY	0x40	/* does the user want this to be r/o? */
#define MNT_STRICTATIME 0x80

#define MNT_SHRINKABLE	0x100
#define MNT_WRITE_HOLD	0x200

#define MNT_SHARED	0x1000	/* if the vfsmount is a shared mount */
#define MNT_UNBINDABLE	0x2000	/* if the vfsmount is a unbindable mount */
/*
 * MNT_SHARED_MASK is the set of flags that should be cleared when a
 * mount becomes shared.  Currently, this is only the flag that says a
 * mount cannot be bind mounted, since this is how we create a mount
 * that shares events with another mount.  If you add a new MNT_*
 * flag, consider how it interacts with shared mounts.
 */
#define MNT_SHARED_MASK	(MNT_UNBINDABLE)
#define MNT_PROPAGATION_MASK	(MNT_SHARED | MNT_UNBINDABLE)


#define MNT_INTERNAL	0x4000

// 用来描述一个安装文件系统的实例
// 每一个安装点都由该结构体表示,它包含了安装点相关信息,如位置和安装标志等。
// 当文件系统被实际安装时，有一个vfsmount结构体在安装点被创建。该结构体用来代表文件系统的实例，即一个安装点
struct vfsmount {
	struct list_head mnt_hash;		/* 散列表 */
	struct vfsmount *mnt_parent;	/* fs we are mounted on */	/* 父文件系统，也就是要挂载到哪个文件系统 */
	struct dentry *mnt_mountpoint;	/* dentry of mountpoint */	/* 安装点的目录项 */
	struct dentry *mnt_root;	/* root of the mounted tree */		/* 该文件系统的根目录项 */
	struct super_block *mnt_sb;	/* pointer to superblock */			/* 该文件系统的超级块 */
	struct list_head mnt_mounts;	/* list of children, anchored here */		/* 子文件系统链表 */
	struct list_head mnt_child;	/* and going through their mnt_child */		/* 子文件系统链表 */
	// vfsmount结构还保存了在安装时指定的标志信息，该信息存储在mnt_flags域中
	// MNT_NOSUID，禁止该文件系统的可执行文件设置setuid和setgid标志
	// MNT_MODEV，禁止访问该文件系统上的设备文件
	// MNT_NOEXEC，禁止执行该文件系统上的可执行文件
	int mnt_flags;		/* 安装标志 */
	/* 4 bytes hole on 64bits arches */
	const char *mnt_devname;	/* Name of device e.g. /dev/dsk/hda1 */	/* 设备文件名 e.g. /dev/dsk/hda1 */
	struct list_head mnt_list;		/* 描述符链表 */
	struct list_head mnt_expire;	/* link in fs-specific expiry list */		/* 到期链表的入口 */
	struct list_head mnt_share;	/* circular list of shared mounts */			/* 共享安装链表的入口 */
	struct list_head mnt_slave_list;/* list of slave mounts */						/* 从安装链表 */
	struct list_head mnt_slave;	/* slave list entry */										/* 从安装链表的入口 */
	struct vfsmount *mnt_master;	/* slave is on master->mnt_slave_list */		/* 从安装链表的主人 */
	struct mnt_namespace *mnt_ns;	/* containing namespace */			/* 相关的命名空间 */
	int mnt_id;			/* mount identifier */			/* 安装标识符 */
	int mnt_group_id;		/* peer group identifier */		/* 组标识符 */
	/*
	 * We put mnt_count & mnt_expiry_mark at the end of struct vfsmount
	 * to let these frequently modified fields in a separate cache line
	 * (so that reads of mnt_flags wont ping-pong on SMP machines)
	 */
	atomic_t mnt_count;		/* 使用计数 */
	int mnt_expiry_mark;		/* true if marked for expiry */		/* 如果标记为到期，则为 True */
	int mnt_pinned;				/* "钉住"进程计数 */
	int mnt_ghosts;				/* "镜像"引用计数 */
#ifdef CONFIG_SMP
	int __percpu *mnt_writers;		/* 写者引用计数 */
#else
	int mnt_writers;							/* 写者引用计数 */
#endif
};

static inline int *get_mnt_writers_ptr(struct vfsmount *mnt)
{
#ifdef CONFIG_SMP
	return mnt->mnt_writers;
#else
	return &mnt->mnt_writers;
#endif
}

static inline struct vfsmount *mntget(struct vfsmount *mnt)
{
	if (mnt)
		atomic_inc(&mnt->mnt_count);
	return mnt;
}

struct file; /* forward dec */

extern int mnt_want_write(struct vfsmount *mnt);
extern int mnt_want_write_file(struct file *file);
extern int mnt_clone_write(struct vfsmount *mnt);
extern void mnt_drop_write(struct vfsmount *mnt);
extern void mntput_no_expire(struct vfsmount *mnt);
extern void mnt_pin(struct vfsmount *mnt);
extern void mnt_unpin(struct vfsmount *mnt);
extern int __mnt_is_readonly(struct vfsmount *mnt);

static inline void mntput(struct vfsmount *mnt)
{
	if (mnt) {
		mnt->mnt_expiry_mark = 0;
		mntput_no_expire(mnt);
	}
}

extern struct vfsmount *do_kern_mount(const char *fstype, int flags,
				      const char *name, void *data);

struct file_system_type;
extern struct vfsmount *vfs_kern_mount(struct file_system_type *type,
				      int flags, const char *name,
				      void *data);

struct nameidata;

struct path;
extern int do_add_mount(struct vfsmount *newmnt, struct path *path,
			int mnt_flags, struct list_head *fslist);

extern void mark_mounts_for_expiry(struct list_head *mounts);

extern dev_t name_to_dev_t(char *name);

#endif /* _LINUX_MOUNT_H */
