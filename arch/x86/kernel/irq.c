/*
 * Common interrupt code for 32 and 64 bit
 */
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/seq_file.h>
#include <linux/smp.h>
#include <linux/ftrace.h>

#include <asm/apic.h>
#include <asm/io_apic.h>
#include <asm/irq.h>
#include <asm/idle.h>
#include <asm/mce.h>
#include <asm/hw_irq.h>

atomic_t irq_err_count;

/* Function pointer for generic interrupt vector handling */
void (*x86_platform_ipi_callback)(void) = NULL;

/*
 * 'what should we do if we get a hw irq event on an illegal vector'.
 * each architecture has to answer this themselves.
 */
void ack_bad_irq(unsigned int irq)
{
	if (printk_ratelimit())
		pr_err("unexpected IRQ trap at vector %02x\n", irq);

	/*
	 * Currently unexpected vectors happen only on SMP and APIC.
	 * We _must_ ack these because every local APIC has only N
	 * irq slots per priority level, and a 'hanging, unacked' IRQ
	 * holds up an irq slot - in excessive cases (when multiple
	 * unexpected vectors occur) that might lock up the APIC
	 * completely.
	 * But only ack when the APIC is enabled -AK
	 */
	ack_APIC_irq();
}

#define irq_stats(x)		(&per_cpu(irq_stat, x))
/*
 * /proc/interrupts printing:
 */
static int show_other_interrupts(struct seq_file *p, int prec)
{
	int j;

	seq_printf(p, "%*s: ", prec, "NMI");
	for_each_online_cpu(j)
		seq_printf(p, "%10u ", irq_stats(j)->__nmi_count);
	seq_printf(p, "  Non-maskable interrupts\n");
#ifdef CONFIG_X86_LOCAL_APIC
	seq_printf(p, "%*s: ", prec, "LOC");
	for_each_online_cpu(j)
		seq_printf(p, "%10u ", irq_stats(j)->apic_timer_irqs);
	seq_printf(p, "  Local timer interrupts\n");

	seq_printf(p, "%*s: ", prec, "SPU");
	for_each_online_cpu(j)
		seq_printf(p, "%10u ", irq_stats(j)->irq_spurious_count);
	seq_printf(p, "  Spurious interrupts\n");
	seq_printf(p, "%*s: ", prec, "PMI");
	for_each_online_cpu(j)
		seq_printf(p, "%10u ", irq_stats(j)->apic_perf_irqs);
	seq_printf(p, "  Performance monitoring interrupts\n");
	seq_printf(p, "%*s: ", prec, "PND");
	for_each_online_cpu(j)
		seq_printf(p, "%10u ", irq_stats(j)->apic_pending_irqs);
	seq_printf(p, "  Performance pending work\n");
#endif
	if (x86_platform_ipi_callback) {
		seq_printf(p, "%*s: ", prec, "PLT");
		for_each_online_cpu(j)
			seq_printf(p, "%10u ", irq_stats(j)->x86_platform_ipis);
		seq_printf(p, "  Platform interrupts\n");
	}
#ifdef CONFIG_SMP
	seq_printf(p, "%*s: ", prec, "RES");
	for_each_online_cpu(j)
		seq_printf(p, "%10u ", irq_stats(j)->irq_resched_count);
	seq_printf(p, "  Rescheduling interrupts\n");
	seq_printf(p, "%*s: ", prec, "CAL");
	for_each_online_cpu(j)
		seq_printf(p, "%10u ", irq_stats(j)->irq_call_count);
	seq_printf(p, "  Function call interrupts\n");
	seq_printf(p, "%*s: ", prec, "TLB");
	for_each_online_cpu(j)
		seq_printf(p, "%10u ", irq_stats(j)->irq_tlb_count);
	seq_printf(p, "  TLB shootdowns\n");
#endif
#ifdef CONFIG_X86_THERMAL_VECTOR
	seq_printf(p, "%*s: ", prec, "TRM");
	for_each_online_cpu(j)
		seq_printf(p, "%10u ", irq_stats(j)->irq_thermal_count);
	seq_printf(p, "  Thermal event interrupts\n");
#endif
#ifdef CONFIG_X86_MCE_THRESHOLD
	seq_printf(p, "%*s: ", prec, "THR");
	for_each_online_cpu(j)
		seq_printf(p, "%10u ", irq_stats(j)->irq_threshold_count);
	seq_printf(p, "  Threshold APIC interrupts\n");
#endif
#ifdef CONFIG_X86_MCE
	seq_printf(p, "%*s: ", prec, "MCE");
	for_each_online_cpu(j)
		seq_printf(p, "%10u ", per_cpu(mce_exception_count, j));
	seq_printf(p, "  Machine check exceptions\n");
	seq_printf(p, "%*s: ", prec, "MCP");
	for_each_online_cpu(j)
		seq_printf(p, "%10u ", per_cpu(mce_poll_count, j));
	seq_printf(p, "  Machine check polls\n");
#endif
	seq_printf(p, "%*s: %10u\n", prec, "ERR", atomic_read(&irq_err_count));
#if defined(CONFIG_X86_IO_APIC)
	seq_printf(p, "%*s: %10u\n", prec, "MIS", atomic_read(&irq_mis_count));
#endif
	return 0;
}

int show_interrupts(struct seq_file *p, void *v)
{
	unsigned long flags, any_count = 0;
	int i = *(loff_t *) v, j, prec;
	struct irqaction *action;
	struct irq_desc *desc;

	if (i > nr_irqs)
		return 0;

	for (prec = 3, j = 1000; prec < 10 && j <= nr_irqs; ++prec)
		j *= 10;

	if (i == nr_irqs)
		return show_other_interrupts(p, prec);

	/* print header */
	if (i == 0) {
		seq_printf(p, "%*s", prec + 8, "");
		for_each_online_cpu(j)
			seq_printf(p, "CPU%-8d", j);
		seq_putc(p, '\n');
	}

	desc = irq_to_desc(i);
	if (!desc)
		return 0;

	raw_spin_lock_irqsave(&desc->lock, flags);
	for_each_online_cpu(j)
		any_count |= kstat_irqs_cpu(i, j);
	action = desc->action;
	if (!action && !any_count)
		goto out;

	seq_printf(p, "%*d: ", prec, i);
	for_each_online_cpu(j)
		seq_printf(p, "%10u ", kstat_irqs_cpu(i, j));
	seq_printf(p, " %8s", desc->chip->name);
	seq_printf(p, "-%-8s", desc->name);

	if (action) {
		seq_printf(p, "  %s", action->name);
		while ((action = action->next) != NULL)
			seq_printf(p, ", %s", action->name);
	}

	seq_putc(p, '\n');
out:
	raw_spin_unlock_irqrestore(&desc->lock, flags);
	return 0;
}

/*
 * /proc/stat helpers
 */
u64 arch_irq_stat_cpu(unsigned int cpu)
{
	u64 sum = irq_stats(cpu)->__nmi_count;

#ifdef CONFIG_X86_LOCAL_APIC
	sum += irq_stats(cpu)->apic_timer_irqs;
	sum += irq_stats(cpu)->irq_spurious_count;
	sum += irq_stats(cpu)->apic_perf_irqs;
	sum += irq_stats(cpu)->apic_pending_irqs;
#endif
	if (x86_platform_ipi_callback)
		sum += irq_stats(cpu)->x86_platform_ipis;
#ifdef CONFIG_SMP
	sum += irq_stats(cpu)->irq_resched_count;
	sum += irq_stats(cpu)->irq_call_count;
	sum += irq_stats(cpu)->irq_tlb_count;
#endif
#ifdef CONFIG_X86_THERMAL_VECTOR
	sum += irq_stats(cpu)->irq_thermal_count;
#endif
#ifdef CONFIG_X86_MCE_THRESHOLD
	sum += irq_stats(cpu)->irq_threshold_count;
#endif
#ifdef CONFIG_X86_MCE
	sum += per_cpu(mce_exception_count, cpu);
	sum += per_cpu(mce_poll_count, cpu);
#endif
	return sum;
}

u64 arch_irq_stat(void)
{
	u64 sum = atomic_read(&irq_err_count);

#ifdef CONFIG_X86_IO_APIC
	sum += atomic_read(&irq_mis_count);
#endif
	return sum;
}


/*
 * do_IRQ handles all normal device IRQ's (the special
 * SMP cross-CPU interrupts have their own specific
 * handlers).
 */
// x86的do_IRQ，执行中断的函数，处理器接收中断，然后就执行这个函数
unsigned int __irq_entry do_IRQ(struct pt_regs *regs)
{
	// 保存当前的寄存器状态，以便中断处理完成后能恢复。
	struct pt_regs *old_regs = set_irq_regs(regs);

	/* high bit used in ret_from_ code  */
	/* 在返回从中断返回的代码中使用的高位 */
	unsigned vector = ~regs->orig_ax;
	unsigned irq;

	exit_idle();	// 如果CPU处于空闲状态，执行退出空闲状态的操作。
	irq_enter();	// 标记中断处理的开始，执行一些统计和跟踪中断的操作。

	irq = __get_cpu_var(vector_irq)[vector];		// 获取irq号

	if (!handle_irq(irq, regs)) {		// 通过handle_irq调用irq号对应的中断
	// 如果没有找到处理这个IRQ的中断处理函数，则执行以下代码。
		ack_APIC_irq();	// 确认APIC（高级可编程中断控制器）中断，告知已经处理完毕。

		// 使用速率限制打印错误消息，避免在短时间内打印过多信息。
		if (printk_ratelimit())
			pr_emerg("%s: %d.%d No irq handler for vector (irq %d)\n",
				__func__, smp_processor_id(), vector, irq);
			// 打印出没有为这个中断向量找到处理函数的紧急错误消息。
	}

	irq_exit();	// 标记中断处理的结束，执行一些清理操作。

	set_irq_regs(old_regs);		// 恢复之前保存的寄存器状态。
	return 1;
}

/*
 * Handler for X86_PLATFORM_IPI_VECTOR.
 */
void smp_x86_platform_ipi(struct pt_regs *regs)
{
	struct pt_regs *old_regs = set_irq_regs(regs);

	ack_APIC_irq();

	exit_idle();

	irq_enter();

	inc_irq_stat(x86_platform_ipis);

	if (x86_platform_ipi_callback)
		x86_platform_ipi_callback();

	irq_exit();

	set_irq_regs(old_regs);
}

EXPORT_SYMBOL_GPL(vector_used_by_percpu_irq);

#ifdef CONFIG_HOTPLUG_CPU
/* A cpu has been removed from cpu_online_mask.  Reset irq affinities. */
void fixup_irqs(void)
{
	unsigned int irq, vector;
	static int warned;
	struct irq_desc *desc;

	for_each_irq_desc(irq, desc) {
		int break_affinity = 0;
		int set_affinity = 1;
		const struct cpumask *affinity;

		if (!desc)
			continue;
		if (irq == 2)
			continue;

		/* interrupt's are disabled at this point */
		raw_spin_lock(&desc->lock);

		affinity = desc->affinity;
		if (!irq_has_action(irq) ||
		    cpumask_equal(affinity, cpu_online_mask)) {
			raw_spin_unlock(&desc->lock);
			continue;
		}

		/*
		 * Complete the irq move. This cpu is going down and for
		 * non intr-remapping case, we can't wait till this interrupt
		 * arrives at this cpu before completing the irq move.
		 */
		irq_force_complete_move(irq);

		if (cpumask_any_and(affinity, cpu_online_mask) >= nr_cpu_ids) {
			break_affinity = 1;
			affinity = cpu_all_mask;
		}

		if (!(desc->status & IRQ_MOVE_PCNTXT) && desc->chip->mask)
			desc->chip->mask(irq);

		if (desc->chip->set_affinity)
			desc->chip->set_affinity(irq, affinity);
		else if (!(warned++))
			set_affinity = 0;

		if (!(desc->status & IRQ_MOVE_PCNTXT) && desc->chip->unmask)
			desc->chip->unmask(irq);

		raw_spin_unlock(&desc->lock);

		if (break_affinity && set_affinity)
			printk("Broke affinity for irq %i\n", irq);
		else if (!set_affinity)
			printk("Cannot set affinity for irq %i\n", irq);
	}

	/*
	 * We can remove mdelay() and then send spuriuous interrupts to
	 * new cpu targets for all the irqs that were handled previously by
	 * this cpu. While it works, I have seen spurious interrupt messages
	 * (nothing wrong but still...).
	 *
	 * So for now, retain mdelay(1) and check the IRR and then send those
	 * interrupts to new targets as this cpu is already offlined...
	 */
	mdelay(1);

	for (vector = FIRST_EXTERNAL_VECTOR; vector < NR_VECTORS; vector++) {
		unsigned int irr;

		if (__get_cpu_var(vector_irq)[vector] < 0)
			continue;

		irr = apic_read(APIC_IRR + (vector / 32 * 0x10));
		if (irr  & (1 << (vector % 32))) {
			irq = __get_cpu_var(vector_irq)[vector];

			desc = irq_to_desc(irq);
			raw_spin_lock(&desc->lock);
			if (desc->chip->retrigger)
				desc->chip->retrigger(irq);
			raw_spin_unlock(&desc->lock);
		}
	}
}
#endif
