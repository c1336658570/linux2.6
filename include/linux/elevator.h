#ifndef _LINUX_ELEVATOR_H
#define _LINUX_ELEVATOR_H

#include <linux/percpu.h>

#ifdef CONFIG_BLOCK

/*
 * 定义各种电梯调度器函数类型，用于处理和调度磁盘IO请求。
 */
// 用于确定是否可以将两个请求合并
typedef int (elevator_merge_fn) (struct request_queue *, struct request **, struct bio *);

// 用于处理两个已经合并的请求
typedef void (elevator_merge_req_fn) (struct request_queue *, struct request *, struct request *);

// 请求合并后的回调函数
typedef void (elevator_merged_fn) (struct request_queue *, struct request *, int);

// 检查是否允许合并请求
typedef int (elevator_allow_merge_fn) (struct request_queue *, struct request *, struct bio *);

// 调度请求
typedef int (elevator_dispatch_fn) (struct request_queue *, int);

// 将请求加入队列
typedef void (elevator_add_req_fn) (struct request_queue *, struct request *);

// 检查队列是否为空
typedef int (elevator_queue_empty_fn) (struct request_queue *);

// 获取请求队列中的下一个或前一个请求
typedef struct request *(elevator_request_list_fn) (struct request_queue *, struct request *);

// 请求完成时的回调函数
typedef void (elevator_completed_req_fn) (struct request_queue *, struct request *);

// 检查是否可以将请求加入队列
typedef int (elevator_may_queue_fn) (struct request_queue *, int);

// 设置请求的处理函数
typedef int (elevator_set_req_fn) (struct request_queue *, struct request *, gfp_t);

// 移除请求时的处理函数
typedef void (elevator_put_req_fn) (struct request *);

// 激活请求
typedef void (elevator_activate_req_fn) (struct request_queue *, struct request *);

// 取消激活请求
typedef void (elevator_deactivate_req_fn) (struct request_queue *, struct request *);

// 初始化电梯调度器
typedef void *(elevator_init_fn) (struct request_queue *);

// 退出电梯调度器
typedef void (elevator_exit_fn) (struct elevator_queue *);

/*
 * 定义了电梯调度器的操作集合，每个调度器都需要实现这些操作。
 */
struct elevator_ops
{
	elevator_merge_fn *elevator_merge_fn;            // 合并请求的函数
	elevator_merged_fn *elevator_merged_fn;          // 请求合并后的回调函数
	elevator_merge_req_fn *elevator_merge_req_fn;    // 处理合并请求的函数
	elevator_allow_merge_fn *elevator_allow_merge_fn;// 检查是否允许合并的函数

	elevator_dispatch_fn *elevator_dispatch_fn;      // 调度请求的函数
	elevator_add_req_fn *elevator_add_req_fn;        // 添加请求到队列的函数
	elevator_activate_req_fn *elevator_activate_req_fn; // 激活请求的函数
	elevator_deactivate_req_fn *elevator_deactivate_req_fn; // 取消激活请求的函数

	elevator_queue_empty_fn *elevator_queue_empty_fn;// 检查队列是否为空的函数
	elevator_completed_req_fn *elevator_completed_req_fn;  // 请求完成的回调函数

	elevator_request_list_fn *elevator_former_req_fn;// 获取前一个请求的函数
	elevator_request_list_fn *elevator_latter_req_fn;// 获取后一个请求的函数

	elevator_set_req_fn *elevator_set_req_fn;        // 设置请求处理的函数
	elevator_put_req_fn *elevator_put_req_fn;        // 移除请求的函数

	elevator_may_queue_fn *elevator_may_queue_fn;    // 检查是否可以加入请求的函数

	elevator_init_fn *elevator_init_fn;              // 初始化电梯调度器的函数
	elevator_exit_fn *elevator_exit_fn;              // 退出电梯调度器的函数
	void (*trim)(struct io_context *);               // 可能用于清理IO上下文的函数
};


#define ELV_NAME_MAX	(16)

struct elv_fs_entry {
	struct attribute attr;
	ssize_t (*show)(struct elevator_queue *, char *);
	ssize_t (*store)(struct elevator_queue *, const char *, size_t);
};

/*
 * identifies an elevator type, such as AS or deadline
 */
/*
 * 用于标识电梯类型，如AS或deadline
 */
// 定义了一个结构体 elevator_type，用于描述Linux内核中的一个IO调度器类型，
// 比如“AS”（Anticipatory Scheduler，预见调度器）或“deadline”调度器。
// 这个结构体用来识别和管理不同的电梯算法类型。
struct elevator_type
{
	struct list_head list;                // 链表头，用于将多个elevator_type连接起来，形成链表
	struct elevator_ops ops;              // 该电梯类型的操作集，定义了具体电梯算法的行为
	struct elv_fs_entry *elevator_attrs;  // 指向电梯调度器属性的指针，这些属性可能会暴露给sysfs，用于配置或监控
	char elevator_name[ELV_NAME_MAX];     // 电梯调度器的名称，作为标识
	struct module *elevator_owner;        // 指向该电梯算法所属模块的指针，用于模块化管理
};

/*
 * each queue has an elevator_queue associated with it
 */
/*
 * 每个队列都有一个与之关联的elevator_queue
 */
// 定义了Linux内核中与IO调度器（电梯算法）相关的elevator_queue结构体。
struct elevator_queue
{
	struct elevator_ops *ops;            // 指向电梯操作结构的指针，定义了电梯调度器的行为
	void *elevator_data;                 // 电梯调度器的数据，可以用于存储特定调度器的内部状态或数据
	struct kobject kobj;                 // 内核对象，用于在sysfs文件系统中表示电梯调度器
	struct elevator_type *elevator_type; // 电梯类型，指向描述电梯调度器类型的结构
	struct mutex sysfs_lock;             // 互斥锁，用于控制对sysfs条目的并发访问
	struct hlist_head *hash;             // 哈希表头指针，可能用于管理调度器中的请求队列或其他散列数据结构
};

/*
 * block elevator interface
 */
extern void elv_dispatch_sort(struct request_queue *, struct request *);
extern void elv_dispatch_add_tail(struct request_queue *, struct request *);
extern void elv_add_request(struct request_queue *, struct request *, int, int);
extern void __elv_add_request(struct request_queue *, struct request *, int, int);
extern void elv_insert(struct request_queue *, struct request *, int);
extern int elv_merge(struct request_queue *, struct request **, struct bio *);
extern void elv_merge_requests(struct request_queue *, struct request *,
			       struct request *);
extern void elv_merged_request(struct request_queue *, struct request *, int);
extern void elv_requeue_request(struct request_queue *, struct request *);
extern int elv_queue_empty(struct request_queue *);
extern struct request *elv_former_request(struct request_queue *, struct request *);
extern struct request *elv_latter_request(struct request_queue *, struct request *);
extern int elv_register_queue(struct request_queue *q);
extern void elv_unregister_queue(struct request_queue *q);
extern int elv_may_queue(struct request_queue *, int);
extern void elv_abort_queue(struct request_queue *);
extern void elv_completed_request(struct request_queue *, struct request *);
extern int elv_set_request(struct request_queue *, struct request *, gfp_t);
extern void elv_put_request(struct request_queue *, struct request *);
extern void elv_drain_elevator(struct request_queue *);

/*
 * io scheduler registration
 */
extern void elv_register(struct elevator_type *);
extern void elv_unregister(struct elevator_type *);

/*
 * io scheduler sysfs switching
 */
extern ssize_t elv_iosched_show(struct request_queue *, char *);
extern ssize_t elv_iosched_store(struct request_queue *, const char *, size_t);

extern int elevator_init(struct request_queue *, char *);
extern void elevator_exit(struct elevator_queue *);
extern int elv_rq_merge_ok(struct request *, struct bio *);

/*
 * Helper functions.
 */
extern struct request *elv_rb_former_request(struct request_queue *, struct request *);
extern struct request *elv_rb_latter_request(struct request_queue *, struct request *);

/*
 * rb support functions.
 */
extern struct request *elv_rb_add(struct rb_root *, struct request *);
extern void elv_rb_del(struct rb_root *, struct request *);
extern struct request *elv_rb_find(struct rb_root *, sector_t);

/*
 * Return values from elevator merger
 */
#define ELEVATOR_NO_MERGE	0
#define ELEVATOR_FRONT_MERGE	1
#define ELEVATOR_BACK_MERGE	2

/*
 * Insertion selection
 */
#define ELEVATOR_INSERT_FRONT	1
#define ELEVATOR_INSERT_BACK	2
#define ELEVATOR_INSERT_SORT	3
#define ELEVATOR_INSERT_REQUEUE	4

/*
 * return values from elevator_may_queue_fn
 */
enum {
	ELV_MQUEUE_MAY,
	ELV_MQUEUE_NO,
	ELV_MQUEUE_MUST,
};

#define rq_end_sector(rq)	(blk_rq_pos(rq) + blk_rq_sectors(rq))
#define rb_entry_rq(node)	rb_entry((node), struct request, rb_node)

/*
 * Hack to reuse the csd.list list_head as the fifo time holder while
 * the request is in the io scheduler. Saves an unsigned long in rq.
 */
#define rq_fifo_time(rq)	((unsigned long) (rq)->csd.list.next)
#define rq_set_fifo_time(rq,exp)	((rq)->csd.list.next = (void *) (exp))
#define rq_entry_fifo(ptr)	list_entry((ptr), struct request, queuelist)
#define rq_fifo_clear(rq)	do {		\
	list_del_init(&(rq)->queuelist);	\
	INIT_LIST_HEAD(&(rq)->csd.list);	\
	} while (0)

/*
 * io context count accounting
 */
#define elv_ioc_count_mod(name, __val)				\
	do {							\
		preempt_disable();				\
		__get_cpu_var(name) += (__val);			\
		preempt_enable();				\
	} while (0)

#define elv_ioc_count_inc(name)	elv_ioc_count_mod(name, 1)
#define elv_ioc_count_dec(name)	elv_ioc_count_mod(name, -1)

#define elv_ioc_count_read(name)				\
({								\
	unsigned long __val = 0;				\
	int __cpu;						\
	smp_wmb();						\
	for_each_possible_cpu(__cpu)				\
		__val += per_cpu(name, __cpu);			\
	__val;							\
})

#endif /* CONFIG_BLOCK */
#endif
