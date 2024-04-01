#ifndef _LINUX_IRQ_H
#define _LINUX_IRQ_H

/*
 * Please do not include this file in generic code.  There is currently
 * no requirement for any architecture to implement anything held
 * within this file.
 *
 * Thanks. --rmk
 */

#include <linux/smp.h>

#ifndef CONFIG_S390

#include <linux/linkage.h>
#include <linux/cache.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/gfp.h>
#include <linux/irqreturn.h>
#include <linux/irqnr.h>
#include <linux/errno.h>
#include <linux/topology.h>
#include <linux/wait.h>

#include <asm/irq.h>
#include <asm/ptrace.h>
#include <asm/irq_regs.h>

struct irq_desc;
typedef	void (*irq_flow_handler_t)(unsigned int irq,
					    struct irq_desc *desc);


/*
 * IRQ line status.
 *
 * Bits 0-7 are reserved for the IRQF_* bits in linux/interrupt.h
 *
 * IRQ types
 */
/*
 * IRQ line status.
 * 中断请求（IRQ）线状态。
 *
 * Bits 0-7 are reserved for the IRQF_* bits in linux/interrupt.h
 * 位0-7用于在linux/interrupt.h中的IRQF_*位保留。
 *
 * IRQ types
 * IRQ类型
 */
#define IRQ_TYPE_NONE			0x00000000	/* Default, unspecified type */
#define IRQ_TYPE_EDGE_RISING		0x00000001	/* 上升沿触发类型 */
#define IRQ_TYPE_EDGE_FALLING		0x00000002	/* 下降沿触发类型 */
#define IRQ_TYPE_EDGE_BOTH		(IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING) /* 上下降沿触发类型 */
#define IRQ_TYPE_LEVEL_HIGH		0x00000004	/* 高电平触发类型 */
#define IRQ_TYPE_LEVEL_LOW		0x00000008	/* 低电平触发类型 */
#define IRQ_TYPE_SENSE_MASK		0x0000000f	/* 以上类型的掩码 */
#define IRQ_TYPE_PROBE			0x00000010	/* 探测正在进行中 */

/* Internal flags */
/* 内部标志 */
#define IRQ_INPROGRESS			0x00000100	/* IRQ处理程序正在运行 - 不要进入！ */
#define IRQ_DISABLED			0x00000200	/* IRQ被禁用 - 不要进入！ */
#define IRQ_PENDING			0x00000400	/* IRQ挂起 - 在启用时重新触发 */
#define IRQ_REPLAY			0x00000800	/* IRQ已经重新触发但尚未确认 */
#define IRQ_AUTODETECT			0x00001000	/* 正在自动检测IRQ */
#define IRQ_WAITING			0x00002000	/* IRQ尚未触发 - 用于自动检测 */
#define IRQ_LEVEL			0x00004000	/* IRQ电平触发 */
#define IRQ_MASKED			0x00008000	/* IRQ被屏蔽 - 不应再次触发 */
#define IRQ_PER_CPU			0x00010000	/* 每个CPU的IRQ */
#define IRQ_NOPROBE			0x00020000	/* IRQ对于探测无效 */
#define IRQ_NOREQUEST			0x00040000	/* IRQ无法请求 */
#define IRQ_NOAUTOEN			0x00080000	/* 请求IRQ时不会启用IRQ */
#define IRQ_WAKEUP			0x00100000	/* IRQ触发系统唤醒 */
#define IRQ_MOVE_PENDING		0x00200000	/* 需要重新定位IRQ目标 */
#define IRQ_NO_BALANCING		0x00400000	/* IRQ不参与负载均衡 */
#define IRQ_SPURIOUS_DISABLED		0x00800000	/* IRQ被虚假陷阱禁用 */
#define IRQ_MOVE_PCNTXT			0x01000000	/* 从进程上下文迁移的IRQ */
#define IRQ_AFFINITY_SET		0x02000000	/* IRQ亲和性已从用户空间设置 */
#define IRQ_SUSPENDED			0x04000000	/* IRQ已经经历挂起序列 */
#define IRQ_ONESHOT			0x08000000	/* IRQ在硬中断后不会取消屏蔽 */
#define IRQ_NESTED_THREAD		0x10000000	/* IRQ嵌套到另一个中断中，没有自己的处理程序线程 */

#ifdef CONFIG_IRQ_PER_CPU
# define CHECK_IRQ_PER_CPU(var) ((var) & IRQ_PER_CPU)
# define IRQ_NO_BALANCING_MASK	(IRQ_PER_CPU | IRQ_NO_BALANCING)
#else
# define CHECK_IRQ_PER_CPU(var) 0
# define IRQ_NO_BALANCING_MASK	IRQ_NO_BALANCING
#endif

struct proc_dir_entry;
struct msi_desc;

/**
 * struct irq_chip - hardware interrupt chip descriptor
 *
 * @name:		name for /proc/interrupts
 * @startup:		start up the interrupt (defaults to ->enable if NULL)
 * @shutdown:		shut down the interrupt (defaults to ->disable if NULL)
 * @enable:		enable the interrupt (defaults to chip->unmask if NULL)
 * @disable:		disable the interrupt
 * @ack:		start of a new interrupt
 * @mask:		mask an interrupt source
 * @mask_ack:		ack and mask an interrupt source
 * @unmask:		unmask an interrupt source
 * @eoi:		end of interrupt - chip level
 * @end:		end of interrupt - flow level
 * @set_affinity:	set the CPU affinity on SMP machines
 * @retrigger:		resend an IRQ to the CPU
 * @set_type:		set the flow type (IRQ_TYPE_LEVEL/etc.) of an IRQ
 * @set_wake:		enable/disable power-management wake-on of an IRQ
 *
 * @bus_lock:		function to lock access to slow bus (i2c) chips
 * @bus_sync_unlock:	function to sync and unlock slow bus (i2c) chips
 *
 * @release:		release function solely used by UML
 * @typename:		obsoleted by name, kept as migration helper
 */
/**
 * struct irq_chip - 硬件中断芯片描述符
 *
 * @name:		用于/proc/interrupts的名称
 * @startup:		启动中断（如果为NULL，则默认为->enable）
 * @shutdown:		关闭中断（如果为NULL，则默认为->disable）
 * @enable:		使能中断（如果为NULL，则默认为chip->unmask）
 * @disable:		禁用中断
 * @ack:		开始一个新的中断
 * @mask:		屏蔽中断源
 * @mask_ack:		屏蔽并确认中断源
 * @unmask:		解除屏蔽中断源
 * @eoi:		中断结束 - 芯片级别
 * @end:		中断结束 - 流控制级别
 * @set_affinity:	设置SMP机器上的CPU亲和性
 * @retrigger:		重新发送一个IRQ给CPU
 * @set_type:		设置IRQ的流类型（IRQ_TYPE_LEVEL等）
 * @set_wake:		启用/禁用IRQ的电源管理唤醒
 *
 * @bus_lock:		用于锁定对慢速总线（如i2c）芯片的访问的函数
 * @bus_sync_unlock:	用于同步和解锁慢速总线（如i2c）芯片的函数
 *
 * @release:		仅由UML使用的释放函数
 * @typename:		已被name取代，作为迁移助手保留
 */
// 定义了一组函数指针，用于处理硬件中断的不同操作，包括启动、关闭、使能、禁用中断，中断确认，屏蔽和解除屏蔽中断源，中断结束等。这些函数指针提供了与硬件平台相关的操作，以便在内核中正确处理硬件中断。
struct irq_chip {
	const char	*name;
	unsigned int	(*startup)(unsigned int irq);
	void		(*shutdown)(unsigned int irq);
	void		(*enable)(unsigned int irq);
	void		(*disable)(unsigned int irq);

	void		(*ack)(unsigned int irq);
	void		(*mask)(unsigned int irq);
	void		(*mask_ack)(unsigned int irq);
	void		(*unmask)(unsigned int irq);
	void		(*eoi)(unsigned int irq);

	void		(*end)(unsigned int irq);
	int		(*set_affinity)(unsigned int irq,
					const struct cpumask *dest);
	int		(*retrigger)(unsigned int irq);
	int		(*set_type)(unsigned int irq, unsigned int flow_type);
	int		(*set_wake)(unsigned int irq, unsigned int on);

	void		(*bus_lock)(unsigned int irq);
	void		(*bus_sync_unlock)(unsigned int irq);

	/* Currently used only by UML, might disappear one day.*/
#ifdef CONFIG_IRQ_RELEASE_METHOD
	void		(*release)(unsigned int irq, void *dev_id);
#endif
	/*
	 * For compatibility, ->typename is copied into ->name.
	 * Will disappear.
	 */
	const char	*typename;
};

struct timer_rand_state;	// 计时器随机状态结构体
struct irq_2_iommu;		// 中断到 IOMMU 的结构体

/**
 * struct irq_desc - 中断描述符
 * @irq: 中断号
 * @timer_rand_state: 指向计时器随机状态结构体的指针
 * @kstat_irqs: 每个 CPU 的中断统计信息
 * @irq_2_iommu: 拥有该中断的 IOMMU
 * @handle_irq: 高级别中断事件处理程序（如果为 NULL，则为 __do_IRQ()）
 * @chip: 低级别中断硬件访问
 * @msi_desc: MSI 描述符
 * @handler_data: irq_chip 方法的每个中断数据
 * @chip_data: 与芯片相关的特定于平台的每个芯片私有数据，以允许共享芯片实现
 * @action: 中断动作链
 * @status: 状态信息，可以通过设置该变量来关闭该中断
 * @depth: 禁用深度，用于嵌套的 irq_disable() 调用
 * @wake_depth: 启用深度，用于多个 set_irq_wake() 调用者
 * @irq_count: 用于检测挂起的中断的统计字段
 * @last_unhandled: 未处理计数的老化定时器
 * @irqs_unhandled: 用于虚假未处理中断的统计字段
 * @lock: SMP 的锁定
 * @affinity: SMP 上的 IRQ 亲和性
 * @node: 用于负载平衡的节点索引
 * @pending_mask: 待处理的重新平衡中断
 * @threads_active: 当前正在运行的 irqaction 线程数量
 * @wait_for_threads: 同步 irq 等待线程处理程序的等待队列
 * @dir: /proc/irq/ procfs 条目
 * @name: 用于 /proc/interrupts 输出的流处理程序名称
 */
// 中断描述符
struct irq_desc {
	unsigned int		irq;
	struct timer_rand_state *timer_rand_state;
	unsigned int            *kstat_irqs;
#ifdef CONFIG_INTR_REMAP
	struct irq_2_iommu      *irq_2_iommu;
#endif
	/*
	handle_irq就是highlevel irq-events handler，何谓high level？站在高处自然看不到细节。我认为high level是和specific相对，specific handler处理具体的事务，例如处理一个按键中断、处理一个磁盘中断。而high level则是对处理各种中断交互过程的一个抽象，根据下列硬件的不同：
	（a）中断控制器
	（b）IRQ trigger type highlevel irq-events handler可以分成：
	（a）处理电平触发类型的中断handler（handle_level_irq）
	（b）处理边缘触发类型的中断handler（handle_edge_irq）
	（c）处理简单类型的中断handler（handle_simple_irq）
	（d）处理EOI类型的中断handler（handle_fasteoi_irq）
	*/
	irq_flow_handler_t	handle_irq;
	struct irq_chip		*chip;
	struct msi_desc		*msi_desc;
	void			*handler_data;
	void			*chip_data;
	struct irqaction	*action;	/* IRQ action list */		// 一个irq（中断）线上可能有多个设备，将多个设备串起来
	unsigned int		status;		/* IRQ status */	/* 中断状态，可以通过设置该变量来关闭该中断 */

	unsigned int		depth;		/* nested irq disables */		/* 嵌套的 irq 禁用计数 */
	unsigned int		wake_depth;	/* nested wake enables */	/* 嵌套的唤醒启用计数 */
	unsigned int		irq_count;	/* For detecting broken IRQs */	/* 用于检测中断异常的统计字段 */
	unsigned long		last_unhandled;	/* Aging timer for unhandled count */	/* 未处理计数的老化定时器 */
	unsigned int		irqs_unhandled;
	raw_spinlock_t		lock;
#ifdef CONFIG_SMP
	cpumask_var_t		affinity;
	unsigned int		node;
#ifdef CONFIG_GENERIC_PENDING_IRQ
	cpumask_var_t		pending_mask;
#endif
#endif
	atomic_t		threads_active;
	wait_queue_head_t       wait_for_threads;
#ifdef CONFIG_PROC_FS
	struct proc_dir_entry	*dir;
#endif
	const char		*name;
} ____cacheline_internodealigned_in_smp;

extern void arch_init_copy_chip_data(struct irq_desc *old_desc,
					struct irq_desc *desc, int node);
extern void arch_free_chip_data(struct irq_desc *old_desc, struct irq_desc *desc);

#ifndef CONFIG_SPARSE_IRQ
// 中断描述符表
extern struct irq_desc irq_desc[NR_IRQS];
#endif

#ifdef CONFIG_NUMA_IRQ_DESC
extern struct irq_desc *move_irq_desc(struct irq_desc *old_desc, int node);
#else
static inline struct irq_desc *move_irq_desc(struct irq_desc *desc, int node)
{
	return desc;
}
#endif

extern struct irq_desc *irq_to_desc_alloc_node(unsigned int irq, int node);

/*
 * Pick up the arch-dependent methods:
 */
#include <asm/hw_irq.h>

extern int setup_irq(unsigned int irq, struct irqaction *new);
extern void remove_irq(unsigned int irq, struct irqaction *act);

#ifdef CONFIG_GENERIC_HARDIRQS

#ifdef CONFIG_SMP

#ifdef CONFIG_GENERIC_PENDING_IRQ

void move_native_irq(int irq);
void move_masked_irq(int irq);

#else /* CONFIG_GENERIC_PENDING_IRQ */

static inline void move_irq(int irq)
{
}

static inline void move_native_irq(int irq)
{
}

static inline void move_masked_irq(int irq)
{
}

#endif /* CONFIG_GENERIC_PENDING_IRQ */

#else /* CONFIG_SMP */

#define move_native_irq(x)
#define move_masked_irq(x)

#endif /* CONFIG_SMP */

extern int no_irq_affinity;

static inline int irq_balancing_disabled(unsigned int irq)
{
	struct irq_desc *desc;

	desc = irq_to_desc(irq);
	return desc->status & IRQ_NO_BALANCING_MASK;
}

/* Handle irq action chains: */
extern irqreturn_t handle_IRQ_event(unsigned int irq, struct irqaction *action);

/*
 * Built-in IRQ handlers for various IRQ types,
 * callable via desc->handle_irq()
 */
extern void handle_level_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_fasteoi_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_edge_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_simple_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_percpu_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_bad_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_nested_irq(unsigned int irq);

/*
 * Monolithic do_IRQ implementation.
 */
#ifndef CONFIG_GENERIC_HARDIRQS_NO__DO_IRQ
extern unsigned int __do_IRQ(unsigned int irq);
#endif

/*
 * Architectures call this to let the generic IRQ layer
 * handle an interrupt. If the descriptor is attached to an
 * irqchip-style controller then we call the ->handle_irq() handler,
 * and it calls __do_IRQ() if it's attached to an irqtype-style controller.
 */
static inline void generic_handle_irq_desc(unsigned int irq, struct irq_desc *desc)
{
#ifdef CONFIG_GENERIC_HARDIRQS_NO__DO_IRQ
	desc->handle_irq(irq, desc);
#else
	if (likely(desc->handle_irq))
		desc->handle_irq(irq, desc);
	else
		__do_IRQ(irq);
#endif
}

static inline void generic_handle_irq(unsigned int irq)
{
	generic_handle_irq_desc(irq, irq_to_desc(irq));
}

/* Handling of unhandled and spurious interrupts: */
extern void note_interrupt(unsigned int irq, struct irq_desc *desc,
			   irqreturn_t action_ret);

/* Resending of interrupts :*/
void check_irq_resend(struct irq_desc *desc, unsigned int irq);

/* Enable/disable irq debugging output: */
extern int noirqdebug_setup(char *str);

/* Checks whether the interrupt can be requested by request_irq(): */
extern int can_request_irq(unsigned int irq, unsigned long irqflags);

/* Dummy irq-chip implementations: */
extern struct irq_chip no_irq_chip;
extern struct irq_chip dummy_irq_chip;

extern void
set_irq_chip_and_handler(unsigned int irq, struct irq_chip *chip,
			 irq_flow_handler_t handle);
extern void
set_irq_chip_and_handler_name(unsigned int irq, struct irq_chip *chip,
			      irq_flow_handler_t handle, const char *name);

extern void
__set_irq_handler(unsigned int irq, irq_flow_handler_t handle, int is_chained,
		  const char *name);

/* caller has locked the irq_desc and both params are valid */
static inline void __set_irq_handler_unlocked(int irq,
					      irq_flow_handler_t handler)
{
	struct irq_desc *desc;

	desc = irq_to_desc(irq);
	desc->handle_irq = handler;
}

/*
 * Set a highlevel flow handler for a given IRQ:
 */
static inline void
set_irq_handler(unsigned int irq, irq_flow_handler_t handle)
{
	__set_irq_handler(irq, handle, 0, NULL);
}

/*
 * Set a highlevel chained flow handler for a given IRQ.
 * (a chained handler is automatically enabled and set to
 *  IRQ_NOREQUEST and IRQ_NOPROBE)
 */
static inline void
set_irq_chained_handler(unsigned int irq,
			irq_flow_handler_t handle)
{
	__set_irq_handler(irq, handle, 1, NULL);
}

extern void set_irq_nested_thread(unsigned int irq, int nest);

extern void set_irq_noprobe(unsigned int irq);
extern void set_irq_probe(unsigned int irq);

/* Handle dynamic irq creation and destruction */
extern unsigned int create_irq_nr(unsigned int irq_want, int node);
extern int create_irq(void);
extern void destroy_irq(unsigned int irq);

/* Test to see if a driver has successfully requested an irq */
static inline int irq_has_action(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	return desc->action != NULL;
}

/* Dynamic irq helper functions */
extern void dynamic_irq_init(unsigned int irq);
void dynamic_irq_init_keep_chip_data(unsigned int irq);
extern void dynamic_irq_cleanup(unsigned int irq);
void dynamic_irq_cleanup_keep_chip_data(unsigned int irq);

/* Set/get chip/data for an IRQ: */
extern int set_irq_chip(unsigned int irq, struct irq_chip *chip);
extern int set_irq_data(unsigned int irq, void *data);
extern int set_irq_chip_data(unsigned int irq, void *data);
extern int set_irq_type(unsigned int irq, unsigned int type);
extern int set_irq_msi(unsigned int irq, struct msi_desc *entry);

#define get_irq_chip(irq)	(irq_to_desc(irq)->chip)
#define get_irq_chip_data(irq)	(irq_to_desc(irq)->chip_data)
#define get_irq_data(irq)	(irq_to_desc(irq)->handler_data)
#define get_irq_msi(irq)	(irq_to_desc(irq)->msi_desc)

#define get_irq_desc_chip(desc)		((desc)->chip)
#define get_irq_desc_chip_data(desc)	((desc)->chip_data)
#define get_irq_desc_data(desc)		((desc)->handler_data)
#define get_irq_desc_msi(desc)		((desc)->msi_desc)

#endif /* CONFIG_GENERIC_HARDIRQS */

#endif /* !CONFIG_S390 */

#ifdef CONFIG_SMP
/**
 * alloc_desc_masks - allocate cpumasks for irq_desc
 * @desc:	pointer to irq_desc struct
 * @node:	node which will be handling the cpumasks
 * @boot:	true if need bootmem
 *
 * Allocates affinity and pending_mask cpumask if required.
 * Returns true if successful (or not required).
 */
static inline bool alloc_desc_masks(struct irq_desc *desc, int node,
							bool boot)
{
	gfp_t gfp = GFP_ATOMIC;

	if (boot)
		gfp = GFP_NOWAIT;

#ifdef CONFIG_CPUMASK_OFFSTACK
	if (!alloc_cpumask_var_node(&desc->affinity, gfp, node))
		return false;

#ifdef CONFIG_GENERIC_PENDING_IRQ
	if (!alloc_cpumask_var_node(&desc->pending_mask, gfp, node)) {
		free_cpumask_var(desc->affinity);
		return false;
	}
#endif
#endif
	return true;
}

static inline void init_desc_masks(struct irq_desc *desc)
{
	cpumask_setall(desc->affinity);
#ifdef CONFIG_GENERIC_PENDING_IRQ
	cpumask_clear(desc->pending_mask);
#endif
}

/**
 * init_copy_desc_masks - copy cpumasks for irq_desc
 * @old_desc:	pointer to old irq_desc struct
 * @new_desc:	pointer to new irq_desc struct
 *
 * Insures affinity and pending_masks are copied to new irq_desc.
 * If !CONFIG_CPUMASKS_OFFSTACK the cpumasks are embedded in the
 * irq_desc struct so the copy is redundant.
 */

static inline void init_copy_desc_masks(struct irq_desc *old_desc,
					struct irq_desc *new_desc)
{
#ifdef CONFIG_CPUMASK_OFFSTACK
	cpumask_copy(new_desc->affinity, old_desc->affinity);

#ifdef CONFIG_GENERIC_PENDING_IRQ
	cpumask_copy(new_desc->pending_mask, old_desc->pending_mask);
#endif
#endif
}

static inline void free_desc_masks(struct irq_desc *old_desc,
				   struct irq_desc *new_desc)
{
	free_cpumask_var(old_desc->affinity);

#ifdef CONFIG_GENERIC_PENDING_IRQ
	free_cpumask_var(old_desc->pending_mask);
#endif
}

#else /* !CONFIG_SMP */

static inline bool alloc_desc_masks(struct irq_desc *desc, int node,
								bool boot)
{
	return true;
}

static inline void init_desc_masks(struct irq_desc *desc)
{
}

static inline void init_copy_desc_masks(struct irq_desc *old_desc,
					struct irq_desc *new_desc)
{
}

static inline void free_desc_masks(struct irq_desc *old_desc,
				   struct irq_desc *new_desc)
{
}
#endif	/* CONFIG_SMP */

#endif /* _LINUX_IRQ_H */
