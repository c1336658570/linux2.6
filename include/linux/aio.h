#ifndef __LINUX__AIO_H
#define __LINUX__AIO_H

#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/aio_abi.h>
#include <linux/uio.h>
#include <linux/rcupdate.h>

#include <asm/atomic.h>

/* 定义 AIO 相关常数 */
#define AIO_MAXSEGS		4
#define AIO_KIOGRP_NR_ATOMIC	8

struct kioctx;	// 前向声明，用于引用 AIO 上下文

/* Notes on cancelling a kiocb:
 *	If a kiocb is cancelled, aio_complete may return 0 to indicate 
 *	that cancel has not yet disposed of the kiocb.  All cancel 
 *	operations *must* call aio_put_req to dispose of the kiocb 
 *	to guard against races with the completion code.
 */
/* kiocb 的取消注释：
 * 如果一个 kiocb 被取消了，aio_complete 可能返回 0，表示取消操作还没有完成对 kiocb 的处理。
 * 所有的取消操作必须调用 aio_put_req 以处理 kiocb，以防止与完成代码的竞争。
 */
#define KIOCB_C_CANCELLED	0x01
#define KIOCB_C_COMPLETE	0x02

// 用作特殊的同步键值
#define KIOCB_SYNC_KEY		(~0U)

/* ki_flags bits */
/* ki_flags 位标志 */
/*
 * This may be used for cancel/retry serialization in the future, but
 * for now it's unused and we probably don't want modules to even
 * think they can use it.
 */
/* #define KIF_LOCKED		0 */
/* kiocb 锁操作宏 */
#define KIF_KICKED		1
#define KIF_CANCELLED		2

#define kiocbTryLock(iocb)	test_and_set_bit(KIF_LOCKED, &(iocb)->ki_flags)
#define kiocbTryKick(iocb)	test_and_set_bit(KIF_KICKED, &(iocb)->ki_flags)

#define kiocbSetLocked(iocb)	set_bit(KIF_LOCKED, &(iocb)->ki_flags)
#define kiocbSetKicked(iocb)	set_bit(KIF_KICKED, &(iocb)->ki_flags)
#define kiocbSetCancelled(iocb)	set_bit(KIF_CANCELLED, &(iocb)->ki_flags)

#define kiocbClearLocked(iocb)	clear_bit(KIF_LOCKED, &(iocb)->ki_flags)
#define kiocbClearKicked(iocb)	clear_bit(KIF_KICKED, &(iocb)->ki_flags)
#define kiocbClearCancelled(iocb)	clear_bit(KIF_CANCELLED, &(iocb)->ki_flags)

#define kiocbIsLocked(iocb)	test_bit(KIF_LOCKED, &(iocb)->ki_flags)
#define kiocbIsKicked(iocb)	test_bit(KIF_KICKED, &(iocb)->ki_flags)
#define kiocbIsCancelled(iocb)	test_bit(KIF_CANCELLED, &(iocb)->ki_flags)

/* is there a better place to document function pointer methods? */
/**
 * ki_retry	-	iocb forward progress callback
 * @kiocb:	The kiocb struct to advance by performing an operation.
 *
 * This callback is called when the AIO core wants a given AIO operation
 * to make forward progress.  The kiocb argument describes the operation
 * that is to be performed.  As the operation proceeds, perhaps partially,
 * ki_retry is expected to update the kiocb with progress made.  Typically
 * ki_retry is set in the AIO core and it itself calls file_operations
 * helpers.
 *
 * ki_retry's return value determines when the AIO operation is completed
 * and an event is generated in the AIO event ring.  Except the special
 * return values described below, the value that is returned from ki_retry
 * is transferred directly into the completion ring as the operation's
 * resulting status.  Once this has happened ki_retry *MUST NOT* reference
 * the kiocb pointer again.
 *
 * If ki_retry returns -EIOCBQUEUED it has made a promise that aio_complete()
 * will be called on the kiocb pointer in the future.  The AIO core will
 * not ask the method again -- ki_retry must ensure forward progress.
 * aio_complete() must be called once and only once in the future, multiple
 * calls may result in undefined behaviour.
 *
 * If ki_retry returns -EIOCBRETRY it has made a promise that kick_iocb()
 * will be called on the kiocb pointer in the future.  This may happen
 * through generic helpers that associate kiocb->ki_wait with a wait
 * queue head that ki_retry uses via current->io_wait.  It can also happen
 * with custom tracking and manual calls to kick_iocb(), though that is
 * discouraged.  In either case, kick_iocb() must be called once and only
 * once.  ki_retry must ensure forward progress, the AIO core will wait
 * indefinitely for kick_iocb() to be called.
 */
/* kiocb 结构体定义 */
struct kiocb {
	// 运行列表
	struct list_head	ki_run_list;
	// 标志位
	unsigned long		ki_flags;
	// 用户计数
	int			ki_users;
	// 请求的 ID
	unsigned		ki_key;		/* id of this request */

	// 文件指针
	struct file		*ki_filp;
	// AIO 上下文，对于同步操作可能为 NULL
	struct kioctx		*ki_ctx;	/* may be NULL for sync ops */
	// 取消函数
	int			(*ki_cancel)(struct kiocb *, struct io_event *);
	// 重试回调，用于推动 AIO 操作前进
	ssize_t			(*ki_retry)(struct kiocb *);
	// 析构函数
	void			(*ki_dtor)(struct kiocb *);

	union {
		// 用户空间指针
		void __user		*user;
		// 任务结构体指针
		struct task_struct	*tsk;
	} ki_obj;

	// 用户完成数据
	__u64			ki_user_data;	/* user's data for completion */
	// 文件操作位置
	loff_t			ki_pos;

	// 私有数据
	void			*private;
	/* State that we remember to be able to restart/retry  */
	/* 状态数据，用于重启/重试 */
	// 操作码
	unsigned short		ki_opcode;
	// nbytes的副本
	size_t			ki_nbytes; 	/* copy of iocb->aio_nbytes */
	// 剩余的 buf
	char 			__user *ki_buf;	/* remaining iocb->aio_buf */
	// 剩余字节数
	size_t			ki_left; 	/* remaining bytes */
	// 内联向量
	struct iovec		ki_inline_vec;	/* inline vector */
	// iovec 指针
 	struct iovec		*ki_iovec;
	// 段数
 	unsigned long		ki_nr_segs;
	// 当前段
 	unsigned long		ki_cur_seg;

	// AIO 核心使用的列表，用于取消
	struct list_head	ki_list;	/* the aio core uses this
						 * for cancellation */

	/*
	 * If the aio_resfd field of the userspace iocb is not zero,
	 * this is the underlying eventfd context to deliver events to.
	 */
	// 事件文件描述符上下文，用于事件通知
	struct eventfd_ctx	*ki_eventfd;
};

#define is_sync_kiocb(iocb)	((iocb)->ki_key == KIOCB_SYNC_KEY)
#define init_sync_kiocb(x, filp)			\
	do {						\
		struct task_struct *tsk = current;	\
		(x)->ki_flags = 0;			\
		(x)->ki_users = 1;			\
		(x)->ki_key = KIOCB_SYNC_KEY;		\
		(x)->ki_filp = (filp);			\
		(x)->ki_ctx = NULL;			\
		(x)->ki_cancel = NULL;			\
		(x)->ki_retry = NULL;			\
		(x)->ki_dtor = NULL;			\
		(x)->ki_obj.tsk = tsk;			\
		(x)->ki_user_data = 0;                  \
	} while (0)

#define AIO_RING_MAGIC			0xa10a10a1
#define AIO_RING_COMPAT_FEATURES	1
#define AIO_RING_INCOMPAT_FEATURES	0
struct aio_ring {
	unsigned	id;	/* kernel internal index number */
	unsigned	nr;	/* number of io_events */
	unsigned	head;
	unsigned	tail;

	unsigned	magic;
	unsigned	compat_features;
	unsigned	incompat_features;
	unsigned	header_length;	/* size of aio_ring */


	struct io_event		io_events[0];
}; /* 128 bytes + ring size */

#define aio_ring_avail(info, ring)	(((ring)->head + (info)->nr - 1 - (ring)->tail) % (info)->nr)

#define AIO_RING_PAGES	8
struct aio_ring_info {
	unsigned long		mmap_base;
	unsigned long		mmap_size;

	struct page		**ring_pages;
	spinlock_t		ring_lock;
	long			nr_pages;

	unsigned		nr, tail;

	struct page		*internal_pages[AIO_RING_PAGES];
};

struct kioctx {
	atomic_t		users;
	int			dead;
	struct mm_struct	*mm;

	/* This needs improving */
	unsigned long		user_id;
	struct hlist_node	list;

	wait_queue_head_t	wait;

	spinlock_t		ctx_lock;

	int			reqs_active;
	struct list_head	active_reqs;	/* used for cancellation */
	struct list_head	run_list;	/* used for kicked reqs */

	/* sys_io_setup currently limits this to an unsigned int */
	unsigned		max_reqs;

	struct aio_ring_info	ring_info;

	struct delayed_work	wq;

	struct rcu_head		rcu_head;
};

/* prototypes */
extern unsigned aio_max_size;

#ifdef CONFIG_AIO
extern ssize_t wait_on_sync_kiocb(struct kiocb *iocb);
extern int aio_put_req(struct kiocb *iocb);
extern void kick_iocb(struct kiocb *iocb);
extern int aio_complete(struct kiocb *iocb, long res, long res2);
struct mm_struct;
extern void exit_aio(struct mm_struct *mm);
#else
static inline ssize_t wait_on_sync_kiocb(struct kiocb *iocb) { return 0; }
static inline int aio_put_req(struct kiocb *iocb) { return 0; }
static inline void kick_iocb(struct kiocb *iocb) { }
static inline int aio_complete(struct kiocb *iocb, long res, long res2) { return 0; }
struct mm_struct;
static inline void exit_aio(struct mm_struct *mm) { }
#endif /* CONFIG_AIO */

static inline struct kiocb *list_kiocb(struct list_head *h)
{
	return list_entry(h, struct kiocb, ki_list);
}

/* for sysctl: */
extern unsigned long aio_nr;
extern unsigned long aio_max_nr;

#endif /* __LINUX__AIO_H */
