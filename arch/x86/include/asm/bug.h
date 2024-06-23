#ifndef _ASM_X86_BUG_H
#define _ASM_X86_BUG_H

#ifdef CONFIG_BUG
#define HAVE_ARCH_BUG

#ifdef CONFIG_DEBUG_BUGVERBOSE

#ifdef CONFIG_X86_32
# define __BUG_C0	"2:\t.long 1b, %c0\n"
#else
# define __BUG_C0	"2:\t.long 1b - 2b, %c0 - 2b\n"
#endif

/*
 * 如果启用了额外的错误记录（通过预处理器判断），则使用更复杂的错误处理：
 * 1. 把当前文件名和行号记录到错误表中，这有助于调试。
 * 2. 通过将文件名、行号和结构体大小作为立即数传给汇编，
 *    在编译时嵌入这些信息。
 * 3. 使用 .pushsection 和 .popsection 指令将相关信息放入特定的段中，
 *    这通常用于后续的错误分析和调试。
 */

/*
 * 宏定义开始
 * 使用 do...while(0) 结构确保宏的使用像函数调用一样安全
 */
#define BUG()							\
do {								\
	asm volatile("1:\tud2\n"				\
		     ".pushsection __bug_table,\"a\"\n"		\
		     __BUG_C0					\
		     "\t.word %c1, 0\n"				\
		     "\t.org 2b+%c2\n"				\
		     ".popsection"				\
		     : : "i" (__FILE__), "i" (__LINE__),	\
		     "i" (sizeof(struct bug_entry)));		\
	unreachable();						\
} while (0)

#else
/*
 * 使用内嵌汇编代码，具体指令是ud2，这是一个故意的无效指令，
 * 用于生成一个无效的操作码异常，这会导致程序的中断或崩溃。
 */
/*
 * unreachable() 表明此位置代码不可到达，通常用于告诉编译器此路径后不会继续执行，
 * 可以用于优化。
 */
#define BUG()							\
do {								\
	asm volatile("ud2");					\
	unreachable();						\
} while (0)
#endif

#endif /* !CONFIG_BUG */

#include <asm-generic/bug.h>
#endif /* _ASM_X86_BUG_H */
