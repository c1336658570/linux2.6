#ifndef _LINUX_POLL_H
#define _LINUX_POLL_H

#include <asm/poll.h>

#ifdef __KERNEL__

#include <linux/compiler.h>
#include <linux/ktime.h>
#include <linux/wait.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/sysctl.h>
#include <asm/uaccess.h>

// 为 sysctl 提供外部引用 epoll 控制表数组
extern struct ctl_table epoll_table[]; /* for sysctl */
/* ~832 bytes of stack space used max in sys_select/sys_poll before allocating
   additional memory. */
	/* 在执行 sys_select/sys_poll 之前，在栈上使用的最大空间约为 832 字节，
   如果需要更多内存，则会额外分配。 */
#define MAX_STACK_ALLOC 832  // 定义在栈上分配的最大字节数为 832
#define FRONTEND_STACK_ALLOC 256  // 定义前端栈分配的字节数为 256
#define SELECT_STACK_ALLOC FRONTEND_STACK_ALLOC  // SELECT 操作的栈分配大小与前端一致
#define POLL_STACK_ALLOC FRONTEND_STACK_ALLOC  // POLL 操作的栈分配大小与前端一致
#define WQUEUES_STACK_ALLOC (MAX_STACK_ALLOC - FRONTEND_STACK_ALLOC)  // 等待队列栈分配的大小
#define N_INLINE_POLL_ENTRIES (WQUEUES_STACK_ALLOC / sizeof(struct poll_table_entry))  // 内联轮询条目的数量

#define DEFAULT_POLLMASK (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM)  // 定义默认的轮询掩码

struct poll_table_struct;  // 前置声明轮询表结构

/* 
 * structures and helpers for f_op->poll implementations
 */
/*
 * 结构和辅助函数用于 f_op->poll 实现
 */
// 定义轮询队列处理函数类型
typedef void (*poll_queue_proc)(struct file *, wait_queue_head_t *, struct poll_table_struct *);

// 定义轮询表结构
typedef struct poll_table_struct {
	poll_queue_proc qproc;  // 指向轮询队列处理函数的指针
	unsigned long key;  // 轮询关键字，用于优化轮询过程
} poll_table;

// 此内联函数用于将文件描述符注册到指定的等待队列中。
static inline void poll_wait(struct file * filp, wait_queue_head_t * wait_address, poll_table *p)
{
	// 如果提供了有效的轮询表和等待地址，则调用轮询表中定义的回调函数来处理注册操作
	if (p && wait_address)
		p->qproc(filp, wait_address, p);
}

// 此内联函数用于初始化轮询表结构，设置其回调函数和事件键。
static inline void init_poll_funcptr(poll_table *pt, poll_queue_proc qproc)
{
	pt->qproc = qproc; // 设置轮询表的回调函数
	/* all events enabled */
	pt->key   = ~0UL;  // 启用所有事件
}

// 定义轮询表条目结构，每个条目对应一个等待队列
struct poll_table_entry {
	struct file *filp;           // 文件指针
	unsigned long key;           // 事件键，用于过滤和优化轮询操作
	wait_queue_t wait;           // 等待队列项
	wait_queue_head_t *wait_address; // 等待队列头部地址
};

/*
 * Structures and helpers for sys_poll/sys_poll
 */
// 定义一个结构，用于在系统调用 sys_poll 和 sys_select 中管理轮询操作
struct poll_wqueues {
	poll_table pt; // 轮询表结构，包含轮询回调函数和事件键
	struct poll_table_page *table; // 指向轮询表页的指针，用于存储动态分配的轮询表条目
	struct task_struct *polling_task; // 正在执行轮询的任务结构体指针
	int triggered; // 标记是否有轮询事件被触发
	int error; // 轮询过程中发生的错误
	int inline_index; // 当前使用的内联条目的索引
	struct poll_table_entry inline_entries[N_INLINE_POLL_ENTRIES]; // 预分配的内联轮询表条目数组
};

// 声明用于初始化轮询等待队列结构的函数
extern void poll_initwait(struct poll_wqueues *pwq);
// 声明用于释放轮询等待队列结构的函数
extern void poll_freewait(struct poll_wqueues *pwq);
// 声明一个函数，用于在有超时时间的情况下调度轮询操作
extern int poll_schedule_timeout(struct poll_wqueues *pwq, int state,
				 ktime_t *expires, unsigned long slack);

// 内联函数，用于在没有超时时间的情况下调度轮询操作
static inline int poll_schedule(struct poll_wqueues *pwq, int state)
{
	return poll_schedule_timeout(pwq, state, NULL, 0);
}

/*
 * Scaleable version of the fd_set.
 */
/*
 * 可扩展版本的文件描述符集合（fd_set）。
 */

typedef struct {
	unsigned long *in, *out, *ex; // 指向用于检查输入、输出和异常条件的位数组
	unsigned long *res_in, *res_out, *res_ex; // 指向用于存储结果的位数组
} fd_set_bits;

/*
 * How many longwords for "nr" bits?
 */
/*
 * 计算所需的longwords数量来表示 "nr" 位。
 */
#define FDS_BITPERLONG	(8*sizeof(long))  // 定义每个long类型变量包含的位数
#define FDS_LONGS(nr)	(((nr)+FDS_BITPERLONG-1)/FDS_BITPERLONG)  // 计算表示nr位所需的long类型变量的数量
#define FDS_BYTES(nr)	(FDS_LONGS(nr)*sizeof(long))  // 计算表示nr位所需的字节总数

/*
 * We do a VERIFY_WRITE here even though we are only reading this time:
 * we'll write to it eventually..
 *
 * Use "unsigned long" accesses to let user-mode fd_set's be long-aligned.
 */
/*
 * 即使这次只进行读取操作，我们也进行了写入验证：
 * 最终我们会对其进行写操作。
 *
 * 使用 "unsigned long" 访问来确保用户模式下的 fd_set 与long对齐。
 */
static inline
int get_fd_set(unsigned long nr, void __user *ufdset, unsigned long *fdset)
{
	nr = FDS_BYTES(nr);  // 计算操作的总字节大小
	if (ufdset)  // 如果用户空间提供了文件描述符集的指针
		return copy_from_user(fdset, ufdset, nr) ? -EFAULT : 0;  // 从用户空间拷贝文件描述符集到内核空间，若失败返回-EFAULT

	memset(fdset, 0, nr);  // 若用户空间没有提供文件描述符集的指针，则清零内核空间的文件描述符集
	return 0;  // 成功执行，返回0
}

static inline unsigned long __must_check
set_fd_set(unsigned long nr, void __user *ufdset, unsigned long *fdset)
{
	if (ufdset)
		return __copy_to_user(ufdset, fdset, FDS_BYTES(nr));	// 将内核空间的文件描述符集复制回用户空间，如果复制成功返回0，否则返回未复制的字节数。
	return 0;	// 如果用户空间没有提供文件描述符集的指针，则无需复制，直接返回0。
}

static inline
void zero_fd_set(unsigned long nr, unsigned long *fdset)
{
	memset(fdset, 0, FDS_BYTES(nr));	// 清零内核空间的文件描述符集。
}

#define MAX_INT64_SECONDS (((s64)(~((u64)0)>>1)/HZ)-1)  // 定义64位整数可表示的最大秒数，用于设置时间间隔上限。

extern int do_select(int n, fd_set_bits *fds, struct timespec *end_time);  // select系统调用的内部实现，它使用fd_set_bits结构来处理文件描述符集。
extern int do_sys_poll(struct pollfd __user * ufds, unsigned int nfds,
		       struct timespec *end_time);  // poll系统调用的内部实现。
extern int core_sys_select(int n, fd_set __user *inp, fd_set __user *outp,
			   fd_set __user *exp, struct timespec *end_time);  // select系统调用的核心实现，处理来自用户空间的文件描述符集。

extern int poll_select_set_timeout(struct timespec *to, long sec, long nsec);  // 设置select或poll调用的超时时间。

#endif /* KERNEL */

#endif /* _LINUX_POLL_H */
