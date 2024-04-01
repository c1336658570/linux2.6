// 实现原子整数

#ifndef _ASM_X86_ATOMIC_H
#define _ASM_X86_ATOMIC_H

#include <linux/compiler.h>
#include <linux/types.h>		// atomic_t类型的定义
#include <asm/processor.h>
#include <asm/alternative.h>
#include <asm/cmpxchg.h>

/*
 * Atomic operations that C can't guarantee us.  Useful for
 * resource counting etc..
 */
// 初始化原子整数	atomic_t u = ATOMIC_INIT(0);	定义u并初始化为0
#define ATOMIC_INIT(i)	{ (i) }

/**
 * atomic_read - read atomic variable
 * @v: pointer of type atomic_t
 *
 * Atomically reads the value of @v.
 */
// 原子的读取变量
static inline int atomic_read(const atomic_t *v)
{
	return v->counter;
}

/**
 * atomic_set - set atomic variable
 * @v: pointer of type atomic_t
 * @i: required value
 *
 * Atomically sets the value of @v to @i.
 */
// 原子的将v的值设置为i
static inline void atomic_set(atomic_t *v, int i)
{
	v->counter = i;
}

/**
 * atomic_add - add integer to atomic variable
 * @i: integer value to add
 * @v: pointer of type atomic_t
 *
 * Atomically adds @i to @v.
 */
// 原子的给v加i
static inline void atomic_add(int i, atomic_t *v)
{
	asm volatile(LOCK_PREFIX "addl %1,%0"
		     : "+m" (v->counter)
		     : "ir" (i));
}

/**
 * atomic_sub - subtract integer from atomic variable
 * @i: integer value to subtract
 * @v: pointer of type atomic_t
 *
 * Atomically subtracts @i from @v.
 */
// 原子的给v减i
static inline void atomic_sub(int i, atomic_t *v)
{
	asm volatile(LOCK_PREFIX "subl %1,%0"
		     : "+m" (v->counter)
		     : "ir" (i));
}

/**
 * atomic_sub_and_test - subtract value from variable and test result
 * @i: integer value to subtract
 * @v: pointer of type atomic_t
 *
 * Atomically subtracts @i from @v and returns
 * true if the result is zero, or false for all
 * other cases.
 */
// 原子的从v减i，如果结果为0,返回真，否则返回假
static inline int atomic_sub_and_test(int i, atomic_t *v)
{
	unsigned char c;

	asm volatile(LOCK_PREFIX "subl %2,%0; sete %1"
		     : "+m" (v->counter), "=qm" (c)
		     : "ir" (i) : "memory");
	return c;
}

/**
 * atomic_inc - increment atomic variable
 * @v: pointer of type atomic_t
 *
 * Atomically increments @v by 1.
 */
// 原子的给v加1
static inline void atomic_inc(atomic_t *v)
{
	asm volatile(LOCK_PREFIX "incl %0"
		     : "+m" (v->counter));
}

/**
 * atomic_dec - decrement atomic variable
 * @v: pointer of type atomic_t
 *
 * Atomically decrements @v by 1.
 */
// 原子的从v减1
static inline void atomic_dec(atomic_t *v)
{
	asm volatile(LOCK_PREFIX "decl %0"
		     : "+m" (v->counter));
}

/**
 * atomic_dec_and_test - decrement and test
 * @v: pointer of type atomic_t
 *
 * Atomically decrements @v by 1 and
 * returns true if the result is 0, or false for all other
 * cases.
 */
// 原子减和检查
// 原子变量减1,如果结果为0返回真，否则返回假
static inline int atomic_dec_and_test(atomic_t *v)
{
	unsigned char c;

	asm volatile(LOCK_PREFIX "decl %0; sete %1"
		     : "+m" (v->counter), "=qm" (c)
		     : : "memory");
	return c != 0;
}

/**
 * atomic_inc_and_test - increment and test
 * @v: pointer of type atomic_t
 *
 * Atomically increments @v by 1
 * and returns true if the result is zero, or false for all
 * other cases.
 */
// 原子给v加1,如果结果为0返回真，否则返回假
static inline int atomic_inc_and_test(atomic_t *v)
{
	unsigned char c;

	asm volatile(LOCK_PREFIX "incl %0; sete %1"
		     : "+m" (v->counter), "=qm" (c)
		     : : "memory");
	return c != 0;
}

/**
 * atomic_add_negative - add and test if negative
 * @i: integer value to add
 * @v: pointer of type atomic_t
 *
 * Atomically adds @i to @v and returns true
 * if the result is negative, or false when
 * result is greater than or equal to zero.
 */
// 原子给v加i，如果结果是负数，返回真，否则返回假
static inline int atomic_add_negative(int i, atomic_t *v)
{
	unsigned char c;

	asm volatile(LOCK_PREFIX "addl %2,%0; sets %1"
		     : "+m" (v->counter), "=qm" (c)
		     : "ir" (i) : "memory");
	return c;
}

/**
 * atomic_add_return - add integer and return
 * @i: integer value to add
 * @v: pointer of type atomic_t
 *
 * Atomically adds @i to @v and returns @i + @v
 */
// 原子地给v加i并返回结果
static inline int atomic_add_return(int i, atomic_t *v)
{
	int __i;
#ifdef CONFIG_M386
	unsigned long flags;
	if (unlikely(boot_cpu_data.x86 <= 3))
		goto no_xadd;
#endif
	/* Modern 486+ processor */
	__i = i;
	asm volatile(LOCK_PREFIX "xaddl %0, %1"
		     : "+r" (i), "+m" (v->counter)
		     : : "memory");
	return i + __i;

#ifdef CONFIG_M386
no_xadd: /* Legacy 386 processor */
	raw_local_irq_save(flags);
	__i = atomic_read(v);
	atomic_set(v, i + __i);
	raw_local_irq_restore(flags);
	return i + __i;
#endif
}

/**
 * atomic_sub_return - subtract integer and return
 * @v: pointer of type atomic_t
 * @i: integer value to subtract
 *
 * Atomically subtracts @i from @v and returns @v - @i
 */
// 原子地给v减i并返回结果
static inline int atomic_sub_return(int i, atomic_t *v)
{
	return atomic_add_return(-i, v);
}

// 原子地给v加1并返回结果
#define atomic_inc_return(v)  (atomic_add_return(1, v))
// 原子地给v减1并返回结果
#define atomic_dec_return(v)  (atomic_sub_return(1, v))

// 原子比较并交换函数。接受一个指向atomic_t类型变量的指针v，以及旧值old和新值new。
// 函数的目的是将v->counter的值与old进行比较，如果相等，则将其替换为new并返回旧值，
// 否则不进行替换并返回v->counter的当前值。
static inline int atomic_cmpxchg(atomic_t *v, int old, int new)
{
	return cmpxchg(&v->counter, old, new);
}

// 把new赋值给原子变量v，返回原子变量v的旧值。
static inline int atomic_xchg(atomic_t *v, int new)
{
	return xchg(&v->counter, new);
}

/**
 * atomic_add_unless - 除非数字已经等于给定值，否则进行加法操作
 * @v: atomic_t 类型的指针
 * @a: 要添加到 v 的数量...
 * @u: ...除非 v 等于 u。
 * 原子地将 @a 添加到 @v，前提是 @v 不等于 @u。如果 @v 不等于 @u，则返回非零值；否则返回零。
 */
/*
 * 用于原子地将给定值`a`添加到指针`v`所指向的`atomic_t`类型变量中，但前提是该变量的值不等于给定值`u`。
 * 如果添加成功，则返回非零值，表示变量的值不等于`u`；如果添加失败，则返回零值，表示变量的值已经等于`u`。
 */
static inline int atomic_add_unless(atomic_t *v, int a, int u)
{
	int c, old;
	c = atomic_read(v);
	for (;;) {
		if (unlikely(c == (u)))		// 如果当前值等于给定值u
			break;
		old = atomic_cmpxchg((v), c, c + (a));		// 原子比较并交换操作
		if (likely(old == c))	// 如果交换成功
			break;
		c = old;
	}
	return c != (u);		// 如果当前值不等于给定值u，返回非零；否则返回零
}

// 用于将给定指针`v`所指向的`atomic_t`类型变量的值加1，前提是该变量的值不等于0。
#define atomic_inc_not_zero(v) atomic_add_unless((v), 1, 0)

/**
 * atomic_inc_short - 对短整型进行递增
 * @v: int 类型的指针
 * 原子地将 1 添加到 @v
 * 返回 @v 的新值
 */
// 用于原子地将指针`v`所指向的`short int`类型变量的值加1。
static inline short int atomic_inc_short(short int *v)
{
	asm(LOCK_PREFIX "addw $1, %0" : "+m" (*v));		// 使用汇编指令实现原子加法操作
	return *v;		// 返回新的值
}

#ifdef CONFIG_X86_64
/**
 * atomic_or_long - 两个长整型数的按位或操作
 * @v1: 指向无符号长整型的指针
 * @v2: 无符号长整型数
 *
 * 原子地对@v1和@v2进行按位或操作
 * 返回按位或的结果
 */
static inline void atomic_or_long(unsigned long *v1, unsigned long v2)
{
	asm(LOCK_PREFIX "orq %1, %0" : "+m" (*v1) : "r" (v2));
}
#endif

/* These are x86-specific, used by some header files */
/* 这些是特定于x86的，在一些头文件中使用 */

/*
 * atomic_clear_mask - 清除地址中指定掩码的位
 * @mask: 指定要清除的位的掩码
 * @addr: 指向值的地址
 *
 * 原子地从地址@addr的值中清除由@mask指定的位。
 */
#define atomic_clear_mask(mask, addr)				\
	asm volatile(LOCK_PREFIX "andl %0,%1"			\
		     : : "r" (~(mask)), "m" (*(addr)) : "memory")

/*
 * atomic_set_mask - 设置地址中指定掩码的位
 * @mask: 指定要设置的位的掩码
 * @addr: 指向值的地址
 *
 * 原子地在地址@addr的值中设置由@mask指定的位。
 */
#define atomic_set_mask(mask, addr)				\
	asm volatile(LOCK_PREFIX "orl %0,%1"			\
		     : : "r" ((unsigned)(mask)), "m" (*(addr))	\
		     : "memory")

/* Atomic operations are already serializing on x86 */
/* 在x86上，原子操作已经具有序列化效果 */

/*
 * smp_mb__before_atomic_dec - 原子递减之前的内存屏障
 *
 * 在原子递减操作之前插入一个内存屏障。
 */
#define smp_mb__before_atomic_dec()	barrier()
/*
 * smp_mb__after_atomic_dec - 原子递减之后的内存屏障
 *
 * 在原子递减操作之后插入一个内存屏障。
 */
#define smp_mb__after_atomic_dec()	barrier()
/*
 * smp_mb__before_atomic_inc - 原子递增之前的内存屏障
 *
 * 在原子递增操作之前插入一个内存屏障。
 */
#define smp_mb__before_atomic_inc()	barrier()
/*
 * smp_mb__after_atomic_inc - 原子递增之后的内存屏障
 *
 * 在原子递增操作之后插入一个内存屏障。
 */
#define smp_mb__after_atomic_inc()	barrier()

#ifdef CONFIG_X86_32
# include "atomic64_32.h"
#else
# include "atomic64_64.h"
#endif

#include <asm-generic/atomic-long.h>
#endif /* _ASM_X86_ATOMIC_H */
