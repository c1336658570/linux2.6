/*
 *	linux/kernel/softirq.c
 *
 *	Copyright (C) 1992 Linus Torvalds
 *
 *	Distribute under GPLv2.
 *
 *	Rewritten. Old one was good in 2.2, but in 2.3 it was immoral. --ANK (990903)
 *
 *	Remote softirq infrastructure is by Jens Axboe.
 */

#include <linux/module.h>
#include <linux/kernel_stat.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/notifier.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>
#include <linux/ftrace.h>
#include <linux/smp.h>
#include <linux/tick.h>

#define CREATE_TRACE_POINTS
#include <trace/events/irq.h>

#include <asm/irq.h>
/*
   - No shared variables, all the data are CPU local.
   - If a softirq needs serialization, let it serialize itself
     by its own spinlocks.
   - Even if softirq is serialized, only local cpu is marked for
     execution. Hence, we get something sort of weak cpu binding.
     Though it is still not clear, will it result in better locality
     or will not.

   Examples:
   - NET RX softirq. It is multithreaded and does not require
     any global serialization.
   - NET TX softirq. It kicks software netdevice queues, hence
     it is logically serialized per device, but this serialization
     is invisible to common code.
   - Tasklets: serialized wrt itself.
 */

#ifndef __ARCH_IRQ_STAT
irq_cpustat_t irq_stat[NR_CPUS] ____cacheline_aligned;	// 每个cpu一个irq_stat
EXPORT_SYMBOL(irq_stat);
#endif

// 包含32个软中断的数组，每个被注册的软中断都占一项，软中断最多有32个
// 内核运行一个软中断处理程序时，就会执行softirq_vec数组中的一项元素的action这个函数
static struct softirq_action softirq_vec[NR_SOFTIRQS] __cacheline_aligned_in_smp;

static DEFINE_PER_CPU(struct task_struct *, ksoftirqd);

char *softirq_to_name[NR_SOFTIRQS] = {
	"HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "BLOCK_IOPOLL",
	"TASKLET", "SCHED", "HRTIMER",	"RCU"
};

/*
 * we cannot loop indefinitely here to avoid userspace starvation,
 * but we also don't want to introduce a worst case 1/HZ latency
 * to the pending events, so lets the scheduler to balance
 * the softirq load for us.
 */
/*
在这里我们不能无限循环以避免用户空间饥饿，
但我们也不想给待处理事件引入最坏情况下的 1/HZ 的延迟，
所以让调度器为我们平衡软中断的负载。
*/
// 唤醒ksoftirqd，即执行软中断的内核线程
void wakeup_softirqd(void)
{
	/* Interrupts are disabled: no need to stop preemption */
	struct task_struct *tsk = __get_cpu_var(ksoftirqd);

	// 唤醒ksoftirqd，即执行软中断的内核线程
	if (tsk && tsk->state != TASK_RUNNING)
		wake_up_process(tsk);
}

/*
 * This one is for softirq.c-internal use,
 * where hardirqs are disabled legitimately:
 */
#ifdef CONFIG_TRACE_IRQFLAGS
static void __local_bh_disable(unsigned long ip)
{
	unsigned long flags;

	WARN_ON_ONCE(in_irq());

	raw_local_irq_save(flags);
	/*
	 * The preempt tracer hooks into add_preempt_count and will break
	 * lockdep because it calls back into lockdep after SOFTIRQ_OFFSET
	 * is set and before current->softirq_enabled is cleared.
	 * We must manually increment preempt_count here and manually
	 * call the trace_preempt_off later.
	 */
	preempt_count() += SOFTIRQ_OFFSET;
	/*
	 * Were softirqs turned off above:
	 */
	if (softirq_count() == SOFTIRQ_OFFSET)
		trace_softirqs_off(ip);
	raw_local_irq_restore(flags);

	if (preempt_count() == SOFTIRQ_OFFSET)
		trace_preempt_off(CALLER_ADDR0, get_parent_ip(CALLER_ADDR1));
}
#else /* !CONFIG_TRACE_IRQFLAGS */
static inline void __local_bh_disable(unsigned long ip)
{
	add_preempt_count(SOFTIRQ_OFFSET);
	barrier();
}
#endif /* CONFIG_TRACE_IRQFLAGS */

// 通过增加preempt_count禁止本地中断的下半部(软中断和tasklet)。有几次local_bh_disable，就需要有几次local_bh_enable
void local_bh_disable(void)
{
	__local_bh_disable((unsigned long)__builtin_return_address(0));
}

EXPORT_SYMBOL(local_bh_disable);

/*
 * Special-case - softirqs can safely be enabled in
 * cond_resched_softirq(), or by __do_softirq(),
 * without processing still-pending softirqs:
 */
void _local_bh_enable(void)
{
	WARN_ON_ONCE(in_irq());
	WARN_ON_ONCE(!irqs_disabled());

	if (softirq_count() == SOFTIRQ_OFFSET)
		trace_softirqs_on((unsigned long)__builtin_return_address(0));
	sub_preempt_count(SOFTIRQ_OFFSET);
}

EXPORT_SYMBOL(_local_bh_enable);

static inline void _local_bh_enable_ip(unsigned long ip)
{
	WARN_ON_ONCE(in_irq() || irqs_disabled());
#ifdef CONFIG_TRACE_IRQFLAGS
	local_irq_disable();
#endif
	/*
	 * Are softirqs going to be turned on now:
	 */
	if (softirq_count() == SOFTIRQ_OFFSET)
		trace_softirqs_on(ip);
	/*
	 * Keep preemption disabled until we are done with
	 * softirq processing:
 	 */
 	sub_preempt_count(SOFTIRQ_OFFSET - 1);

	// 不在软中断和硬中断中，并且有软中断待处理
	if (unlikely(!in_interrupt() && local_softirq_pending()))
		do_softirq();

	dec_preempt_count();
#ifdef CONFIG_TRACE_IRQFLAGS
	local_irq_enable();
#endif
	preempt_check_resched();
}

// 通过减少preempt_count来激活本地中断的下半部。有几次local_bh_disable，就需要有几次local_bh_enable
// 如果preempt_count返回值为0,则将导致自动激活下半部（即该函数内部判断preempt_count是否为0,如果为0就主动调用do_softirq()）
void local_bh_enable(void)
{
	_local_bh_enable_ip((unsigned long)__builtin_return_address(0));
}
EXPORT_SYMBOL(local_bh_enable);

void local_bh_enable_ip(unsigned long ip)
{
	_local_bh_enable_ip(ip);
}
EXPORT_SYMBOL(local_bh_enable_ip);

/*
 * We restart softirq processing MAX_SOFTIRQ_RESTART times,
 * and we fall back to softirqd after that.
 *
 * This number has been established via experimentation.
 * The two things to balance is latency against fairness -
 * we want to handle softirqs as soon as possible, but they
 * should not be able to lock up the box.
 */
/*
 * 定义了软中断（softirq）处理的重启次数上限。如果处理次数达到这个上限，
 * 之后的处理会退回到软中断守护进程（softirqd）进行。这个上限值是通过实验确定的。
 * 要平衡的两个因素是延迟与公平性——我们希望尽可能快地处理软中断，但它们不应该导致系统锁死。
 */
#define MAX_SOFTIRQ_RESTART 10

// 执行软中断
// 只要do_softirq()函数发现已经执行过的内核线程重新触发了它自己，软中断内核线程就会被唤醒
asmlinkage void __do_softirq(void)
{
	struct softirq_action *h;
	__u32 pending;
	int max_restart = MAX_SOFTIRQ_RESTART;
	int cpu;

	pending = local_softirq_pending();	// 获取待处理的软中断的32位位图
	account_system_vtime(current);

	__local_bh_disable((unsigned long)__builtin_return_address(0));
	lockdep_softirq_enter();

	cpu = smp_processor_id();
restart:
	/* Reset the pending bitmask before enabling irqs */
	// 重设待处理的位图，即清零，因为软中断位图已经保存了
	set_softirq_pending(0);

	local_irq_enable();		// 开启中断

	h = softirq_vec;		// 将指针h指向softirq_vec的第一项

	do {
		if (pending & 1) {
			// 如果pending的第一位被设置则h->action(h)被调用
			int prev_count = preempt_count();
			kstat_incr_softirqs_this_cpu(h - softirq_vec);

			trace_softirq_entry(h, softirq_vec);
			h->action(h);		// 调用h->action，如果是tasklet，则此处调用的就是下面的tasklet_action和tasklet_hi_action
			trace_softirq_exit(h, softirq_vec);
			if (unlikely(prev_count != preempt_count())) {
				printk(KERN_ERR "huh, entered softirq %td %s %p"
				       "with preempt_count %08x,"
				       " exited with %08x?\n", h - softirq_vec,
				       softirq_to_name[h - softirq_vec],
				       h->action, prev_count, preempt_count());
				preempt_count() = prev_count;
			}

			rcu_bh_qs(cpu);
		}
		h++;		// 指针加1,指向下一项
		pending >>= 1;	// 位掩码pending右移一位
	} while (pending);		// 反复执行，知道pending为0，该循环最多执行32次，所以可以保证h指向softirq_vec的有效项

	local_irq_disable();		// 关中断

	pending = local_softirq_pending();	// 再次获取软中断位图
	// 如果还有软中断没处理（因为软中断中可能调用raise_softirq再次触发软中断）而且最大重启次数大与0,就再次去处理软中断
	if (pending && --max_restart)
		goto restart;

	// 只要do_softirq()函数发现已经执行过的内核线程重新触发了它自己，软中断内核线程就会被唤醒
	// 唤醒ksoftirqd，即执行软中断的内核线程
	if (pending)	// 如果还有软中断没处理就唤醒softirqd/n
		wakeup_softirqd();

	lockdep_softirq_exit();

	account_system_vtime(current);
	_local_bh_enable();
}

#ifndef __ARCH_HAS_DO_SOFTIRQ

// 执行软中断的函数
// 只要do_softirq()函数发现已经执行过的内核线程重新触发了它自己，软中断内核线程就会被唤醒
asmlinkage void do_softirq(void)
{
	// 如果有待处理的软中断，do_softirq()会循环遍历每一个，调用它们的处理程序。
	__u32 pending;	// 全局变量pending保存local_softirq_pending的返回值，它时待处理的软中断的32位位图，如果第n位被设置为1,那么对应类型的软中断等待处理
	unsigned long flags;

	if (in_interrupt())		// 已经在中断中（软中断或硬中断）
		return;

	local_irq_save(flags);		// 禁用中断，防止在保存位图和清除它的间隙，有一个新的软中断，这可能会造成对待次处理的位进行不应该的清0

	pending = local_softirq_pending();		// 获取待处理的软中断的32位位图

	if (pending)
		__do_softirq();

	local_irq_restore(flags);
}

#endif

/*
 * Enter an interrupt context.
 */
void irq_enter(void)
{
	int cpu = smp_processor_id();

	rcu_irq_enter();
	if (idle_cpu(cpu) && !in_interrupt()) {
		__irq_enter();
		tick_check_idle(cpu);
	} else
		__irq_enter();
}

#ifdef __ARCH_IRQ_EXIT_IRQS_DISABLED
# define invoke_softirq()	__do_softirq()
#else
# define invoke_softirq()	do_softirq()
#endif

/*
 * Exit an interrupt context. Process softirqs if needed and possible:
 */
void irq_exit(void)
{
	account_system_vtime(current);
	trace_hardirq_exit();
	sub_preempt_count(IRQ_EXIT_OFFSET);
	if (!in_interrupt() && local_softirq_pending())
		invoke_softirq();

	rcu_irq_exit();
#ifdef CONFIG_NO_HZ
	/* Make sure that timer wheel updates are propagated */
	if (idle_cpu(smp_processor_id()) && !in_interrupt() && !need_resched())
		tick_nohz_stop_sched_tick(0);
#endif
	preempt_enable_no_resched();
}

/*
 * This function must run with irqs disabled!
 */
inline void raise_softirq_irqoff(unsigned int nr)
{
	__raise_softirq_irqoff(nr);		// 通过__raise_softirq_irqoff设置cpu的软中断pending标志位

	/*
	 * If we're in an interrupt or softirq, we're done
	 * (this also catches softirq-disabled code). We will
	 * actually run the softirq once we return from
	 * the irq or softirq.
	 *
	 * Otherwise we wake up ksoftirqd to make sure we
	 * schedule the softirq soon.
	 */
	// 通过in_interrupt判断现在是否在中断上下文中，或者软中断是否被禁止，如果都不成立，
	// 则唤醒软中断的守护进程，在守护进程中执行软中 断的回调函数。否则什么也不做，软中断将会在中断的退出阶段被执行。
	if (!in_interrupt())
		wakeup_softirqd();
}

// 可以将一个软中断设置为挂起状态，让它在下次调用do_softirq()函数时投入运行。
// raise_softirq(NET_TX_SOFTIRQ)，这会触发NET_TX_SOFTIRQ软中断。它的处理程序会在下次内核执行软中断时投入运行。
// 该函数在触发软中断之前先要禁止中断，触发后再恢复。如果中断本来就已经被禁止了，就可以调用另一函数raise_softirq_irqoff
// 主动唤起一个软中断，会首先设置__softirq_pending对应的软中断位为挂起，然后检查in_interrupt，如果不在中断中，则唤起ksoftirq线程执行软中断
void raise_softirq(unsigned int nr)
{
	unsigned long flags;

	local_irq_save(flags);
	raise_softirq_irqoff(nr);
	local_irq_restore(flags);
}

// 注册软中断处理程序，俩参数，软中断的索引号和处理函数
void open_softirq(int nr, void (*action)(struct softirq_action *))
{
	softirq_vec[nr].action = action;
}

/*
 * Tasklets
 */
struct tasklet_head
{
	struct tasklet_struct *head;
	struct tasklet_struct **tail;
};

// 定义两个单处理器的数据结构，一个tasklet_vec存放正常优先级的tasklet，一个tasklet_hi_vec存放高优先级的tasklet
static DEFINE_PER_CPU(struct tasklet_head, tasklet_vec);
static DEFINE_PER_CPU(struct tasklet_head, tasklet_hi_vec);

void __tasklet_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	// 保存中断状态，并禁止本地中断。这样做保证tasklet_schedule()处理这些tasklet时，处理器上数据不会弄乱
	local_irq_save(flags);
	t->next = NULL;
	// 把需要调度的tasklet加到每个处理器一个的tasklet_vec链表的表头上去
	*__get_cpu_var(tasklet_vec).tail = t;
	__get_cpu_var(tasklet_vec).tail = &(t->next);
	// 唤起TASKLET_SOFTIRQ软中断，这样在下一次调用do_softirq()时就会执行该tasklet
	raise_softirq_irqoff(TASKLET_SOFTIRQ);
	local_irq_restore(flags);		// 恢复中断到原状态并返回
}

EXPORT_SYMBOL(__tasklet_schedule);

void __tasklet_hi_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	// 保存中断状态，并禁止本地中断。这样做保证tasklet_hi_schedule()处理这些tasklet时，处理器上数据不会弄乱
	local_irq_save(flags);
	t->next = NULL;
	// 把需要调度的tasklet加到每个处理器一个的tasklet_hi_vec链表的表头上去
	*__get_cpu_var(tasklet_hi_vec).tail = t;
	__get_cpu_var(tasklet_hi_vec).tail = &(t->next);
	// 唤起HI_SOFTIRQ软中断，这样在下一次调用do_softirq()时就会执行该tasklet
	raise_softirq_irqoff(HI_SOFTIRQ);
	local_irq_restore(flags);
}

EXPORT_SYMBOL(__tasklet_hi_schedule);

void __tasklet_hi_schedule_first(struct tasklet_struct *t)
{
	BUG_ON(!irqs_disabled());

	t->next = __get_cpu_var(tasklet_hi_vec).head;
	__get_cpu_var(tasklet_hi_vec).head = t;
	__raise_softirq_irqoff(HI_SOFTIRQ);
}

EXPORT_SYMBOL(__tasklet_hi_schedule_first);

// tasklet_action()就是tasklet处理的核心。由do_softirq()执行
static void tasklet_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	local_irq_disable();		// 禁止中断
	// 检索tasklet_vec链表
	list = __get_cpu_var(tasklet_vec).head;
	// 将当前处理器上的链表设置为空，达到清空的效果
	__get_cpu_var(tasklet_vec).head = NULL;
	__get_cpu_var(tasklet_vec).tail = &__get_cpu_var(tasklet_vec).head;
	local_irq_enable();		// 开中断，不需要恢复为原来状态，因为这段程序时软中断处理程序，就应该允许中断。

	// 遍历链表上的每一个tasklet
	while (list) {
		struct tasklet_struct *t = list;

		list = list->next;

		// 如果是多处理器，通过tasklet_trylock检查TASKLET_STATE_RUN来判断这个tasklet是否正在其他处理器上运行。如果正在运行，直接跳转到下一个tasklet。如果没在运行就将其设置成正在运行
		// 如果这个tasklet没有运行，设置状态为TASKLET_STATE_RUN
		if (tasklet_trylock(t)) {
			// 检查count是否为0，确保tasklet没有被禁止
			if (!atomic_read(&t->count)) {
				// 检查此tasklet是否正在调度
				if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
					BUG();
				t->func(t->data);			// 执行tasklet
				tasklet_unlock(t);		// 清除tasklet的state域的TASKLET_STATE_RUN状态标志
				continue;							// 进入下次循环执行下一个tasklet
			}
			tasklet_unlock(t);
		}

		// 如果当前tasklet正在其他处理器运行，就把它添加到末尾，等之后再运行
		local_irq_disable();
		t->next = NULL;
		*__get_cpu_var(tasklet_vec).tail = t;		// tail存储了最后一个tasklet节点的next域的地址，通过这样直接修改最后一个节点的next域
		__get_cpu_var(tasklet_vec).tail = &(t->next);		// 把tail改为最后一个节点的next域的地址。
		__raise_softirq_irqoff(TASKLET_SOFTIRQ);
		local_irq_enable();
	}
}

// tasklet_hi_action()就是tasklet处理的核心。由do_softirq()执行
static void tasklet_hi_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	local_irq_disable();
	list = __get_cpu_var(tasklet_hi_vec).head;
	__get_cpu_var(tasklet_hi_vec).head = NULL;
	__get_cpu_var(tasklet_hi_vec).tail = &__get_cpu_var(tasklet_hi_vec).head;
	local_irq_enable();

	while (list) {
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (!atomic_read(&t->count)) {
				if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
					BUG();
				t->func(t->data);
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}

		local_irq_disable();
		t->next = NULL;
		*__get_cpu_var(tasklet_hi_vec).tail = t;
		__get_cpu_var(tasklet_hi_vec).tail = &(t->next);
		__raise_softirq_irqoff(HI_SOFTIRQ);
		local_irq_enable();
	}
}


void tasklet_init(struct tasklet_struct *t,
		  void (*func)(unsigned long), unsigned long data)
{
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->func = func;
	t->data = data;
}

EXPORT_SYMBOL(tasklet_init);

// 从挂起的队列中去掉一个tasklet，参数是一个指向某个tasklet的tasklet_struct的指针。
// 在处理一个经常重新调度它自身的tasklet的时候，从挂起队列中移除已调度的tasklet很有用。该函数首先等待tasklet执行完，再将它移去
void tasklet_kill(struct tasklet_struct *t)
{
	if (in_interrupt())	// 检查是否在软中断或中断中
		printk("Attempt to kill tasklet from interrupt\n");

	// 检查tasklet是否正在等待调度，如果正在等待调度就进入循环。
	while (test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) {
		do {
			yield();
		} while (test_bit(TASKLET_STATE_SCHED, &t->state));	// 循环检查tasklet是否正在等待调度，直到不是正在等待调度退出循环
	}
	tasklet_unlock_wait(t);
	clear_bit(TASKLET_STATE_SCHED, &t->state);		// 清除tasklet等待调度的标志位，即从挂起队列移除
}

EXPORT_SYMBOL(tasklet_kill);

/*
 * tasklet_hrtimer
 */

/*
 * The trampoline is called when the hrtimer expires. It schedules a tasklet
 * to run __tasklet_hrtimer_trampoline() which in turn will call the intended
 * hrtimer callback, but from softirq context.
 */
static enum hrtimer_restart __hrtimer_tasklet_trampoline(struct hrtimer *timer)
{
	struct tasklet_hrtimer *ttimer =
		container_of(timer, struct tasklet_hrtimer, timer);

	tasklet_hi_schedule(&ttimer->tasklet);
	return HRTIMER_NORESTART;
}

/*
 * Helper function which calls the hrtimer callback from
 * tasklet/softirq context
 */
static void __tasklet_hrtimer_trampoline(unsigned long data)
{
	struct tasklet_hrtimer *ttimer = (void *)data;
	enum hrtimer_restart restart;

	restart = ttimer->function(&ttimer->timer);
	if (restart != HRTIMER_NORESTART)
		hrtimer_restart(&ttimer->timer);
}

/**
 * tasklet_hrtimer_init - Init a tasklet/hrtimer combo for softirq callbacks
 * @ttimer:	 tasklet_hrtimer which is initialized
 * @function:	 hrtimer callback funtion which gets called from softirq context
 * @which_clock: clock id (CLOCK_MONOTONIC/CLOCK_REALTIME)
 * @mode:	 hrtimer mode (HRTIMER_MODE_ABS/HRTIMER_MODE_REL)
 */
void tasklet_hrtimer_init(struct tasklet_hrtimer *ttimer,
			  enum hrtimer_restart (*function)(struct hrtimer *),
			  clockid_t which_clock, enum hrtimer_mode mode)
{
	hrtimer_init(&ttimer->timer, which_clock, mode);
	ttimer->timer.function = __hrtimer_tasklet_trampoline;
	tasklet_init(&ttimer->tasklet, __tasklet_hrtimer_trampoline,
		     (unsigned long)ttimer);
	ttimer->function = function;
}
EXPORT_SYMBOL_GPL(tasklet_hrtimer_init);

/*
 * Remote softirq bits
 */

DEFINE_PER_CPU(struct list_head [NR_SOFTIRQS], softirq_work_list);
EXPORT_PER_CPU_SYMBOL(softirq_work_list);

static void __local_trigger(struct call_single_data *cp, int softirq)
{
	struct list_head *head = &__get_cpu_var(softirq_work_list[softirq]);

	list_add_tail(&cp->list, head);

	/* Trigger the softirq only if the list was previously empty.  */
	if (head->next == &cp->list)
		raise_softirq_irqoff(softirq);
}

#ifdef CONFIG_USE_GENERIC_SMP_HELPERS
static void remote_softirq_receive(void *data)
{
	struct call_single_data *cp = data;
	unsigned long flags;
	int softirq;

	softirq = cp->priv;

	local_irq_save(flags);
	__local_trigger(cp, softirq);
	local_irq_restore(flags);
}

static int __try_remote_softirq(struct call_single_data *cp, int cpu, int softirq)
{
	if (cpu_online(cpu)) {
		cp->func = remote_softirq_receive;
		cp->info = cp;
		cp->flags = 0;
		cp->priv = softirq;

		__smp_call_function_single(cpu, cp, 0);
		return 0;
	}
	return 1;
}
#else /* CONFIG_USE_GENERIC_SMP_HELPERS */
static int __try_remote_softirq(struct call_single_data *cp, int cpu, int softirq)
{
	return 1;
}
#endif

/**
 * __send_remote_softirq - try to schedule softirq work on a remote cpu
 * @cp: private SMP call function data area
 * @cpu: the remote cpu
 * @this_cpu: the currently executing cpu
 * @softirq: the softirq for the work
 *
 * Attempt to schedule softirq work on a remote cpu.  If this cannot be
 * done, the work is instead queued up on the local cpu.
 *
 * Interrupts must be disabled.
 */
void __send_remote_softirq(struct call_single_data *cp, int cpu, int this_cpu, int softirq)
{
	if (cpu == this_cpu || __try_remote_softirq(cp, cpu, softirq))
		__local_trigger(cp, softirq);
}
EXPORT_SYMBOL(__send_remote_softirq);

/**
 * send_remote_softirq - try to schedule softirq work on a remote cpu
 * @cp: private SMP call function data area
 * @cpu: the remote cpu
 * @softirq: the softirq for the work
 *
 * Like __send_remote_softirq except that disabling interrupts and
 * computing the current cpu is done for the caller.
 */
void send_remote_softirq(struct call_single_data *cp, int cpu, int softirq)
{
	unsigned long flags;
	int this_cpu;

	local_irq_save(flags);
	this_cpu = smp_processor_id();
	__send_remote_softirq(cp, cpu, this_cpu, softirq);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(send_remote_softirq);

static int __cpuinit remote_softirq_cpu_notify(struct notifier_block *self,
					       unsigned long action, void *hcpu)
{
	/*
	 * If a CPU goes away, splice its entries to the current CPU
	 * and trigger a run of the softirq
	 */
	if (action == CPU_DEAD || action == CPU_DEAD_FROZEN) {
		int cpu = (unsigned long) hcpu;
		int i;

		local_irq_disable();
		for (i = 0; i < NR_SOFTIRQS; i++) {
			struct list_head *head = &per_cpu(softirq_work_list[i], cpu);
			struct list_head *local_head;

			if (list_empty(head))
				continue;

			local_head = &__get_cpu_var(softirq_work_list[i]);
			list_splice_init(head, local_head);
			raise_softirq_irqoff(i);
		}
		local_irq_enable();
	}

	return NOTIFY_OK;
}

static struct notifier_block __cpuinitdata remote_softirq_cpu_notifier = {
	.notifier_call	= remote_softirq_cpu_notify,
};

// 初始化软中断
void __init softirq_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		int i;

		per_cpu(tasklet_vec, cpu).tail =
			&per_cpu(tasklet_vec, cpu).head;
		per_cpu(tasklet_hi_vec, cpu).tail =
			&per_cpu(tasklet_hi_vec, cpu).head;
		for (i = 0; i < NR_SOFTIRQS; i++)
			INIT_LIST_HEAD(&per_cpu(softirq_work_list[i], cpu));
	}

	register_hotcpu_notifier(&remote_softirq_cpu_notifier);

	open_softirq(TASKLET_SOFTIRQ, tasklet_action);
	open_softirq(HI_SOFTIRQ, tasklet_hi_action);
}

// ksoftirqd/n		即内核线程运行的函数
static int run_ksoftirqd(void * __bind_cpu)
{
	set_current_state(TASK_INTERRUPTIBLE);

	// 只要do_softirq()函数发现已经执行过的内核线程重新触发了它自己，软中断内核线程就会被唤醒
	while (!kthread_should_stop()) {
		// 一直死循环
		preempt_disable();	// 禁止内核抢占
		if (!local_softirq_pending()) {	// 没有任何软中断触发
			preempt_enable_no_resched();
			schedule();
			preempt_disable();
		}

		__set_current_state(TASK_RUNNING);

		while (local_softirq_pending()) {
			/* Preempt disable stops cpu going offline.
			   If already offline, we'll be on wrong CPU:
			   don't process */
			if (cpu_is_offline((long)__bind_cpu))	// cpu离线
				goto wait_to_die;
			do_softirq();		// 执行软中断
			preempt_enable_no_resched();
			cond_resched();
			preempt_disable();
			rcu_sched_qs((long)__bind_cpu);
		}
		preempt_enable();
		// 处理完所有软中断将自己状态设置为TASK_INTERRUPTIBLE
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;

wait_to_die:
	preempt_enable();
	/* Wait for kthread_stop */
	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * tasklet_kill_immediate is called to remove a tasklet which can already be
 * scheduled for execution on @cpu.
 *
 * Unlike tasklet_kill, this function removes the tasklet
 * _immediately_, even if the tasklet is in TASKLET_STATE_SCHED state.
 *
 * When this function is called, @cpu must be in the CPU_DEAD state.
 */
/*
 * 当调用tasklet_kill_immediate时，用于移除一个可能已经被安排在@cpu上执行的tasklet。
 *
 * 与tasklet_kill不同，此函数_立即_移除tasklet，即使tasklet处于TASKLET_STATE_SCHED状态。
 *
 * 调用此函数时，@cpu必须处于CPU_DEAD状态。
 */
void tasklet_kill_immediate(struct tasklet_struct *t, unsigned int cpu)
{
	struct tasklet_struct **i;

	// 确保指定的CPU已经离线，如果还在线则报错。
	BUG_ON(cpu_online(cpu));
	// 确保tasklet没有在运行，如果在运行则报错。
	BUG_ON(test_bit(TASKLET_STATE_RUN, &t->state));

	// 如果tasklet没有被调度执行，就直接返回。
	if (!test_bit(TASKLET_STATE_SCHED, &t->state))
		return;

	/* CPU is dead, so no lock needed. */
	/* 由于CPU已经死亡，所以不需要锁。 */
	// 遍历指定CPU的tasklet向量，查找并移除指定的tasklet。
	for (i = &per_cpu(tasklet_vec, cpu).head; *i; i = &(*i)->next) {
		if (*i == t) {
			*i = t->next;
			/* If this was the tail element, move the tail ptr */
			if (*i == NULL)
				per_cpu(tasklet_vec, cpu).tail = i;
			return;
		}
	}
	// 如果遍历结束都没有找到指定的tasklet，则报错。
	BUG();
}

static void takeover_tasklets(unsigned int cpu)
{
	/* CPU is dead, so no lock needed. */
	local_irq_disable();

	/* Find end, append list for that CPU. */
	if (&per_cpu(tasklet_vec, cpu).head != per_cpu(tasklet_vec, cpu).tail) {
		*(__get_cpu_var(tasklet_vec).tail) = per_cpu(tasklet_vec, cpu).head;
		__get_cpu_var(tasklet_vec).tail = per_cpu(tasklet_vec, cpu).tail;
		per_cpu(tasklet_vec, cpu).head = NULL;
		per_cpu(tasklet_vec, cpu).tail = &per_cpu(tasklet_vec, cpu).head;
	}
	raise_softirq_irqoff(TASKLET_SOFTIRQ);

	if (&per_cpu(tasklet_hi_vec, cpu).head != per_cpu(tasklet_hi_vec, cpu).tail) {
		*__get_cpu_var(tasklet_hi_vec).tail = per_cpu(tasklet_hi_vec, cpu).head;
		__get_cpu_var(tasklet_hi_vec).tail = per_cpu(tasklet_hi_vec, cpu).tail;
		per_cpu(tasklet_hi_vec, cpu).head = NULL;
		per_cpu(tasklet_hi_vec, cpu).tail = &per_cpu(tasklet_hi_vec, cpu).head;
	}
	raise_softirq_irqoff(HI_SOFTIRQ);

	local_irq_enable();
}
#endif /* CONFIG_HOTPLUG_CPU */

static int __cpuinit cpu_callback(struct notifier_block *nfb,
				  unsigned long action,
				  void *hcpu)
{
	int hotcpu = (unsigned long)hcpu;
	struct task_struct *p;

	switch (action) {
	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
		p = kthread_create(run_ksoftirqd, hcpu, "ksoftirqd/%d", hotcpu);
		if (IS_ERR(p)) {
			printk("ksoftirqd for %i failed\n", hotcpu);
			return NOTIFY_BAD;
		}
		kthread_bind(p, hotcpu);
  		per_cpu(ksoftirqd, hotcpu) = p;
 		break;
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		wake_up_process(per_cpu(ksoftirqd, hotcpu));
		break;
#ifdef CONFIG_HOTPLUG_CPU
	case CPU_UP_CANCELED:
	case CPU_UP_CANCELED_FROZEN:
		if (!per_cpu(ksoftirqd, hotcpu))
			break;
		/* Unbind so it can run.  Fall thru. */
		kthread_bind(per_cpu(ksoftirqd, hotcpu),
			     cpumask_any(cpu_online_mask));
	case CPU_DEAD:
	case CPU_DEAD_FROZEN: {
		struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };

		p = per_cpu(ksoftirqd, hotcpu);
		per_cpu(ksoftirqd, hotcpu) = NULL;
		sched_setscheduler_nocheck(p, SCHED_FIFO, &param);
		kthread_stop(p);
		takeover_tasklets(hotcpu);
		break;
	}
#endif /* CONFIG_HOTPLUG_CPU */
 	}
	return NOTIFY_OK;
}

static struct notifier_block __cpuinitdata cpu_nfb = {
	.notifier_call = cpu_callback
};

static __init int spawn_ksoftirqd(void)
{
	void *cpu = (void *)(long)smp_processor_id();
	int err = cpu_callback(&cpu_nfb, CPU_UP_PREPARE, cpu);

	BUG_ON(err == NOTIFY_BAD);
	cpu_callback(&cpu_nfb, CPU_ONLINE, cpu);
	register_cpu_notifier(&cpu_nfb);
	return 0;
}
early_initcall(spawn_ksoftirqd);

#ifdef CONFIG_SMP
/*
 * Call a function on all processors
 */
int on_each_cpu(void (*func) (void *info), void *info, int wait)
{
	int ret = 0;

	preempt_disable();
	ret = smp_call_function(func, info, wait);
	local_irq_disable();
	func(info);
	local_irq_enable();
	preempt_enable();
	return ret;
}
EXPORT_SYMBOL(on_each_cpu);
#endif

/*
 * [ These __weak aliases are kept in a separate compilation unit, so that
 *   GCC does not inline them incorrectly. ]
 */

int __init __weak early_irq_init(void)
{
	return 0;
}

int __init __weak arch_probe_nr_irqs(void)
{
	return 0;
}

int __init __weak arch_early_irq_init(void)
{
	return 0;
}

int __weak arch_init_chip_data(struct irq_desc *desc, int node)
{
	return 0;
}
