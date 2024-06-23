#ifndef _ASM_GENERIC_BUG_H
#define _ASM_GENERIC_BUG_H

#include <linux/compiler.h>

#ifdef CONFIG_BUG

#ifdef CONFIG_GENERIC_BUG
#ifndef __ASSEMBLY__
struct bug_entry {
#ifndef CONFIG_GENERIC_BUG_RELATIVE_POINTERS
	unsigned long	bug_addr;
#else
	signed int	bug_addr_disp;
#endif
#ifdef CONFIG_DEBUG_BUGVERBOSE
#ifndef CONFIG_GENERIC_BUG_RELATIVE_POINTERS
	const char	*file;
#else
	signed int	file_disp;
#endif
	unsigned short	line;
#endif
	unsigned short	flags;
};
#endif		/* __ASSEMBLY__ */

#define BUGFLAG_WARNING	(1<<0)
#endif	/* CONFIG_GENERIC_BUG */

/*
 * Don't use BUG() or BUG_ON() unless there's really no way out; one
 * example might be detecting data structure corruption in the middle
 * of an operation that can't be backed out of.  If the (sub)system
 * can somehow continue operating, perhaps with reduced functionality,
 * it's probably not BUG-worthy.
 *
 * If you're tempted to BUG(), think again:  is completely giving up
 * really the *only* solution?  There are usually better options, where
 * users don't need to reboot ASAP and can mostly shut down cleanly.
 */
/*
 * 不要使用 BUG() 或 BUG_ON() 除非真的没有其他出路；
 * 一个例子可能是在无法回退的操作中检测到数据结构损坏。
 * 如果子系统能够以某种方式继续运行，哪怕功能减少，
 * 那么它可能不值得使用 BUG()。
 *
 * 如果你想使用 BUG()，再次思考：完全放弃真的是*唯一*的解决方案吗？
 * 通常有更好的选择，用户不需要立即重启，并且可以大致上干净地关闭。
 */
// 如果没有定义 HAVE_ARCH_BUG，则定义 BUG() 宏
#ifndef HAVE_ARCH_BUG
#define BUG() do { \
	/* 在控制台打印出错信息，包括文件名、行号和函数名 */ \
	printk("BUG: failure at %s:%d/%s()!\n", __FILE__, __LINE__, __func__); \
	/* 调用 panic() 函数，产生内核崩溃，停止系统运行 */ \
	panic("BUG!"); \
} while (0)
#endif

#ifndef HAVE_ARCH_BUG_ON
// 如果没有定义 HAVE_ARCH_BUG_ON，则定义 BUG_ON() 宏
#define BUG_ON(condition) do { if (unlikely(condition)) BUG(); } while(0)
#endif

/*
 * WARN(), WARN_ON(), WARN_ON_ONCE, and so on can be used to report
 * significant issues that need prompt attention if they should ever
 * appear at runtime.  Use the versions with printk format strings
 * to provide better diagnostics.
 */
#ifndef __WARN
#ifndef __ASSEMBLY__
extern void warn_slowpath_fmt(const char *file, const int line,
		const char *fmt, ...) __attribute__((format(printf, 3, 4)));
extern void warn_slowpath_null(const char *file, const int line);
#define WANT_WARN_ON_SLOWPATH
#endif
#define __WARN()		warn_slowpath_null(__FILE__, __LINE__)
#define __WARN_printf(arg...)	warn_slowpath_fmt(__FILE__, __LINE__, arg)
#else
#define __WARN_printf(arg...)	do { printk(arg); __WARN(); } while (0)
#endif

#ifndef WARN_ON
#define WARN_ON(condition) ({						\
	int __ret_warn_on = !!(condition);				\
	if (unlikely(__ret_warn_on))					\
		__WARN();						\
	unlikely(__ret_warn_on);					\
})
#endif

#ifndef WARN
#define WARN(condition, format...) ({						\
	int __ret_warn_on = !!(condition);				\
	if (unlikely(__ret_warn_on))					\
		__WARN_printf(format);					\
	unlikely(__ret_warn_on);					\
})
#endif

#else /* !CONFIG_BUG */
#ifndef HAVE_ARCH_BUG
#define BUG() do {} while(0)
#endif

#ifndef HAVE_ARCH_BUG_ON
#define BUG_ON(condition) do { if (condition) ; } while(0)
#endif

#ifndef HAVE_ARCH_WARN_ON
#define WARN_ON(condition) ({						\
	int __ret_warn_on = !!(condition);				\
	unlikely(__ret_warn_on);					\
})
#endif

#ifndef WARN
#define WARN(condition, format...) ({					\
	int __ret_warn_on = !!(condition);				\
	unlikely(__ret_warn_on);					\
})
#endif

#endif

#define WARN_ON_ONCE(condition)	({				\
	static bool __warned;					\
	int __ret_warn_once = !!(condition);			\
								\
	if (unlikely(__ret_warn_once))				\
		if (WARN_ON(!__warned)) 			\
			__warned = true;			\
	unlikely(__ret_warn_once);				\
})

#define WARN_ONCE(condition, format...)	({			\
	static bool __warned;					\
	int __ret_warn_once = !!(condition);			\
								\
	if (unlikely(__ret_warn_once))				\
		if (WARN(!__warned, format)) 			\
			__warned = true;			\
	unlikely(__ret_warn_once);				\
})

#define WARN_ON_RATELIMIT(condition, state)			\
		WARN_ON((condition) && __ratelimit(state))

#ifdef CONFIG_SMP
# define WARN_ON_SMP(x)			WARN_ON(x)
#else
# define WARN_ON_SMP(x)			do { } while (0)
#endif

#endif
