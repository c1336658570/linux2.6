/*
 * include/asm-cris/processor.h
 *
 * Copyright (C) 2000, 2001 Axis Communications AB
 *
 * Authors:         Bjorn Wesen        Initial version
 *
 */

#ifndef __ASM_CRIS_PROCESSOR_H
#define __ASM_CRIS_PROCESSOR_H

#include <asm/system.h>
#include <asm/page.h>
#include <asm/ptrace.h>
#include <arch/processor.h>

struct task_struct;

#define STACK_TOP	TASK_SIZE	// 定义堆栈顶部为任务大小
#define STACK_TOP_MAX	STACK_TOP	// 堆栈顶部最大值同样设为任务大小

/* This decides where the kernel will search for a free chunk of vm
 * space during mmap's.
 */
/*
 * 这决定了内核在进行内存映射时会在哪里搜索空闲的虚拟内存块。
 */
#define TASK_UNMAPPED_BASE      (PAGE_ALIGN(TASK_SIZE / 3))

/* THREAD_SIZE is the size of the task_struct/kernel_stack combo.
 * normally, the stack is found by doing something like p + THREAD_SIZE
 * in CRIS, a page is 8192 bytes, which seems like a sane size
 */
/*
 * THREAD_SIZE 是 task_struct/kernel_stack 组合的大小。
 * 通常，可以通过 p + THREAD_SIZE 来找到堆栈。
 * 在 CRIS 架构中，一个页面是 8192 字节，这似乎是一个合理的大小。
 */

#define THREAD_SIZE       PAGE_SIZE
#define KERNEL_STACK_SIZE PAGE_SIZE

/*
 * At user->kernel entry, the pt_regs struct is stacked on the top of the kernel-stack.
 * This macro allows us to find those regs for a task.
 * Notice that subsequent pt_regs stackings, like recursive interrupts occurring while
 * we're in the kernel, won't affect this - only the first user->kernel transition
 * registers are reached by this.
 */
/*
 * 在用户到内核的转换时，pt_regs 结构被压入内核栈顶部。
 * 这个宏允许我们为一个任务找到这些寄存器。
 * 注意，随后的 pt_regs 压栈，比如当我们在内核中时发生的递归中断，不会影响此处 -
 * 只有第一次用户到内核转换的寄存器可以通过此方式访问。
 */

#define user_regs(thread_info) (((struct pt_regs *)((unsigned long)(thread_info) + THREAD_SIZE)) - 1)

/*
 * Dito but for the currently running task
 */
/*
 * 同上，但适用于当前运行的任务
 */

#define task_pt_regs(task) user_regs(task_thread_info(task))
#define current_regs() task_pt_regs(current)

static inline void prepare_to_copy(struct task_struct *tsk)
{
	// 准备复制任务结构
}

extern int kernel_thread(int (*fn)(void *), void * arg, unsigned long flags);

unsigned long get_wchan(struct task_struct *p);

// 获取任务的用户栈指针
#define KSTK_ESP(tsk)   ((tsk) == current ? rdusp() : (tsk)->thread.usp)

extern unsigned long thread_saved_pc(struct task_struct *tsk);

/* Free all resources held by a thread. */
static inline void release_thread(struct task_struct *dead_task)
{
        /* Nothing needs to be done.  */
				// 释放线程所持有的所有资源
}

// 初始化栈
#define init_stack      (init_thread_union.stack)

// 内存屏障
#define cpu_relax()     barrier()

#endif /* __ASM_CRIS_PROCESSOR_H */
