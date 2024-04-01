/* thread_info.h: low-level thread information
 *
 * Copyright (C) 2002  David Howells (dhowells@redhat.com)
 * - Incorporating suggestions made by Linus Torvalds and Dave Miller
 */

#ifndef _ASM_X86_THREAD_INFO_H
#define _ASM_X86_THREAD_INFO_H

#include <linux/compiler.h>
#include <asm/page.h>
#include <asm/types.h>

/*
 * low level task data that entry.S needs immediate access to
 * - this struct should fit entirely inside of one cache line
 * - this struct shares the supervisor stack pages
 */
#ifndef __ASSEMBLY__
struct task_struct;
struct exec_domain;
#include <asm/processor.h>
#include <asm/ftrace.h>
#include <asm/atomic.h>

// 在内核栈尾端创建一个thread_info，用来指向task_struct，task_struct通过slab分配器动态分配，就不需要在栈中分配了
struct thread_info {
	struct task_struct	*task;		/* main task structure */		// 指向该进程的task_struct
	struct exec_domain	*exec_domain;	/* execution domain */
	__u32			flags;		/* low level flags */		// 其中有一位来表示need_resched，表示需要重新执行一次调度（schedule）
	__u32			status;		/* thread synchronous flags */
	__u32			cpu;		/* current CPU */
	// 当使用锁此值加1,释放锁此值减1。数值为0的时候，内核就可以执行抢占。从中断返回内核空间时，内核会检查need_resched和
	//preempt_count的值。如果need_resched被设置并且preempt_count为0的话，说明有一个更重要的任务需要执行，此时可以安全的抢占，调度程序会被执行。
	// 如果此时preempt_count不为0,说明当前任务持有锁，所以抢占是不安全的。此时，内核就会向通常那样直接从中断返回当前执行进程。
	// 如果当前锁都被释放，preempt_count就会重新为0。此时，释放锁的代码会检查need_resched是否被设置。如果是的话，就会执行调度程序。
	// 如果内核进程被阻塞，或主动调用schedule()，那么内核抢占也会显示发生。
	// 内核抢占发生在：
	// 中断处理程序正在执行，且返回内核空间之前
	// 内核代码再一次具有可抢占性的时候
	// 如果内核中任务显式调用schedule
	// 内核中的任务阻塞
	/* 
	 * 0-7位preemption count
	 * 在中断上下文中，调度是关闭的，不会发生进程的切换，这属于一种隐式的禁止调度，而在代码中，
	 * 也可以使用preempt_disable()来显示地关闭调度，
	 * 关闭次数由第0到7个bits组成的preemption count(注意不是preempt count)来记录。
	 * 每使用一次preempt_disable()，preemption count的值就会加1，
	 * 使用preempt_enable()则会让preemption count的值减1。
	 * preemption count占8个bits，因此一共可以表示最多256层调度关闭的嵌套。
	 * 处于中断上下文，或者显示地禁止了调度，preempt_count()的值都不为0，都不允许睡眠/调度的发生，
	 * 这两种场景被统称为atomic上下文，可由in_atomic()宏给出判断。
	 * 
	 * preempt_count中的第8到15个bit表示softirq count，它记录了进入softirq的嵌套次数，
	 * 如果softirq count的值为正数，说明现在正处于softirq上下文中。
	 * 由于softirq在单个CPU上是不会嵌套执行的，因此和hardirq count一样，实际只需要一个bit(bit 8)就可以了。
	 * 但这里多出的7个bits并不是因为历史原因多出来的，而是另有他用。
	 * 这个"他用"就是表示在进程上下文中，为了防止进程被softirq所抢占，关闭/禁止softirq的次数，
	 * 比如每使用一次local_bh_disable()，softirq count高7个bits(bit 9到bit 15)的值就会加1，
	 * 使用local_bh_enable()则会让softirq count高7个bits的的值减1。
	 * 代码中可借助in_softirq()宏快速判断当前是否在softirq上下文
	 * 
	 * 
	 * 16到19个bit表示hardirq count，记录了进入hardirq/top half的嵌套次数。
	 * irq_enter()用于标记hardirq的进入，此时hardirq count的值会加1。
	 * irq_exit()用于标记hardirq的退出，hardirq count的值会相应的减1。
	 * 如果hardirq count的值为正数，说明现在正处于hardirq上下文中，代码中可借助in_irq()宏实现快速判断。
	 * hardirq count占据4个bits，理论上可以表示16层嵌套，但现在Linux系统并不支持hardirq的嵌套执行，所以实际使用的只有1个bit。
	 */
	int			preempt_count;	/* 0 => preemptable,
						   <0 => BUG */
	mm_segment_t		addr_limit;
	struct restart_block    restart_block;
	void __user		*sysenter_return;
#ifdef CONFIG_X86_32
	unsigned long           previous_esp;   /* ESP of the previous stack in
						   case of nested (IRQ) stacks
						*/
	__u8			supervisor_stack[0];
#endif
	int			uaccess_err;
};

#define INIT_THREAD_INFO(tsk)			\
{						\
	.task		= &tsk,			\
	.exec_domain	= &default_exec_domain,	\
	.flags		= 0,			\
	.cpu		= 0,			\
	.preempt_count	= INIT_PREEMPT_COUNT,	\
	.addr_limit	= KERNEL_DS,		\
	.restart_block = {			\
		.fn = do_no_restart_syscall,	\
	},					\
}

#define init_thread_info	(init_thread_union.thread_info)
#define init_stack		(init_thread_union.stack)

#else /* !__ASSEMBLY__ */

#include <asm/asm-offsets.h>

#endif

/*
 * thread information flags
 * - these are process state flags that various assembly files
 *   may need to access
 * - pending work-to-be-done flags are in LSW
 * - other flags in MSW
 * Warning: layout of LSW is hardcoded in entry.S
 */
#define TIF_SYSCALL_TRACE	0	/* syscall trace active */
#define TIF_NOTIFY_RESUME	1	/* callback before returning to user */
#define TIF_SIGPENDING		2	/* signal pending */
#define TIF_NEED_RESCHED	3	/* rescheduling necessary */
#define TIF_SINGLESTEP		4	/* reenable singlestep on user return*/
#define TIF_IRET		5	/* force IRET */
#define TIF_SYSCALL_EMU		6	/* syscall emulation active */
#define TIF_SYSCALL_AUDIT	7	/* syscall auditing active */
#define TIF_SECCOMP		8	/* secure computing */
#define TIF_MCE_NOTIFY		10	/* notify userspace of an MCE */
#define TIF_USER_RETURN_NOTIFY	11	/* notify kernel of userspace return */
#define TIF_NOTSC		16	/* TSC is not accessible in userland */
#define TIF_IA32		17	/* 32bit process */
#define TIF_FORK		18	/* ret_from_fork */
#define TIF_MEMDIE		20
#define TIF_DEBUG		21	/* uses debug registers */
#define TIF_IO_BITMAP		22	/* uses I/O bitmap */
#define TIF_FREEZE		23	/* is freezing for suspend */
#define TIF_FORCED_TF		24	/* true if TF in eflags artificially */
#define TIF_DEBUGCTLMSR		25	/* uses thread_struct.debugctlmsr */
#define TIF_DS_AREA_MSR		26      /* uses thread_struct.ds_area_msr */
#define TIF_LAZY_MMU_UPDATES	27	/* task is updating the mmu lazily */
#define TIF_SYSCALL_TRACEPOINT	28	/* syscall tracepoint instrumentation */

#define _TIF_SYSCALL_TRACE	(1 << TIF_SYSCALL_TRACE)
#define _TIF_NOTIFY_RESUME	(1 << TIF_NOTIFY_RESUME)
#define _TIF_SIGPENDING		(1 << TIF_SIGPENDING)
#define _TIF_SINGLESTEP		(1 << TIF_SINGLESTEP)
#define _TIF_NEED_RESCHED	(1 << TIF_NEED_RESCHED)
#define _TIF_IRET		(1 << TIF_IRET)
#define _TIF_SYSCALL_EMU	(1 << TIF_SYSCALL_EMU)
#define _TIF_SYSCALL_AUDIT	(1 << TIF_SYSCALL_AUDIT)
#define _TIF_SECCOMP		(1 << TIF_SECCOMP)
#define _TIF_MCE_NOTIFY		(1 << TIF_MCE_NOTIFY)
#define _TIF_USER_RETURN_NOTIFY	(1 << TIF_USER_RETURN_NOTIFY)
#define _TIF_NOTSC		(1 << TIF_NOTSC)
#define _TIF_IA32		(1 << TIF_IA32)
#define _TIF_FORK		(1 << TIF_FORK)
#define _TIF_DEBUG		(1 << TIF_DEBUG)
#define _TIF_IO_BITMAP		(1 << TIF_IO_BITMAP)
#define _TIF_FREEZE		(1 << TIF_FREEZE)
#define _TIF_FORCED_TF		(1 << TIF_FORCED_TF)
#define _TIF_DEBUGCTLMSR	(1 << TIF_DEBUGCTLMSR)
#define _TIF_DS_AREA_MSR	(1 << TIF_DS_AREA_MSR)
#define _TIF_LAZY_MMU_UPDATES	(1 << TIF_LAZY_MMU_UPDATES)
#define _TIF_SYSCALL_TRACEPOINT	(1 << TIF_SYSCALL_TRACEPOINT)

/* work to do in syscall_trace_enter() */
#define _TIF_WORK_SYSCALL_ENTRY	\
	(_TIF_SYSCALL_TRACE | _TIF_SYSCALL_EMU | _TIF_SYSCALL_AUDIT |	\
	 _TIF_SECCOMP | _TIF_SINGLESTEP | _TIF_SYSCALL_TRACEPOINT)

/* work to do in syscall_trace_leave() */
#define _TIF_WORK_SYSCALL_EXIT	\
	(_TIF_SYSCALL_TRACE | _TIF_SYSCALL_AUDIT | _TIF_SINGLESTEP |	\
	 _TIF_SYSCALL_TRACEPOINT)

/* work to do on interrupt/exception return */
#define _TIF_WORK_MASK							\
	(0x0000FFFF &							\
	 ~(_TIF_SYSCALL_TRACE|_TIF_SYSCALL_AUDIT|			\
	   _TIF_SINGLESTEP|_TIF_SECCOMP|_TIF_SYSCALL_EMU))

/* work to do on any return to user space */
#define _TIF_ALLWORK_MASK						\
	((0x0000FFFF & ~_TIF_SECCOMP) | _TIF_SYSCALL_TRACEPOINT)

/* Only used for 64 bit */
#define _TIF_DO_NOTIFY_MASK						\
	(_TIF_SIGPENDING | _TIF_MCE_NOTIFY | _TIF_NOTIFY_RESUME |	\
	 _TIF_USER_RETURN_NOTIFY)

/* flags to check in __switch_to() */
#define _TIF_WORK_CTXSW							\
	(_TIF_IO_BITMAP|_TIF_DEBUGCTLMSR|_TIF_DS_AREA_MSR|_TIF_NOTSC)

#define _TIF_WORK_CTXSW_PREV (_TIF_WORK_CTXSW|_TIF_USER_RETURN_NOTIFY)
#define _TIF_WORK_CTXSW_NEXT (_TIF_WORK_CTXSW|_TIF_DEBUG)

#define PREEMPT_ACTIVE		0x10000000

/* thread information allocation */
#ifdef CONFIG_DEBUG_STACK_USAGE
#define THREAD_FLAGS (GFP_KERNEL | __GFP_NOTRACK | __GFP_ZERO)
#else
#define THREAD_FLAGS (GFP_KERNEL | __GFP_NOTRACK)
#endif

#define __HAVE_ARCH_THREAD_INFO_ALLOCATOR

#define alloc_thread_info(tsk)						\
	((struct thread_info *)__get_free_pages(THREAD_FLAGS, THREAD_ORDER))

#ifdef CONFIG_X86_32

#define STACK_WARN	(THREAD_SIZE/8)
/*
 * macros/functions for gaining access to the thread information structure
 *
 * preempt_count needs to be 1 initially, until the scheduler is functional.
 */
#ifndef __ASSEMBLY__


/* how to get the current stack pointer from C */
register unsigned long current_stack_pointer asm("esp") __used;

/* how to get the thread information struct from C */
static inline struct thread_info *current_thread_info(void)
{
	return (struct thread_info *)
		(current_stack_pointer & ~(THREAD_SIZE - 1));
}

#else /* !__ASSEMBLY__ */

/* how to get the thread information struct from ASM */
#define GET_THREAD_INFO(reg)	 \
	movl $-THREAD_SIZE, reg; \
	andl %esp, reg

/* use this one if reg already contains %esp */
#define GET_THREAD_INFO_WITH_ESP(reg) \
	andl $-THREAD_SIZE, reg

#endif

#else /* X86_32 */

#include <asm/percpu.h>
#define KERNEL_STACK_OFFSET (5*8)

/*
 * macros/functions for gaining access to the thread information structure
 * preempt_count needs to be 1 initially, until the scheduler is functional.
 */
#ifndef __ASSEMBLY__
DECLARE_PER_CPU(unsigned long, kernel_stack);

// 获取内核栈中存储的thread_info，然后可以通过thread_info来找到task_struct
/*
 * 对应汇编如下，假定内核栈是8KB，如果内核栈是4KB，就需要改为4096：
 * movl $-8192, %eax
 * andl %esp, %eax
 */
static inline struct thread_info *current_thread_info(void)
{
	struct thread_info *ti;
	/*
	 * 获取当前CPU内核栈地址
	 * percpu_read_stable可以在当前CPU上读取per_cpu变量的值
	 * kernel_stack是一个per_cpu变量,保存每个CPU对应的内核栈的起始地址
	 */
	ti = (void *)(percpu_read_stable(kernel_stack) +
		      KERNEL_STACK_OFFSET - THREAD_SIZE);
	return ti;
}

#else /* !__ASSEMBLY__ */

/* how to get the thread information struct from ASM */
#define GET_THREAD_INFO(reg) \
	movq PER_CPU_VAR(kernel_stack),reg ; \
	subq $(THREAD_SIZE-KERNEL_STACK_OFFSET),reg

#endif

#endif /* !X86_32 */

/*
 * Thread-synchronous status.
 *
 * This is different from the flags in that nobody else
 * ever touches our thread-synchronous status, so we don't
 * have to worry about atomic accesses.
 */
#define TS_USEDFPU		0x0001	/* FPU was used by this task
					   this quantum (SMP) */
#define TS_COMPAT		0x0002	/* 32bit syscall active (64BIT)*/
#define TS_POLLING		0x0004	/* true if in idle loop
					   and not sleeping */
#define TS_RESTORE_SIGMASK	0x0008	/* restore signal mask in do_signal() */
#define TS_XSAVE		0x0010	/* Use xsave/xrstor */

#define tsk_is_polling(t) (task_thread_info(t)->status & TS_POLLING)

#ifndef __ASSEMBLY__
#define HAVE_SET_RESTORE_SIGMASK	1
static inline void set_restore_sigmask(void)
{
	struct thread_info *ti = current_thread_info();
	ti->status |= TS_RESTORE_SIGMASK;
	set_bit(TIF_SIGPENDING, (unsigned long *)&ti->flags);
}
#endif	/* !__ASSEMBLY__ */

#ifndef __ASSEMBLY__
extern void arch_task_cache_init(void);
extern void free_thread_info(struct thread_info *ti);
extern int arch_dup_task_struct(struct task_struct *dst, struct task_struct *src);
#define arch_task_cache_init arch_task_cache_init
#endif
#endif /* _ASM_X86_THREAD_INFO_H */
