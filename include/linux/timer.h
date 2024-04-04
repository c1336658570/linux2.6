#ifndef _LINUX_TIMER_H
#define _LINUX_TIMER_H

#include <linux/list.h>
#include <linux/ktime.h>
#include <linux/stddef.h>
#include <linux/debugobjects.h>
#include <linux/stringify.h>

struct tvec_base;

struct timer_list {
	// 定时器链表节点，用于将定时器连接到定时器列表中。通过链表节点，可以将多个定时器组织成一个列表，以便进行管理和遍历。
	struct list_head entry;
	unsigned long expires;		// 定时器的到期时间（绝对时间），以jiffies为单位的定时值。

	void (*function)(unsigned long);		// 定时器到期时要执行的回调函数
	unsigned long data;									// 回调函数的参数，data参数使我们可以利用同一个处理函数注册多个定时器，只要data参数不同就可以

	struct tvec_base *base;							// 定时器内部值，用户不要使用
#ifdef CONFIG_TIMER_STATS
	void *start_site;										// 定时器启动位置的指针
	char start_comm[16];             		// 定时器启动时的进程名
	int start_pid;                   		// 定时器启动时的进程ID
#endif
#ifdef CONFIG_LOCKDEP
	// 这是锁依赖映射，用于锁依赖检测。在编译时启用了CONFIG_LOCKDEP选项时，定时器会使用这个映射来跟踪相关的锁依赖关系，以检测潜在的死锁和锁相关问题。
	struct lockdep_map lockdep_map;			// 锁依赖映射，用于锁依赖检测
#endif
};

extern struct tvec_base boot_tvec_bases;

#ifdef CONFIG_LOCKDEP
/*
 * NB: because we have to copy the lockdep_map, setting the lockdep_map key
 * (second argument) here is required, otherwise it could be initialised to
 * the copy of the lockdep_map later! We use the pointer to and the string
 * "<file>:<line>" as the key resp. the name of the lockdep_map.
 */
#define __TIMER_LOCKDEP_MAP_INITIALIZER(_kn)				\
	.lockdep_map = STATIC_LOCKDEP_MAP_INIT(_kn, &_kn),
#else
#define __TIMER_LOCKDEP_MAP_INITIALIZER(_kn)
#endif

#define TIMER_INITIALIZER(_function, _expires, _data) {		\
		.entry = { .prev = TIMER_ENTRY_STATIC },	\
		.function = (_function),			\
		.expires = (_expires),				\
		.data = (_data),				\
		.base = &boot_tvec_bases,			\
		__TIMER_LOCKDEP_MAP_INITIALIZER(		\
			__FILE__ ":" __stringify(__LINE__))	\
	}

#define DEFINE_TIMER(_name, _function, _expires, _data)		\
	struct timer_list _name =				\
		TIMER_INITIALIZER(_function, _expires, _data)

void init_timer_key(struct timer_list *timer,
		    const char *name,
		    struct lock_class_key *key);
void init_timer_deferrable_key(struct timer_list *timer,
			       const char *name,
			       struct lock_class_key *key);

#ifdef CONFIG_LOCKDEP
// 初始化定义的定时器
#define init_timer(timer)						\
	do {								\
		static struct lock_class_key __key;			\
		init_timer_key((timer), #timer, &__key);		\
	} while (0)

#define init_timer_deferrable(timer)					\
	do {								\
		static struct lock_class_key __key;			\
		init_timer_deferrable_key((timer), #timer, &__key);	\
	} while (0)

#define init_timer_on_stack(timer)					\
	do {								\
		static struct lock_class_key __key;			\
		init_timer_on_stack_key((timer), #timer, &__key);	\
	} while (0)

#define setup_timer(timer, fn, data)					\
	do {								\
		static struct lock_class_key __key;			\
		setup_timer_key((timer), #timer, &__key, (fn), (data));\
	} while (0)

#define setup_timer_on_stack(timer, fn, data)				\
	do {								\
		static struct lock_class_key __key;			\
		setup_timer_on_stack_key((timer), #timer, &__key,	\
					 (fn), (data));			\
	} while (0)
#else
// 初始化定义的定时器
#define init_timer(timer)\
	init_timer_key((timer), NULL, NULL)
#define init_timer_deferrable(timer)\
	init_timer_deferrable_key((timer), NULL, NULL)
#define init_timer_on_stack(timer)\
	init_timer_on_stack_key((timer), NULL, NULL)
#define setup_timer(timer, fn, data)\
	setup_timer_key((timer), NULL, NULL, (fn), (data))
#define setup_timer_on_stack(timer, fn, data)\
	setup_timer_on_stack_key((timer), NULL, NULL, (fn), (data))
#endif

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS
extern void init_timer_on_stack_key(struct timer_list *timer,
				    const char *name,
				    struct lock_class_key *key);
extern void destroy_timer_on_stack(struct timer_list *timer);
#else
static inline void destroy_timer_on_stack(struct timer_list *timer) { }
static inline void init_timer_on_stack_key(struct timer_list *timer,
					   const char *name,
					   struct lock_class_key *key)
{
	init_timer_key(timer, name, key);
}
#endif

static inline void setup_timer_key(struct timer_list * timer,
				const char *name,
				struct lock_class_key *key,
				void (*function)(unsigned long),
				unsigned long data)
{
	timer->function = function;
	timer->data = data;
	init_timer_key(timer, name, key);
}

static inline void setup_timer_on_stack_key(struct timer_list *timer,
					const char *name,
					struct lock_class_key *key,
					void (*function)(unsigned long),
					unsigned long data)
{
	timer->function = function;
	timer->data = data;
	init_timer_on_stack_key(timer, name, key);
}

/**
 * timer_pending - is a timer pending?
 * @timer: the timer in question
 *
 * timer_pending will tell whether a given timer is currently pending,
 * or not. Callers must ensure serialization wrt. other operations done
 * to this timer, eg. interrupt contexts, or other CPUs on SMP.
 *
 * return value: 1 if the timer is pending, 0 if not.
 */
static inline int timer_pending(const struct timer_list * timer)
{
	return timer->entry.next != NULL;
}

extern void add_timer_on(struct timer_list *timer, int cpu);
// 该函数实现在定时器超时前停止定时器
// 被激活或未被激活的定时器都可以使用，如果定时器未被激活，则该函数返回0,否则返回1。不需要为已经超时的定时器调用该函数，因为他们会自动删除。
// 该函数只能保证定时器不会再被激活（将来不会执行），如果有多个处理器，该定时器可能已经在其他处理器上运行了，
// 如果删除定时器同时需要等待可能在其他处理器上运行的定时器处理程序都退出，则可以使用del_timer_sync()来执行删除操作。
extern int del_timer(struct timer_list * timer);
// 更改已激活的定时器超时时间，也可以操作那些已经初始化但没激活的定时器，如果定时器没激活，那么mod_timer()会激活它。
// 如果定时器未被激活，该函数返回0，否则返回1。
extern int mod_timer(struct timer_list *timer, unsigned long expires);
extern int mod_timer_pending(struct timer_list *timer, unsigned long expires);
extern int mod_timer_pinned(struct timer_list *timer, unsigned long expires);

#define TIMER_NOT_PINNED	0
#define TIMER_PINNED		1
/*
 * The jiffies value which is added to now, when there is no timer
 * in the timer wheel:
 */
#define NEXT_TIMER_MAX_DELTA	((1UL << 30) - 1)

/*
 * Return when the next timer-wheel timeout occurs (in absolute jiffies),
 * locks the timer base and does the comparison against the given
 * jiffie.
 */
extern unsigned long get_next_timer_interrupt(unsigned long now);

/*
 * Timer-statistics info:
 */
#ifdef CONFIG_TIMER_STATS

extern int timer_stats_active;

#define TIMER_STATS_FLAG_DEFERRABLE	0x1

extern void init_timer_stats(void);

extern void timer_stats_update_stats(void *timer, pid_t pid, void *startf,
				     void *timerf, char *comm,
				     unsigned int timer_flag);

extern void __timer_stats_timer_set_start_info(struct timer_list *timer,
					       void *addr);

static inline void timer_stats_timer_set_start_info(struct timer_list *timer)
{
	if (likely(!timer_stats_active))
		return;
	__timer_stats_timer_set_start_info(timer, __builtin_return_address(0));
}

static inline void timer_stats_timer_clear_start_info(struct timer_list *timer)
{
	timer->start_site = NULL;
}
#else
static inline void init_timer_stats(void)
{
}

static inline void timer_stats_timer_set_start_info(struct timer_list *timer)
{
}

static inline void timer_stats_timer_clear_start_info(struct timer_list *timer)
{
}
#endif

// 激活定时器
extern void add_timer(struct timer_list *timer);

#ifdef CONFIG_SMP
  extern int try_to_del_timer_sync(struct timer_list *timer);
	// 如果有多个处理器，删除该定时器时该定时器可能已经在其他处理器上运行了，该函数删除定时器同时需要等待可能在其他处理器上运行的定时器处理程序都退出
	// 该函数不能在中断上下文中使用
	extern int del_timer_sync(struct timer_list *timer);
#else
# define try_to_del_timer_sync(t)	del_timer(t)
# define del_timer_sync(t)		del_timer(t)
#endif

#define del_singleshot_timer_sync(t) del_timer_sync(t)

extern void init_timers(void);
extern void run_local_timers(void);
struct hrtimer;
extern enum hrtimer_restart it_real_fn(struct hrtimer *);

unsigned long __round_jiffies(unsigned long j, int cpu);
unsigned long __round_jiffies_relative(unsigned long j, int cpu);
unsigned long round_jiffies(unsigned long j);
unsigned long round_jiffies_relative(unsigned long j);

unsigned long __round_jiffies_up(unsigned long j, int cpu);
unsigned long __round_jiffies_up_relative(unsigned long j, int cpu);
unsigned long round_jiffies_up(unsigned long j);
unsigned long round_jiffies_up_relative(unsigned long j);

#endif
