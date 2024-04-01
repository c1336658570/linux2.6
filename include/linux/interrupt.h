/* interrupt.h */
#ifndef _LINUX_INTERRUPT_H
#define _LINUX_INTERRUPT_H

#include <linux/kernel.h>
#include <linux/linkage.h>
#include <linux/bitops.h>
#include <linux/preempt.h>
#include <linux/cpumask.h>
#include <linux/irqreturn.h>
#include <linux/irqnr.h>
#include <linux/hardirq.h>
#include <linux/irqflags.h>
#include <linux/smp.h>
#include <linux/percpu.h>
#include <linux/hrtimer.h>

#include <asm/atomic.h>
#include <asm/ptrace.h>
#include <asm/system.h>

/*
 * These correspond to the IORESOURCE_IRQ_* defines in
 * linux/ioport.h to select the interrupt line behaviour.  When
 * requesting an interrupt without specifying a IRQF_TRIGGER, the
 * setting should be assumed to be "as already configured", which
 * may be as per machine or firmware initialisation.
 */
#define IRQF_TRIGGER_NONE	0x00000000
#define IRQF_TRIGGER_RISING	0x00000001
#define IRQF_TRIGGER_FALLING	0x00000002
#define IRQF_TRIGGER_HIGH	0x00000004
#define IRQF_TRIGGER_LOW	0x00000008
#define IRQF_TRIGGER_MASK	(IRQF_TRIGGER_HIGH | IRQF_TRIGGER_LOW | \
				 IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING)
#define IRQF_TRIGGER_PROBE	0x00000010

/*
 * These flags used only by the kernel as part of the
 * irq handling routines.
 *
 * IRQF_DISABLED - keep irqs disabled when calling the action handler
 * IRQF_SAMPLE_RANDOM - irq is used to feed the random generator
 * IRQF_SHARED - allow sharing the irq among several devices
 * IRQF_PROBE_SHARED - set by callers when they expect sharing mismatches to occur
 * IRQF_TIMER - Flag to mark this interrupt as timer interrupt
 * IRQF_PERCPU - Interrupt is per cpu
 * IRQF_NOBALANCING - Flag to exclude this interrupt from irq balancing
 * IRQF_IRQPOLL - Interrupt is used for polling (only the interrupt that is
 *                registered first in an shared interrupt is considered for
 *                performance reasons)
 * IRQF_ONESHOT - Interrupt is not reenabled after the hardirq handler finished.
 *                Used by threaded interrupts which need to keep the
 *                irq line disabled until the threaded handler has been run.
 */
/*
这些标志仅由内核作为中断处理例程的一部分使用。
IRQF_DISABLED - 在调用动作处理程序时保持中断禁用，如果不设置，中断处理程序可以与除本身外的其他任何中断同时运行。
IRQF_SAMPLE_RANDOM - 中断用于提供随机数生成器的输入，此标志表明这个设备产生的中断对内核熵池由贡献。
IRQF_SHARED - 允许多个设备共享中断
IRQF_PROBE_SHARED - 调用者在预期发生共享不匹配时设置
IRQF_TIMER - 将此中断标记为定时器中断的标志
IRQF_PERCPU - 中断是每个CPU的
IRQF_NOBALANCING - 排除此中断在中断平衡中的标志
IRQF_IRQPOLL - 中断用于轮询（只有在共享中断中首先注册的中断才会考虑，出于性能原因）
IRQF_ONESHOT - 硬中断处理程序完成后不重新启用中断。
*/
#define IRQF_DISABLED		0x00000020
#define IRQF_SAMPLE_RANDOM	0x00000040	// 如果设置该标志，那么来自该设备的中断间隔时间会作为熵填充到熵池。内核熵池负责提供从各种随即事件导出真正的随机数。
#define IRQF_SHARED		0x00000080				// 表明可以在多个中断处理程序之间共享中断线。同一个给定线上注册的每个处理程序必须指定该标志，否则在每条线上只能有一个处理程序
#define IRQF_PROBE_SHARED	0x00000100
#define IRQF_TIMER		0x00000200		// 为系统定时器的中断处理而准备的
#define IRQF_PERCPU		0x00000400
#define IRQF_NOBALANCING	0x00000800
#define IRQF_IRQPOLL		0x00001000
#define IRQF_ONESHOT		0x00002000

/*
 * Bits used by threaded handlers:
 * IRQTF_RUNTHREAD - signals that the interrupt handler thread should run
 * IRQTF_DIED      - handler thread died
 * IRQTF_WARNED    - warning "IRQ_WAKE_THREAD w/o thread_fn" has been printed
 * IRQTF_AFFINITY  - irq thread is requested to adjust affinity
 */
enum {
	IRQTF_RUNTHREAD,
	IRQTF_DIED,
	IRQTF_WARNED,
	IRQTF_AFFINITY,
};

typedef irqreturn_t (*irq_handler_t)(int, void *);	// 中断处理程序类型，返回类型在irqreturn.h中定义

/**
 * struct irqaction - per interrupt action descriptor
 * @handler:	interrupt handler function
 * @flags:	flags (see IRQF_* above)
 * @name:	name of the device
 * @dev_id:	cookie to identify the device
 * @next:	pointer to the next irqaction for shared interrupts
 * @irq:	interrupt number
 * @dir:	pointer to the proc/irq/NN/name entry
 * @thread_fn:	interupt handler function for threaded interrupts
 * @thread:	thread pointer for threaded interrupts
 * @thread_flags:	flags related to @thread
 */
/**
 * struct irqaction - 每个中断动作的描述符
 * @handler:	中断处理函数
 * @flags:	标志（参见上面的 IRQF_*）
 * @name:	设备的名称
 * @dev_id:	用于识别设备的cookie
 * @next:	指向共享中断的下一个irqaction的指针
 * @irq:	中断号
 * @dir:	指向proc/irq/NN/name条目的指针
 * @thread_fn:	线程化中断的处理函数
 * @thread:	线程化中断的线程指针
 * @thread_flags:	与@thread相关的标志
*/
// 定义了每个中断的动作描述符
struct irqaction {
	irq_handler_t handler; // 中断处理函数。这是一个函数指针，指向处理这个中断的函数。
	unsigned long flags; // 中断处理相关的标志（例如，IRQF_SHARED 表示中断可以被多个处理器共享）。
	const char *name; // 设备的名称。这通常用于调试目的，帮助识别哪个设备注册了这个中断处理函数。
	void *dev_id; // 设备标识符。这是一个cookie，用于在共享中断的情况下区分不同的设备。
	struct irqaction *next; // 指向共享同一中断号的下一个irqaction的指针。如果中断不是共享的，这个指针通常是NULL。
	int irq; // 中断号。这个字段指明了这个动作描述符是为哪个中断所设置。
	struct proc_dir_entry *dir; // 指向/proc/irq/NN/name条目的指针。这是中断的proc文件系统入口，用于提供中断的相关信息。
	irq_handler_t thread_fn; // 针对线程化中断的处理函数。如果这个中断被配置为线程化执行，这个函数将被用作线程的入口点。
	struct task_struct *thread; // 线程化中断的线程指针。如果中断被线程化，这里会保存线程的task_struct结构体指针。
	unsigned long thread_flags; // 与线程化处理相关的标志。这些标志用于管理中断线程的行为和状态。
};

extern irqreturn_t no_action(int cpl, void *dev_id);

#ifdef CONFIG_GENERIC_HARDIRQS
extern int __must_check
request_threaded_irq(unsigned int irq, irq_handler_t handler,
		     irq_handler_t thread_fn,
		     unsigned long flags, const char *name, void *dev);

// 分配一条给定的中断线，驱动程序可以通过此函数注册一个中断处理程序
// irq表示要分配的中断号。第二个参数是一个指针，指向这个中断的实际中断处理程序。
// 第三个参数flags可以是0,也可能是一些掩码，在上面40-80行有详细信息
// 第四个参数是与中断相关的设备的ASCII文本表示
// 第五个参数dev用于共享中断线，当中断处理程序需要释放时，dev将提供唯一的标识信息（cookie），
// 以便从共享中断线的诸多中断处理程序中删除指定的那个。如果无需共享中断线，此值设置为NULL即可。
// 函数调用成功返回0,返回非0表示错误，指定的中断处理程序不会被注册。最常见的错误是-EBUSY，表示给定中断线已经在使用（或者当前用户你没有指定IRQF_SHARED）
// 此函数可能睡眠，不能在不安全的上下文调用（如中断），注册过程中，内核需要再/proc/irq文件中创建一个与中断对应的项。
// 此函数调用pro_mkdir来创建，proc_mkdir调用proc_create，proc_create会调kmalloc，kmalloc是可以睡眠的
static inline int __must_check
request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
	    const char *name, void *dev)
{
	return request_threaded_irq(irq, handler, NULL, flags, name, dev);
}

extern void exit_irq_thread(void);
#else

extern int __must_check
request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
	    const char *name, void *dev);

/*
 * Special function to avoid ifdeffery in kernel/irq/devres.c which
 * gets magically built by GENERIC_HARDIRQS=n architectures (sparc,
 * m68k). I really love these $@%#!* obvious Makefile references:
 * ../../../kernel/irq/devres.o
 */
static inline int __must_check
request_threaded_irq(unsigned int irq, irq_handler_t handler,
		     irq_handler_t thread_fn,
		     unsigned long flags, const char *name, void *dev)
{
	return request_irq(irq, handler, flags, name, dev);
}

static inline void exit_irq_thread(void) { }
#endif

// 注销相应的中断处理程序，并释放掉中断线。
// 如果中断线不是共享，那么删除处理程序同时禁用这条中断线。如果时是共享，则仅删除dev对应的处理程序，而这条中断线本身只有在删除了最后一个处理程序才会被禁用
extern void free_irq(unsigned int, void *dev);

struct device;

extern int __must_check
devm_request_threaded_irq(struct device *dev, unsigned int irq,
			  irq_handler_t handler, irq_handler_t thread_fn,
			  unsigned long irqflags, const char *devname,
			  void *dev_id);

static inline int __must_check
devm_request_irq(struct device *dev, unsigned int irq, irq_handler_t handler,
		 unsigned long irqflags, const char *devname, void *dev_id)
{
	return devm_request_threaded_irq(dev, irq, handler, NULL, irqflags,
					 devname, dev_id);
}

extern void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id);

/*
 * On lockdep we dont want to enable hardirqs in hardirq
 * context. Use local_irq_enable_in_hardirq() to annotate
 * kernel code that has to do this nevertheless (pretty much
 * the only valid case is for old/broken hardware that is
 * insanely slow).
 *
 * NOTE: in theory this might break fragile code that relies
 * on hardirq delivery - in practice we dont seem to have such
 * places left. So the only effect should be slightly increased
 * irqs-off latencies.
 */
#ifdef CONFIG_LOCKDEP
# define local_irq_enable_in_hardirq()	do { } while (0)
#else
# define local_irq_enable_in_hardirq()	local_irq_enable()
#endif

// 每一次disable_irq_nosync和disable_irq都需要一次enable_irq。只有完成最后一次enable_irq后，才真正激活中断线
// 禁止中断控制器上指定的中断线，即禁止给定中断向系统中处理器的传递，此函数不会确保所有已经开始执行的中断处理程序已经全部退出
extern void disable_irq_nosync(unsigned int irq);
// 每一次disable_irq_nosync和disable_irq都需要一次enable_irq。只有完成最后一次enable_irq后，才真正激活中断线
// 禁止中断控制器上指定的中断线，即禁止给定中断向系统中处理器的传递，此函数还会确保所有已经开始执行的中断处理程序已经全部退出
extern void disable_irq(unsigned int irq);
// 每一次disable_irq_nosync和disable_irq都需要一次enable_irq。只有完成最后一次enable_irq后，才真正激活中断线
extern void enable_irq(unsigned int irq);

/* The following three functions are for the core kernel use only. */
#ifdef CONFIG_GENERIC_HARDIRQS
extern void suspend_device_irqs(void);
extern void resume_device_irqs(void);
#ifdef CONFIG_PM_SLEEP
extern int check_wakeup_irqs(void);
#else
static inline int check_wakeup_irqs(void) { return 0; }
#endif
#else
static inline void suspend_device_irqs(void) { };
static inline void resume_device_irqs(void) { };
static inline int check_wakeup_irqs(void) { return 0; }
#endif

#if defined(CONFIG_SMP) && defined(CONFIG_GENERIC_HARDIRQS)

extern cpumask_var_t irq_default_affinity;

extern int irq_set_affinity(unsigned int irq, const struct cpumask *cpumask);
extern int irq_can_set_affinity(unsigned int irq);
extern int irq_select_affinity(unsigned int irq);

#else /* CONFIG_SMP */

static inline int irq_set_affinity(unsigned int irq, const struct cpumask *m)
{
	return -EINVAL;
}

static inline int irq_can_set_affinity(unsigned int irq)
{
	return 0;
}

static inline int irq_select_affinity(unsigned int irq)  { return 0; }

#endif /* CONFIG_SMP && CONFIG_GENERIC_HARDIRQS */

#ifdef CONFIG_GENERIC_HARDIRQS
/*
 * Special lockdep variants of irq disabling/enabling.
 * These should be used for locking constructs that
 * know that a particular irq context which is disabled,
 * and which is the only irq-context user of a lock,
 * that it's safe to take the lock in the irq-disabled
 * section without disabling hardirqs.
 *
 * On !CONFIG_LOCKDEP they are equivalent to the normal
 * irq disable/enable methods.
 */
static inline void disable_irq_nosync_lockdep(unsigned int irq)
{
	disable_irq_nosync(irq);
#ifdef CONFIG_LOCKDEP
	local_irq_disable();
#endif
}

static inline void disable_irq_nosync_lockdep_irqsave(unsigned int irq, unsigned long *flags)
{
	disable_irq_nosync(irq);
#ifdef CONFIG_LOCKDEP
	local_irq_save(*flags);
#endif
}

static inline void disable_irq_lockdep(unsigned int irq)
{
	disable_irq(irq);
#ifdef CONFIG_LOCKDEP
	local_irq_disable();
#endif
}

static inline void enable_irq_lockdep(unsigned int irq)
{
#ifdef CONFIG_LOCKDEP
	local_irq_enable();
#endif
	enable_irq(irq);
}

static inline void enable_irq_lockdep_irqrestore(unsigned int irq, unsigned long *flags)
{
#ifdef CONFIG_LOCKDEP
	local_irq_restore(*flags);
#endif
	enable_irq(irq);
}

/* IRQ wakeup (PM) control: */
extern int set_irq_wake(unsigned int irq, unsigned int on);

static inline int enable_irq_wake(unsigned int irq)
{
	return set_irq_wake(irq, 1);
}

static inline int disable_irq_wake(unsigned int irq)
{
	return set_irq_wake(irq, 0);
}

#else /* !CONFIG_GENERIC_HARDIRQS */
/*
 * NOTE: non-genirq architectures, if they want to support the lock
 * validator need to define the methods below in their asm/irq.h
 * files, under an #ifdef CONFIG_LOCKDEP section.
 */
#ifndef CONFIG_LOCKDEP
#  define disable_irq_nosync_lockdep(irq)	disable_irq_nosync(irq)
#  define disable_irq_nosync_lockdep_irqsave(irq, flags) \
						disable_irq_nosync(irq)
#  define disable_irq_lockdep(irq)		disable_irq(irq)
#  define enable_irq_lockdep(irq)		enable_irq(irq)
#  define enable_irq_lockdep_irqrestore(irq, flags) \
						enable_irq(irq)
# endif

static inline int enable_irq_wake(unsigned int irq)
{
	return 0;
}

static inline int disable_irq_wake(unsigned int irq)
{
	return 0;
}
#endif /* CONFIG_GENERIC_HARDIRQS */

#ifndef __ARCH_SET_SOFTIRQ_PENDING
#define set_softirq_pending(x) (local_softirq_pending() = (x))
#define or_softirq_pending(x)  (local_softirq_pending() |= (x))
#endif

/* Some architectures might implement lazy enabling/disabling of
 * interrupts. In some cases, such as stop_machine, we might want
 * to ensure that after a local_irq_disable(), interrupts have
 * really been disabled in hardware. Such architectures need to
 * implement the following hook.
 */
#ifndef hard_irq_disable
#define hard_irq_disable()	do { } while(0)
#endif

/* PLEASE, avoid to allocate new softirqs, if you need not _really_ high
   frequency threaded job scheduling. For almost all the purposes
   tasklets are more than enough. F.e. all serial device BHs et
   al. should be converted to tasklets, not to softirqs.
 */

// 所有软中断的枚举（即系统所有软中断，也代表在软中断表中所处的项），需要添加软中断首先要在这里添加一项
enum
{
	HI_SOFTIRQ=0,						// 优先级高的tasklets
	TIMER_SOFTIRQ,					// 定时器的下半部分
	NET_TX_SOFTIRQ,					// 发送网络数据包
	NET_RX_SOFTIRQ,					// 接受网络数据包
	BLOCK_SOFTIRQ,					// BLOCK装置
	BLOCK_IOPOLL_SOFTIRQ,		
	TASKLET_SOFTIRQ,		// 正常优先级的tasklets
	SCHED_SOFTIRQ,			// 调度程度
	HRTIMER_SOFTIRQ,		// 高分辨率定时器
	RCU_SOFTIRQ,	/* Preferable RCU should always be the last softirq */		// RCU锁定

	NR_SOFTIRQS
};

/* map softirq index to softirq name. update 'softirq_to_name' in
 * kernel/softirq.c when adding a new softirq.
 */
extern char *softirq_to_name[NR_SOFTIRQS];

/* softirq mask and active fields moved to irq_cpustat_t in
 * asm/hardirq.h to get better cache usage.  KAO
 */
// 软中断的结构体定义
struct softirq_action
{
	void	(*action)(struct softirq_action *);		// 内核运行一个软中断处理程序时，就会执行action这个函数
};

// 执行软中断的函数
asmlinkage void do_softirq(void);
asmlinkage void __do_softirq(void);
// 注册软中断处理程序，俩参数，软中断的索引号和处理函数（在上面枚举中添加具体的软中断后使用这个函数注册）
extern void open_softirq(int nr, void (*action)(struct softirq_action *));
extern void softirq_init(void);		// 初始化软中断
#define __raise_softirq_irqoff(nr) do { or_softirq_pending(1UL << (nr)); } while (0)
extern void raise_softirq_irqoff(unsigned int nr);
// 可以将一个软中断设置为挂起状态，让它在下次调用do_softirq()函数时投入运行。
// raise_softirq(NET_TX_SOFTIRQ)，这会触发NET_TX_SOFTIRQ软中断。它的处理程序会在下次内核执行软中断时投入运行。
// 该函数在触发软中断之前先要禁止中断，触发后再恢复。如果中断本来就已经被禁止了，就可以调用另一函数raise_softirq_irqoff
// 主动唤起一个软中断，会首先设置__softirq_pending对应的软中断位为挂起，然后检查in_interrupt，如果不在中断中，则唤起ksoftirq线程执行软中断
extern void raise_softirq(unsigned int nr);
extern void wakeup_softirqd(void);

/* This is the worklist that queues up per-cpu softirq work.
 *
 * send_remote_sendirq() adds work to these lists, and
 * the softirq handler itself dequeues from them.  The queues
 * are protected by disabling local cpu interrupts and they must
 * only be accessed by the local cpu that they are for.
 */
DECLARE_PER_CPU(struct list_head [NR_SOFTIRQS], softirq_work_list);

/* Try to send a softirq to a remote cpu.  If this cannot be done, the
 * work will be queued to the local cpu.
 */
extern void send_remote_softirq(struct call_single_data *cp, int cpu, int softirq);

/* Like send_remote_softirq(), but the caller must disable local cpu interrupts
 * and compute the current cpu, passed in as 'this_cpu'.
 */
extern void __send_remote_softirq(struct call_single_data *cp, int cpu,
				  int this_cpu, int softirq);

/* Tasklets --- multithreaded analogue of BHs.

   Main feature differing them of generic softirqs: tasklet
   is running only on one CPU simultaneously.

   Main feature differing them of BHs: different tasklets
   may be run simultaneously on different CPUs.

   Properties:
   * If tasklet_schedule() is called, then tasklet is guaranteed
     to be executed on some cpu at least once after this.
   * If the tasklet is already scheduled, but its excecution is still not
     started, it will be executed only once.
   * If this tasklet is already running on another CPU (or schedule is called
     from tasklet itself), it is rescheduled for later.
   * Tasklet is strictly serialized wrt itself, but not
     wrt another tasklets. If client needs some intertask synchronization,
     he makes it with spinlocks.
 */

// tasklet结构体，tasklet是由软中断实现的
struct tasklet_struct
{
	struct tasklet_struct *next;			// 链表中下一个tasklet
	unsigned long state;							// tasklet的状态，只能是0,TASKLET_STATE_SCHED和TASKLET_STATE_RUN
	// TASKLET_STATE_SCHED表明tasklet已被调度，准备投入运行，TASKLET_STATE_RUN表明tasklet正在运行。
	atomic_t count;										// 引用计数器，如果不为0,表示tasklet被禁止，不允许执行。只有为0时，tasklet才被激活，并且在被设置为挂起状态时，该tasklet才能够执行
	void (*func)(unsigned long);			// tasklet处理函数
	unsigned long data;								// 给tasklet处理函数的参数
};

// 创建tasklet，初始计数器为0，即初始的tasklet是启用的
#define DECLARE_TASKLET(name, func, data) \
struct tasklet_struct name = { NULL, 0, ATOMIC_INIT(0), func, data }

// 创建tasklet，初始计数器为1，即初始的tasklet是禁用的
#define DECLARE_TASKLET_DISABLED(name, func, data) \
struct tasklet_struct name = { NULL, 0, ATOMIC_INIT(1), func, data }

// tasklet所处的两种状态，TASKLET_STATE_SCHED表明tasklet已被调度，准备投入运行，TASKLET_STATE_RUN表明tasklet正在运行。
enum
{
	TASKLET_STATE_SCHED,	/* Tasklet is scheduled for execution */
	TASKLET_STATE_RUN	/* Tasklet is running (SMP only) */
};

// 对称多处理器
#ifdef CONFIG_SMP
static inline int tasklet_trylock(struct tasklet_struct *t)
{
	return !test_and_set_bit(TASKLET_STATE_RUN, &(t)->state);
}

static inline void tasklet_unlock(struct tasklet_struct *t)
{
	smp_mb__before_clear_bit(); 
	clear_bit(TASKLET_STATE_RUN, &(t)->state);
}

static inline void tasklet_unlock_wait(struct tasklet_struct *t)
{
	while (test_bit(TASKLET_STATE_RUN, &(t)->state)) { barrier(); }
}
#else
// 看#ifdef那个宏里面定义的函数
#define tasklet_trylock(t) 1
#define tasklet_unlock_wait(t) do { } while (0)
#define tasklet_unlock(t) do { } while (0)
#endif

extern void __tasklet_schedule(struct tasklet_struct *t);

// 正常优先级tasklet(TASKLET_SOFTIRQ)的调度函数，通过传递tasklet的地址到t，实现t的调度。
static inline void tasklet_schedule(struct tasklet_struct *t)
{
	// 检查tasklet状态是否为TASKLET_STATE_SCHED。如果是说明tasklet被调度过了，立马返回。
	if (!test_and_set_bit(TASKLET_STATE_SCHED, &t->state))
		__tasklet_schedule(t);	// 调用__tasklet_schedule
}

extern void __tasklet_hi_schedule(struct tasklet_struct *t);

// 高优先级tasklet(HI_SOFTIRQ)的调度函数
static inline void tasklet_hi_schedule(struct tasklet_struct *t)
{
	if (!test_and_set_bit(TASKLET_STATE_SCHED, &t->state))
		__tasklet_hi_schedule(t);
}

extern void __tasklet_hi_schedule_first(struct tasklet_struct *t);

/*
 * This version avoids touching any other tasklets. Needed for kmemcheck
 * in order not to take any page faults while enqueueing this tasklet;
 * consider VERY carefully whether you really need this or
 * tasklet_hi_schedule()...
 */
static inline void tasklet_hi_schedule_first(struct tasklet_struct *t)
{
	if (!test_and_set_bit(TASKLET_STATE_SCHED, &t->state))
		__tasklet_hi_schedule_first(t);
}


// 禁止某个指定的tasklet，如果该tasklet正在执行，此函数不会等到执行完再返回，不安全，因为无法估计该tasklet是否仍在执行
static inline void tasklet_disable_nosync(struct tasklet_struct *t)
{
	atomic_inc(&t->count);
	smp_mb__after_atomic_inc();
}

// 禁止某个指定的tasklet，如果该tasklet正在执行，此函数会等到执行完再返回
static inline void tasklet_disable(struct tasklet_struct *t)
{
	tasklet_disable_nosync(t);
	tasklet_unlock_wait(t);
	smp_mb();
}

// 此函数激活一个tasklet，通过DECLARE_TASKLET_DISABLED创建的tasklet可以通过该函数激活
static inline void tasklet_enable(struct tasklet_struct *t)
{
	smp_mb__before_atomic_dec();
	atomic_dec(&t->count);
}

static inline void tasklet_hi_enable(struct tasklet_struct *t)
{
	smp_mb__before_atomic_dec();
	atomic_dec(&t->count);
}

// 从挂起的队列中去掉一个tasklet，参数是一个指向某个tasklet的tasklet_struct的指针。
// 在处理一个经常重新调度它自身的tasklet的时候，从挂起队列中移除已调度的tasklet很有用。该函数首先等待tasklet执行完，再将它移去
extern void tasklet_kill(struct tasklet_struct *t);
extern void tasklet_kill_immediate(struct tasklet_struct *t, unsigned int cpu);
// 初始化一个tasklet，需要将已经定义的tasklet的地址传给t，函数传给func，函数参数传给data，tasklet默认是启动的
extern void tasklet_init(struct tasklet_struct *t,
			 void (*func)(unsigned long), unsigned long data);

struct tasklet_hrtimer {
	struct hrtimer		timer;
	struct tasklet_struct	tasklet;
	enum hrtimer_restart	(*function)(struct hrtimer *);
};

extern void
tasklet_hrtimer_init(struct tasklet_hrtimer *ttimer,
		     enum hrtimer_restart (*function)(struct hrtimer *),
		     clockid_t which_clock, enum hrtimer_mode mode);

static inline
int tasklet_hrtimer_start(struct tasklet_hrtimer *ttimer, ktime_t time,
			  const enum hrtimer_mode mode)
{
	return hrtimer_start(&ttimer->timer, time, mode);
}

static inline
void tasklet_hrtimer_cancel(struct tasklet_hrtimer *ttimer)
{
	hrtimer_cancel(&ttimer->timer);
	tasklet_kill(&ttimer->tasklet);
}

/*
 * Autoprobing for irqs:
 *
 * probe_irq_on() and probe_irq_off() provide robust primitives
 * for accurate IRQ probing during kernel initialization.  They are
 * reasonably simple to use, are not "fooled" by spurious interrupts,
 * and, unlike other attempts at IRQ probing, they do not get hung on
 * stuck interrupts (such as unused PS2 mouse interfaces on ASUS boards).
 *
 * For reasonably foolproof probing, use them as follows:
 *
 * 1. clear and/or mask the device's internal interrupt.
 * 2. sti();
 * 3. irqs = probe_irq_on();      // "take over" all unassigned idle IRQs
 * 4. enable the device and cause it to trigger an interrupt.
 * 5. wait for the device to interrupt, using non-intrusive polling or a delay.
 * 6. irq = probe_irq_off(irqs);  // get IRQ number, 0=none, negative=multiple
 * 7. service the device to clear its pending interrupt.
 * 8. loop again if paranoia is required.
 *
 * probe_irq_on() returns a mask of allocated irq's.
 *
 * probe_irq_off() takes the mask as a parameter,
 * and returns the irq number which occurred,
 * or zero if none occurred, or a negative irq number
 * if more than one irq occurred.
 */

#if defined(CONFIG_GENERIC_HARDIRQS) && !defined(CONFIG_GENERIC_IRQ_PROBE) 
static inline unsigned long probe_irq_on(void)
{
	return 0;
}
static inline int probe_irq_off(unsigned long val)
{
	return 0;
}
static inline unsigned int probe_irq_mask(unsigned long val)
{
	return 0;
}
#else
extern unsigned long probe_irq_on(void);	/* returns 0 on failure */
extern int probe_irq_off(unsigned long);	/* returns 0 or negative on failure */
extern unsigned int probe_irq_mask(unsigned long);	/* returns mask of ISA interrupts */
#endif

#ifdef CONFIG_PROC_FS
/* Initialize /proc/irq/ */
extern void init_irq_proc(void);
#else
static inline void init_irq_proc(void)
{
}
#endif

struct seq_file;
int show_interrupts(struct seq_file *p, void *v);

struct irq_desc;

extern int early_irq_init(void);
extern int arch_probe_nr_irqs(void);
extern int arch_early_irq_init(void);
extern int arch_init_chip_data(struct irq_desc *desc, int node);

#endif
