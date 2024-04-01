#ifndef _ASM_X86_HARDIRQ_H
#define _ASM_X86_HARDIRQ_H

#include <linux/threads.h>
#include <linux/irq.h>

typedef struct {
		// __softirq_pending 和 __nmi_count 字段提供了软中断和非屏蔽中断的计数，这两种中断类型对系统稳定性和响应时间至关重要。	
    unsigned int __softirq_pending; /* 等待处理的软中断数量 */
    unsigned int __nmi_count;       /* 依赖于架构的非屏蔽中断计数 */
		// irq0_irqs 字段通常记录系统时钟中断的发生次数，这是操作系统时间管理和调度的基础。
    unsigned int irq0_irqs;         /* IRQ0的中断次数，通常指时钟中断 */
		// 在启用了CONFIG_X86_LOCAL_APIC配置的系统中，apic_timer_irqs 记录了本地APIC定时器中断的次数，irq_spurious_count 记录了伪中断的次数。伪中断是指那些系统认为发生了但实际上并未真正发生的中断，它们可能指示着潜在的硬件问题或配置错误。
#ifdef CONFIG_X86_LOCAL_APIC
    unsigned int apic_timer_irqs;   /* 本地APIC定时器中断次数，依赖于架构 */
    unsigned int irq_spurious_count;/* 伪中断的数量 */
#endif
		// x86_platform_ipis 字段记录了在x86平台上发生的处理器间中断的次数，这些中断用于处理器之间的通信和同步。
    unsigned int x86_platform_ipis; /* x86平台的IPIs(Inter-Processor Interrupts)数量，依赖于架构 */
		// apic_perf_irqs 和 apic_pending_irqs 字段分别跟踪了APIC性能监控中断和待处理的APIC中断的数量。
    unsigned int apic_perf_irqs;    /* APIC性能监控中断的数量 */
    unsigned int apic_pending_irqs; /* 待处理的APIC中断数量 */
		// 在多处理器系统（CONFIG_SMP配置启用）中，irq_resched_count、irq_call_count 和 irq_tlb_count 字段分别记录了调度中断、函数调用中断和TLB无效化中断的次数，这些中断对于维护多核系统的性能和一致性至关重要。
#ifdef CONFIG_SMP
    unsigned int irq_resched_count; /* 调度中断的数量 */
    unsigned int irq_call_count;    /* 函数调用中断的数量 */
    unsigned int irq_tlb_count;     /* TLB(Translation Lookaside Buffer)无效化中断的数量 */
#endif
	// rq_thermal_count、irq_threshold_count 字段在相应的配置（如CONFIG_X86_THERMAL_VECTOR 和 CONFIG_X86_MCE_THRESHOLD）启用时出现，它们分别记录了热管理中断和机器检查异常阈值中断的次数，这些中断对于系统的健康状态和稳定运行至关重要。
#ifdef CONFIG_X86_THERMAL_VECTOR
    unsigned int irq_thermal_count; /* 热相关中断的数量 */
#endif
#ifdef CONFIG_X86_MCE_THRESHOLD
    unsigned int irq_threshold_count; /* 机器检查异常阈值中断的数量 */
#endif
} ____cacheline_aligned irq_cpustat_t; /* 确保按缓存行对齐，优化性能 */

DECLARE_PER_CPU_SHARED_ALIGNED(irq_cpustat_t, irq_stat);

/* We can have at most NR_VECTORS irqs routed to a cpu at a time */
#define MAX_HARDIRQS_PER_CPU NR_VECTORS

#define __ARCH_IRQ_STAT

#define inc_irq_stat(member)	percpu_add(irq_stat.member, 1)

#define local_softirq_pending()	percpu_read(irq_stat.__softirq_pending)

#define __ARCH_SET_SOFTIRQ_PENDING

#define set_softirq_pending(x)	percpu_write(irq_stat.__softirq_pending, (x))
#define or_softirq_pending(x)	percpu_or(irq_stat.__softirq_pending, (x))

extern void ack_bad_irq(unsigned int irq);

extern u64 arch_irq_stat_cpu(unsigned int cpu);
#define arch_irq_stat_cpu	arch_irq_stat_cpu

extern u64 arch_irq_stat(void);
#define arch_irq_stat		arch_irq_stat

#endif /* _ASM_X86_HARDIRQ_H */
