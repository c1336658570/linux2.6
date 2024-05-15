/*
 * Copyright (c) 1982, 1986 Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Robert Elz at The University of Melbourne.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _LINUX_QUOTA_
#define _LINUX_QUOTA_

#include <linux/errno.h>
#include <linux/types.h>

#define __DQUOT_VERSION__	"dquot_6.5.2"

#define MAXQUOTAS 2
#define USRQUOTA  0		/* element used for user quotas */
#define GRPQUOTA  1		/* element used for group quotas */

/*
 * Definitions for the default names of the quotas files.
 */
#define INITQFNAMES { \
	"user",    /* USRQUOTA */ \
	"group",   /* GRPQUOTA */ \
	"undefined", \
};

/*
 * Command definitions for the 'quotactl' system call.
 * The commands are broken into a main command defined below
 * and a subcommand that is used to convey the type of
 * quota that is being manipulated (see above).
 */
#define SUBCMDMASK  0x00ff
#define SUBCMDSHIFT 8
#define QCMD(cmd, type)  (((cmd) << SUBCMDSHIFT) | ((type) & SUBCMDMASK))

#define Q_SYNC     0x800001	/* sync disk copy of a filesystems quotas */
#define Q_QUOTAON  0x800002	/* turn quotas on */
#define Q_QUOTAOFF 0x800003	/* turn quotas off */
#define Q_GETFMT   0x800004	/* get quota format used on given filesystem */
#define Q_GETINFO  0x800005	/* get information about quota files */
#define Q_SETINFO  0x800006	/* set information about quota files */
#define Q_GETQUOTA 0x800007	/* get user quota structure */
#define Q_SETQUOTA 0x800008	/* set user quota structure */

/* Quota format type IDs */
#define	QFMT_VFS_OLD 1
#define	QFMT_VFS_V0 2
#define QFMT_OCFS2 3
#define	QFMT_VFS_V1 4

/* Size of block in which space limits are passed through the quota
 * interface */
#define QIF_DQBLKSIZE_BITS 10
#define QIF_DQBLKSIZE (1 << QIF_DQBLKSIZE_BITS)

/*
 * Quota structure used for communication with userspace via quotactl
 * Following flags are used to specify which fields are valid
 */
enum {
	QIF_BLIMITS_B = 0,
	QIF_SPACE_B,
	QIF_ILIMITS_B,
	QIF_INODES_B,
	QIF_BTIME_B,
	QIF_ITIME_B,
};

#define QIF_BLIMITS	(1 << QIF_BLIMITS_B)
#define QIF_SPACE	(1 << QIF_SPACE_B)
#define QIF_ILIMITS	(1 << QIF_ILIMITS_B)
#define QIF_INODES	(1 << QIF_INODES_B)
#define QIF_BTIME	(1 << QIF_BTIME_B)
#define QIF_ITIME	(1 << QIF_ITIME_B)
#define QIF_LIMITS	(QIF_BLIMITS | QIF_ILIMITS)
#define QIF_USAGE	(QIF_SPACE | QIF_INODES)
#define QIF_TIMES	(QIF_BTIME | QIF_ITIME)
#define QIF_ALL		(QIF_LIMITS | QIF_USAGE | QIF_TIMES)

struct if_dqblk {
	__u64 dqb_bhardlimit;
	__u64 dqb_bsoftlimit;
	__u64 dqb_curspace;
	__u64 dqb_ihardlimit;
	__u64 dqb_isoftlimit;
	__u64 dqb_curinodes;
	__u64 dqb_btime;
	__u64 dqb_itime;
	__u32 dqb_valid;
};

/*
 * Structure used for setting quota information about file via quotactl
 * Following flags are used to specify which fields are valid
 */
#define IIF_BGRACE	1
#define IIF_IGRACE	2
#define IIF_FLAGS	4
#define IIF_ALL		(IIF_BGRACE | IIF_IGRACE | IIF_FLAGS)

struct if_dqinfo {
	__u64 dqi_bgrace;
	__u64 dqi_igrace;
	__u32 dqi_flags;
	__u32 dqi_valid;
};

/*
 * Definitions for quota netlink interface
 */
#define QUOTA_NL_NOWARN 0
#define QUOTA_NL_IHARDWARN 1		/* Inode hardlimit reached */
#define QUOTA_NL_ISOFTLONGWARN 2 	/* Inode grace time expired */
#define QUOTA_NL_ISOFTWARN 3		/* Inode softlimit reached */
#define QUOTA_NL_BHARDWARN 4		/* Block hardlimit reached */
#define QUOTA_NL_BSOFTLONGWARN 5	/* Block grace time expired */
#define QUOTA_NL_BSOFTWARN 6		/* Block softlimit reached */
#define QUOTA_NL_IHARDBELOW 7		/* Usage got below inode hardlimit */
#define QUOTA_NL_ISOFTBELOW 8		/* Usage got below inode softlimit */
#define QUOTA_NL_BHARDBELOW 9		/* Usage got below block hardlimit */
#define QUOTA_NL_BSOFTBELOW 10		/* Usage got below block softlimit */

enum {
	QUOTA_NL_C_UNSPEC,
	QUOTA_NL_C_WARNING,
	__QUOTA_NL_C_MAX,
};
#define QUOTA_NL_C_MAX (__QUOTA_NL_C_MAX - 1)

enum {
	QUOTA_NL_A_UNSPEC,
	QUOTA_NL_A_QTYPE,
	QUOTA_NL_A_EXCESS_ID,
	QUOTA_NL_A_WARNING,
	QUOTA_NL_A_DEV_MAJOR,
	QUOTA_NL_A_DEV_MINOR,
	QUOTA_NL_A_CAUSED_ID,
	__QUOTA_NL_A_MAX,
};
#define QUOTA_NL_A_MAX (__QUOTA_NL_A_MAX - 1)


#ifdef __KERNEL__
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include <linux/dqblk_xfs.h>
#include <linux/dqblk_v1.h>
#include <linux/dqblk_v2.h>

#include <asm/atomic.h>

/* 内存中存储ID的类型 */
typedef __kernel_uid32_t qid_t; /* Type in which we store ids in memory */
/* 内存中存储大小的类型 */
typedef long long qsize_t;	/* Type in which we store sizes */

/* 外部声明的自旋锁，用于保护配额数据 */
extern spinlock_t dq_data_lock;

/* Maximal numbers of writes for quota operation (insert/delete/update)
 * (over VFS all formats) */
#define DQUOT_INIT_ALLOC max(V1_INIT_ALLOC, V2_INIT_ALLOC)      /* 配额操作的最大初始化分配写次数 */
#define DQUOT_INIT_REWRITE max(V1_INIT_REWRITE, V2_INIT_REWRITE)/* 配额操作的最大初始化重写写次数 */
#define DQUOT_DEL_ALLOC max(V1_DEL_ALLOC, V2_DEL_ALLOC)         /* 配额操作的最大删除分配写次数 */
#define DQUOT_DEL_REWRITE max(V1_DEL_REWRITE, V2_DEL_REWRITE)   /* 配额操作的最大删除重写写次数 */

/*
 * Data for one user/group kept in memory
 */
/*
 * 内存中保存的单个用户/组的数据
 */
// 定义了一个结构体 mem_dqblk，用于在内存中保存单个用户或组的配额数据。
struct mem_dqblk {
	/* 磁盘块的绝对限制，表示该用户或组可以使用的最大磁盘块数。 */
	qsize_t dqb_bhardlimit;	/* absolute limit on disk blks alloc */
	/* 磁盘块的首选限制，表示该用户或组在正常情况下可以使用的最大磁盘块数。 */
	qsize_t dqb_bsoftlimit;	/* preferred limit on disk blks */
	/* 当前已使用的空间，表示该用户或组当前实际使用的磁盘空间。 */
	qsize_t dqb_curspace;	/* current used space */
	/* 当前保留的空间用于延迟分配 */
	qsize_t dqb_rsvspace;   /* current reserved space for delalloc*/
	/* 已分配inode的绝对限制，表示该用户或组可以使用的最大inode数量。 */
	qsize_t dqb_ihardlimit;	/* absolute limit on allocated inodes */
	/* 首选inode限制，表示该用户或组在正常情况下可以使用的最大inode数量。 */
	qsize_t dqb_isoftlimit;	/* preferred inode limit */
	/* 当前已分配的inode数量 */
	qsize_t dqb_curinodes;	/* current # allocated inodes */
	/* 超出磁盘使用限制的时间限制，表示用户或组可以超出软限制的最长时间。 */
	time_t dqb_btime;	/* time limit for excessive disk use */
	/* 超出inode使用限制的时间限制，表示用户或组可以超出软限制的最长时间。 */
	time_t dqb_itime;	/* time limit for excessive inode use */
};

/*
 * Data for one quotafile kept in memory
 */
/*
 * 内存中保存的单个配额文件的数据
 */
struct quota_format_type;	// 前置声明，表示配额格式类型

// 定义了一个结构体 mem_dqinfo，用于在内存中保存单个配额文件的数据。
struct mem_dqinfo {
	// 指向配额格式类型的指针
	struct quota_format_type *dqi_format;
	/* 配额格式的ID，用于在重新挂载为读写时开启配额。 */
	int dqi_fmt_id;		/* Id of the dqi_format - used when turning
				 * quotas on after remount RW */
	/* 脏配额项的链表 */
	struct list_head dqi_dirty_list;	/* List of dirty dquots */
	unsigned long dqi_flags;               // 配额信息的标志
	unsigned int dqi_bgrace;               // 磁盘块宽限时间
	unsigned int dqi_igrace;               // inode宽限时间
	qsize_t dqi_maxblimit;                 // 磁盘块的最大限制
	qsize_t dqi_maxilimit;                 // inode的最大限制
	void *dqi_priv;                        // 私有数据指针
};

struct super_block;

// 定义一个宏 DQF_MASK，用于表示格式特定标志的掩码。
#define DQF_MASK 0xffff		/* Mask for format specific flags */
// 定义一个宏 DQF_INFO_DIRTY_B，表示信息脏标志的位位置。
#define DQF_INFO_DIRTY_B 16
/* 信息是否脏 */
#define DQF_INFO_DIRTY (1 << DQF_INFO_DIRTY_B)	/* Is info dirty? */

// 外部声明，用于标记配额信息为脏
extern void mark_info_dirty(struct super_block *sb, int type);
// 定义一个内联函数 info_dirty，用于测试配额信息是否脏。
static inline int info_dirty(struct mem_dqinfo *info)
{
	// 测试配额信息是否脏
	return test_bit(DQF_INFO_DIRTY_B, &info->dqi_flags);
}

/* 统计配额管理操作的各种次数 */
struct dqstats {
	int lookups;             /* 记录查找操作的次数。 */
	int drops;               /* 记录丢弃操作的次数。 */
	int reads;               /* 读取次数 */
	int writes;              /* 写入次数 */
	int cache_hits;          /* 缓存命中次数 */
	int allocated_dquots;    /* 已分配的配额对象数 */
	int free_dquots;         /* 空闲的配额对象数 */
	int syncs;               /* 同步次数 */
};

// 声明一个外部的 dqstats 结构体实例，用于在其他文件中引用和操作统计数据。
extern struct dqstats dqstats;  /* 外部声明的dqstats结构体实例 */

/* 自上次读取以来dquot已被修改 */
#define DQ_MOD_B	0	/* dquot modified since read */
/* uid/gid已被警告关于块限制 */
#define DQ_BLKS_B	1	/* uid/gid has been warned about blk limit */
/* uid/gid已被警告关于inode限制 */
#define DQ_INODES_B	2	/* uid/gid has been warned about inode limit */
/* 仅限于使用，无限制 */
#define DQ_FAKE_B	3	/* no limits only usage */
/* dquot已被读入内存 */
#define DQ_READ_B	4	/* dquot was read into memory */
/* dquot是活跃的（尚未调用dquot_release） */
#define DQ_ACTIVE_B	5	/* dquot is active (dquot_release not called) */
/* 以下6位（参见QIF_）保留用于通过SETQUOTA quotactl设置的条目掩码。
 * 它们在dq_data_lock下设置并且配额格式处理程序可以在适当的时候清除它们。
 */
#define DQ_LASTSET_B	6	/* Following 6 bits (see QIF_) are reserved\
				 * for the mask of entries set via SETQUOTA\
				 * quotactl. They are set under dq_data_lock\
				 * and the quota format handling dquot can\
				 * clear them when it sees fit. */

struct dquot {
	/* 内存中的哈希链表 */
	struct hlist_node dq_hash;	/* Hash list in memory */
	/* 所有配额的链表 */
	struct list_head dq_inuse;	/* List of all quotas */
	/* 空闲链表元素 */
	struct list_head dq_free;	/* Free list element */
	/* 脏dquot链表 */
	struct list_head dq_dirty;	/* List of dirty dquots */
	/* dquot IO锁 */
	struct mutex dq_lock;		/* dquot IO lock */
	/* 使用计数 */
	atomic_t dq_count;		/* Use count */
	/* 等待队列，等待dquot变为未使用状态 */
	wait_queue_head_t dq_wait_unused;	/* Wait queue for dquot to become unused */
	/* 所属的超级块 */
	struct super_block *dq_sb;	/* superblock this applies to */
	/* 所属的ID（uid，gid） */
	unsigned int dq_id;		/* ID this applies to (uid, gid) */
	/* dquot在磁盘上的偏移 */
	loff_t dq_off;			/* Offset of dquot on disk */
	/* 参见DQ_* */
	unsigned long dq_flags;		/* See DQ_* */
	/* 配额类型 */
	short dq_type;			/* Type of quota */
	/* 磁盘配额使用情况 */
	struct mem_dqblk dq_dqb;	/* Diskquota usage */
};

/* Operations which must be implemented by each quota format */
struct quota_format_ops {
	/* 检测文件是否为我们的格式 */
	int (*check_quota_file)(struct super_block *sb, int type);	/* Detect whether file is in our format */
	/* 读取文件的主要信息 - 在quotaon()时调用 */
	int (*read_file_info)(struct super_block *sb, int type);	/* Read main info about file - called on quotaon() */
	/* 写入文件的主要信息 */
	int (*write_file_info)(struct super_block *sb, int type);	/* Write main info about file */
	/* 在quotaoff()时调用 */
	int (*free_file_info)(struct super_block *sb, int type);	/* Called on quotaoff() */
	/* 读取单个用户的结构 */
	int (*read_dqblk)(struct dquot *dquot);		/* Read structure for one user */
	/* 写入单个用户的结构 */
	int (*commit_dqblk)(struct dquot *dquot);	/* Write structure for one user */
	/* 当最后一个引用被释放时调用 */
	int (*release_dqblk)(struct dquot *dquot);	/* Called when last reference to dquot is being dropped */
};

/* Operations working with dquots */
/* 操作dquot的操作 */
struct dquot_operations {
	/* 普通dquot写操作 */
	int (*write_dquot) (struct dquot *);		/* Ordinary dquot write */
	/* 为新的dquot分配内存 */
	struct dquot *(*alloc_dquot)(struct super_block *, int);	/* Allocate memory for new dquot */
	/* 释放dquot的内存 */
	void (*destroy_dquot)(struct dquot *);		/* Free memory for dquot */
	/* 准备在磁盘上创建配额 */
	int (*acquire_dquot) (struct dquot *);		/* Quota is going to be created on disk */
	/* 准备在磁盘上删除配额 */
	int (*release_dquot) (struct dquot *);		/* Quota is going to be deleted from disk */
	/* 将dquot标记为脏 */
	int (*mark_dirty) (struct dquot *);		/* Dquot is marked dirty */
	/* 写入配额“超级块” */
	int (*write_info) (struct super_block *, int);	/* Write of quota "superblock" */
	/* get reserved quota for delayed alloc, value returned is managed by
	 * quota code only */
	/* 获取延迟分配的保留配额，返回的值仅由配额代码管理 */
	qsize_t *(*get_reserved_space) (struct inode *);
};

/* Operations handling requests from userspace */
/* 处理来自用户空间请求的操作 */
struct quotactl_ops {
	int (*quota_on)(struct super_block *, int, int, char *, int);	/* 启用配额 */
	int (*quota_off)(struct super_block *, int, int);		/* 禁用配额 */
	int (*quota_sync)(struct super_block *, int, int);		/* 同步配额 */
	int (*get_info)(struct super_block *, int, struct if_dqinfo *);	/* 获取配额信息 */
	int (*set_info)(struct super_block *, int, struct if_dqinfo *);	/* 设置配额信息 */
	int (*get_dqblk)(struct super_block *, int, qid_t, struct if_dqblk *);	/* 获取dquot块 */
	int (*set_dqblk)(struct super_block *, int, qid_t, struct if_dqblk *);	/* 设置dquot块 */
	int (*get_xstate)(struct super_block *, struct fs_quota_stat *);	/* 获取扩展状态 */
	int (*set_xstate)(struct super_block *, unsigned int, int);		/* 设置扩展状态 */
	int (*get_xquota)(struct super_block *, int, qid_t, struct fs_disk_quota *);	/* 获取扩展配额 */
	int (*set_xquota)(struct super_block *, int, qid_t, struct fs_disk_quota *);	/* 设置扩展配额 */
};

/* 配额格式类型 */
struct quota_format_type {
	/* 配额格式ID */
	int qf_fmt_id;	/* Quota format id */
	/* 格式操作 */
	const struct quota_format_ops *qf_ops;	/* Operations of format */
	/* 实现配额格式的模块 */
	struct module *qf_owner;		/* Module implementing quota format */
	/* 链接到下一个配额格式 */
	struct quota_format_type *qf_next;
};

/* Quota state flags - they actually come in two flavors - for users and groups */
enum {
	_DQUOT_USAGE_ENABLED = 0,		/* Track disk usage for users */
	_DQUOT_LIMITS_ENABLED,			/* Enforce quota limits for users */
	_DQUOT_SUSPENDED,			/* User diskquotas are off, but
						 * we have necessary info in
						 * memory to turn them on */
	_DQUOT_STATE_FLAGS
};
#define DQUOT_USAGE_ENABLED	(1 << _DQUOT_USAGE_ENABLED)
#define DQUOT_LIMITS_ENABLED	(1 << _DQUOT_LIMITS_ENABLED)
#define DQUOT_SUSPENDED		(1 << _DQUOT_SUSPENDED)
#define DQUOT_STATE_FLAGS	(DQUOT_USAGE_ENABLED | DQUOT_LIMITS_ENABLED | \
				 DQUOT_SUSPENDED)
/* Other quota flags */
#define DQUOT_STATE_LAST	(_DQUOT_STATE_FLAGS * MAXQUOTAS)
#define DQUOT_QUOTA_SYS_FILE	(1 << DQUOT_STATE_LAST)
						/* Quota file is a special
						 * system file and user cannot
						 * touch it. Filesystem is
						 * responsible for setting
						 * S_NOQUOTA, S_NOATIME flags
						 */
#define DQUOT_NEGATIVE_USAGE	(1 << (DQUOT_STATE_LAST + 1))
					       /* Allow negative quota usage */

static inline unsigned int dquot_state_flag(unsigned int flags, int type)
{
	return flags << _DQUOT_STATE_FLAGS * type;
}

static inline unsigned int dquot_generic_flag(unsigned int flags, int type)
{
	return (flags >> _DQUOT_STATE_FLAGS * type) & DQUOT_STATE_FLAGS;
}

#ifdef CONFIG_QUOTA_NETLINK_INTERFACE
extern void quota_send_warning(short type, unsigned int id, dev_t dev,
			       const char warntype);
#else
static inline void quota_send_warning(short type, unsigned int id, dev_t dev,
				      const char warntype)
{
	return;
}
#endif /* CONFIG_QUOTA_NETLINK_INTERFACE */

struct quota_info {
	unsigned int flags;			/* Flags for diskquotas on this device */
	struct mutex dqio_mutex;		/* lock device while I/O in progress */
	struct mutex dqonoff_mutex;		/* Serialize quotaon & quotaoff */
	struct rw_semaphore dqptr_sem;		/* serialize ops using quota_info struct, pointers from inode to dquots */
	struct inode *files[MAXQUOTAS];		/* inodes of quotafiles */
	struct mem_dqinfo info[MAXQUOTAS];	/* Information for each quota type */
	const struct quota_format_ops *ops[MAXQUOTAS];	/* Operations for each type */
};

int register_quota_format(struct quota_format_type *fmt);
void unregister_quota_format(struct quota_format_type *fmt);

struct quota_module_name {
	int qm_fmt_id;
	char *qm_mod_name;
};

#define INIT_QUOTA_MODULE_NAMES {\
	{QFMT_VFS_OLD, "quota_v1"},\
	{QFMT_VFS_V0, "quota_v2"},\
	{0, NULL}}

#else

# /* nodep */ include <sys/cdefs.h>

__BEGIN_DECLS
long quotactl __P ((unsigned int, const char *, int, caddr_t));
__END_DECLS

#endif /* __KERNEL__ */
#endif /* _QUOTA_ */
