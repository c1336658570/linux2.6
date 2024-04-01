// 一组对位进行操作的函数
#ifndef _ASM_X86_BITOPS_H
#define _ASM_X86_BITOPS_H

/*
 * Copyright 1992, Linus Torvalds.
 *
 * Note: inlines with more than a single statement should be marked
 * __always_inline to avoid problems with older gcc's inlining heuristics.
 */

#ifndef _LINUX_BITOPS_H
#error only <linux/bitops.h> can be included directly
#endif

#include <linux/compiler.h>
#include <asm/alternative.h>

/*
 * 这些操作必须使用内联汇编来实现，以确保位设置操作是原子的。
 * 所有位操作返回值为0表示在操作之前该位被清除，返回值不为0表示该位在操作之前没有被清除。
 *
 * 位0是addr的最低有效位（LSB）；位32是(addr+1)的最低有效位（LSB）。
 */

#if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 1)
/* Technically wrong, but this avoids compilation errors on some gcc
   versions. */
/* 在某些gcc版本上编译时会出错，但在技术上是不正确的。 */
// 定义了用于内联汇编的地址操作数。根据GCC版本的不同，它采用不同的约束修饰符。对于GCC版本低于4.1，使用"=m"修饰符，表示输出内存操作数；对于GCC版本大于等于4.1，使用"+m"修饰符，表示输入输出内存操作数。
#define BITOP_ADDR(x) "=m" (*(volatile long *) (x))
#else
#define BITOP_ADDR(x) "+m" (*(volatile long *) (x))
#endif

#define ADDR				BITOP_ADDR(addr)

/*
 * We do the locked ops that don't return the old value as
 * a mask operation on a byte.
 */
/*
 * 我们将不返回旧值的锁定操作视为对字节进行掩码操作。
 */
// IS_IMMEDIATE 宏用于检查传入的参数是否是常量。通过使用GCC内置函数 __builtin_constant_p，可以在编译时优化常量操作。
#define IS_IMMEDIATE(nr)		(__builtin_constant_p(nr))
// 定义了一个掩码操作数的地址。它根据给定的位数 nr 和地址 addr 计算出正确的内存地址。
#define CONST_MASK_ADDR(nr, addr)	BITOP_ADDR((void *)(addr) + ((nr)>>3))
// 用于计算位掩码。给定位数 nr，它通过将1左移位 nr 对8取余数的结果来生成位掩码。
#define CONST_MASK(nr)			(1 << ((nr) & 7))

/**
 * set_bit - Atomically set a bit in memory
 * @nr: the bit to set
 * @addr: the address to start counting from
 *
 * This function is atomic and may not be reordered.  See __set_bit()
 * if you do not require the atomic guarantees.
 *
 * Note: there are no guarantees that this function will not be reordered
 * on non x86 architectures, so if you are writing portable code,
 * make sure not to rely on its reordering guarantees.
 *
 * Note that @nr may be almost arbitrarily large; this function is not
 * restricted to acting on a single-word quantity.
 */
// 原子地设置addr所指对象的第nr位
static __always_inline void
set_bit(unsigned int nr, volatile unsigned long *addr)
{
	// BTS（位测试并置位）
	// 写法：BTS REG16/MEM16,REG16/IMM8;或BTS REG32/MEM32,REG32/IMM8;
	// 作用：CF=DEST的第index位，dest的第index位=1；
	if (IS_IMMEDIATE(nr)) {
		asm volatile(LOCK_PREFIX "orb %1,%0"
			: CONST_MASK_ADDR(nr, addr)
			: "iq" ((u8)CONST_MASK(nr))
			: "memory");
	} else {
		asm volatile(LOCK_PREFIX "bts %1,%0"
			: BITOP_ADDR(addr) : "Ir" (nr) : "memory");
	}
}

/**
 * __set_bit - Set a bit in memory
 * @nr: the bit to set
 * @addr: the address to start counting from
 *
 * Unlike set_bit(), this function is non-atomic and may be reordered.
 * If it's called on the same region of memory simultaneously, the effect
 * may be that only one operation succeeds.
 */
// 同样功能的非原子操作
static inline void __set_bit(int nr, volatile unsigned long *addr)
{
	asm volatile("bts %1,%0" : ADDR : "Ir" (nr) : "memory");
}

/**
 * clear_bit - Clears a bit in memory
 * @nr: Bit to clear
 * @addr: Address to start counting from
 *
 * clear_bit() is atomic and may not be reordered.  However, it does
 * not contain a memory barrier, so if it is used for locking purposes,
 * you should call smp_mb__before_clear_bit() and/or smp_mb__after_clear_bit()
 * in order to ensure changes are visible on other processors.
 */
// 原子地清空addr所指对象的第nr位
static __always_inline void
clear_bit(int nr, volatile unsigned long *addr)
{
	// BTR(位测试并复位)
	// 写法：BTR REG16/MEM16,REG16/IMM8;或BTR REG32/MEM32,REG32/IMM8;
	// 作用：CF=DEST的第index位，dest的第index位=0；
	if (IS_IMMEDIATE(nr)) {
		asm volatile(LOCK_PREFIX "andb %1,%0"
			: CONST_MASK_ADDR(nr, addr)
			: "iq" ((u8)~CONST_MASK(nr)));
	} else {
		asm volatile(LOCK_PREFIX "btr %1,%0"
			: BITOP_ADDR(addr)
			: "Ir" (nr));
	}
}

/*
 * clear_bit_unlock - Clears a bit in memory
 * @nr: Bit to clear
 * @addr: Address to start counting from
 *
 * clear_bit() is atomic and implies release semantics before the memory
 * operation. It can be used for an unlock.
 */
static inline void clear_bit_unlock(unsigned nr, volatile unsigned long *addr)
{
	barrier();
	clear_bit(nr, addr);
}

// 同样功能的非原子操作
static inline void __clear_bit(int nr, volatile unsigned long *addr)
{
	asm volatile("btr %1,%0" : ADDR : "Ir" (nr));
}

/*
 * __clear_bit_unlock - Clears a bit in memory
 * @nr: Bit to clear
 * @addr: Address to start counting from
 *
 * __clear_bit() is non-atomic and implies release semantics before the memory
 * operation. It can be used for an unlock if no other CPUs can concurrently
 * modify other bits in the word.
 *
 * No memory barrier is required here, because x86 cannot reorder stores past
 * older loads. Same principle as spin_unlock.
 */
static inline void __clear_bit_unlock(unsigned nr, volatile unsigned long *addr)
{
	barrier();
	__clear_bit(nr, addr);
}

#define smp_mb__before_clear_bit()	barrier()
#define smp_mb__after_clear_bit()	barrier()

/**
 * __change_bit - Toggle a bit in memory
 * @nr: the bit to change
 * @addr: the address to start counting from
 *
 * Unlike change_bit(), this function is non-atomic and may be reordered.
 * If it's called on the same region of memory simultaneously, the effect
 * may be that only one operation succeeds.
 */
// 同样功能的非原子操作
static inline void __change_bit(int nr, volatile unsigned long *addr)
{
	// BTC(位测试并复位)	
	// 写法：BTC REG16/MEM16,REG16/IMM8;或BTC REG32/MEM32,REG32/IMM8;
	// 作用：CF=DEST的第index位，dest的第index位取反；
	asm volatile("btc %1,%0" : ADDR : "Ir" (nr));
}

/**
 * change_bit - Toggle a bit in memory
 * @nr: Bit to change
 * @addr: Address to start counting from
 *
 * change_bit() is atomic and may not be reordered.
 * Note that @nr may be almost arbitrarily large; this function is not
 * restricted to acting on a single-word quantity.
 */
// 原子地翻转addr所指对象的第nr位
static inline void change_bit(int nr, volatile unsigned long *addr)
{
	// BTC(位测试并复位)	
	// 写法：BTC REG16/MEM16,REG16/IMM8;或BTC REG32/MEM32,REG32/IMM8;
	// 作用：CF=DEST的第index位，dest的第index位取反；
	if (IS_IMMEDIATE(nr)) {
		asm volatile(LOCK_PREFIX "xorb %1,%0"
			: CONST_MASK_ADDR(nr, addr)
			: "iq" ((u8)CONST_MASK(nr)));
	} else {
		asm volatile(LOCK_PREFIX "btc %1,%0"
			: BITOP_ADDR(addr)
			: "Ir" (nr));
	}
}

/**
 * test_and_set_bit - Set a bit and return its old value
 * @nr: Bit to set
 * @addr: Address to count from
 *
 * This operation is atomic and cannot be reordered.
 * It also implies a memory barrier.
 */
// 原子地设置addr所指对象的第nr位，并返回原先的值
static inline int test_and_set_bit(int nr, volatile unsigned long *addr)
{
	int oldbit;

	// BTS（位测试并置位）
	// 写法：BTS REG16/MEM16,REG16/IMM8;或BTS REG32/MEM32,REG32/IMM8;
	// 作用：CF=DEST的第index位，dest的第index位=1；
	// SBB（带借位减法）
	// 写法：SBB reg/mem， reg/mem/imm
	// 作用：dest=dest-src-cf；
	asm volatile(LOCK_PREFIX "bts %2,%1\n\t"
		     "sbb %0,%0" : "=r" (oldbit), ADDR : "Ir" (nr) : "memory");

	return oldbit;
}

/**
 * test_and_set_bit_lock - Set a bit and return its old value for lock
 * @nr: Bit to set
 * @addr: Address to count from
 *
 * This is the same as test_and_set_bit on x86.
 */
static __always_inline int
test_and_set_bit_lock(int nr, volatile unsigned long *addr)
{
	return test_and_set_bit(nr, addr);
}

/**
 * __test_and_set_bit - Set a bit and return its old value
 * @nr: Bit to set
 * @addr: Address to count from
 *
 * This operation is non-atomic and can be reordered.
 * If two examples of this operation race, one can appear to succeed
 * but actually fail.  You must protect multiple accesses with a lock.
 */
static inline int __test_and_set_bit(int nr, volatile unsigned long *addr)
{
	int oldbit;

	asm("bts %2,%1\n\t"
	    "sbb %0,%0"
	    : "=r" (oldbit), ADDR
	    : "Ir" (nr));
	return oldbit;
}

/**
 * test_and_clear_bit - Clear a bit and return its old value
 * @nr: Bit to clear
 * @addr: Address to count from
 *
 * This operation is atomic and cannot be reordered.
 * It also implies a memory barrier.
 */
// 原子地清空addr所指对象的第nr位，并返回原先的值
static inline int test_and_clear_bit(int nr, volatile unsigned long *addr)
{
	int oldbit;

	// BTR(位测试并复位)
	// 写法：BTR REG16/MEM16,REG16/IMM8;或BTR REG32/MEM32,REG32/IMM8;
	// 作用：CF=DEST的第index位，dest的第index位=0；
	// SBB（带借位减法）
	// 写法：SBB reg/mem， reg/mem/imm
	// 作用：dest=dest-src-cf；
	asm volatile(LOCK_PREFIX "btr %2,%1\n\t"
		     "sbb %0,%0"
		     : "=r" (oldbit), ADDR : "Ir" (nr) : "memory");

	return oldbit;
}

/**
 * __test_and_clear_bit - Clear a bit and return its old value
 * @nr: Bit to clear
 * @addr: Address to count from
 *
 * This operation is non-atomic and can be reordered.
 * If two examples of this operation race, one can appear to succeed
 * but actually fail.  You must protect multiple accesses with a lock.
 */
static inline int __test_and_clear_bit(int nr, volatile unsigned long *addr)
{
	int oldbit;

	asm volatile("btr %2,%1\n\t"
		     "sbb %0,%0"
		     : "=r" (oldbit), ADDR
		     : "Ir" (nr));
	return oldbit;
}

/* WARNING: non atomic and it can be reordered! */
static inline int __test_and_change_bit(int nr, volatile unsigned long *addr)
{
	int oldbit;

	// BTC(位测试并复位)	
	// 写法：BTC REG16/MEM16,REG16/IMM8;或BTC REG32/MEM32,REG32/IMM8;
	// 作用：CF=DEST的第index位，dest的第index位取反；
	// SBB（带借位减法）
	// 写法：SBB reg/mem， reg/mem/imm
	// 作用：dest=dest-src-cf；
	asm volatile("btc %2,%1\n\t"
		     "sbb %0,%0"
		     : "=r" (oldbit), ADDR
		     : "Ir" (nr) : "memory");

	return oldbit;
}

/**
 * test_and_change_bit - Change a bit and return its old value
 * @nr: Bit to change
 * @addr: Address to count from
 *
 * This operation is atomic and cannot be reordered.
 * It also implies a memory barrier.
 */
// 原子地翻转addr所指对象的第nr位，并返回原先的值
static inline int test_and_change_bit(int nr, volatile unsigned long *addr)
{
	int oldbit;

	// BTC(位测试并复位)	
	// 写法：BTC REG16/MEM16,REG16/IMM8;或BTC REG32/MEM32,REG32/IMM8;
	// 作用：CF=DEST的第index位，dest的第index位取反；
	// SBB（带借位减法）
	// 写法：SBB reg/mem， reg/mem/imm
	// 作用：dest=dest-src-cf；
	asm volatile(LOCK_PREFIX "btc %2,%1\n\t"
		     "sbb %0,%0"
		     : "=r" (oldbit), ADDR : "Ir" (nr) : "memory");

	return oldbit;
}

/**
 * constant_test_bit - 判断位是否被设置（常量版本）
 * @nr: 要测试的位号
 * @addr: 开始计数的地址
 *
 * 该函数用于判断给定地址 @addr 开始处的位号 @nr 是否被设置为1。
 * 该函数适用于位号在编译时已知的情况。
 *
 * 返回值:
 *   如果位被设置，则返回非零值；否则返回0。
 */
static __always_inline int constant_test_bit(unsigned int nr, const volatile unsigned long *addr)
{
	// 计算位号在一个长整型数中的偏移量，并使用位运算判断位是否被设置
	return ((1UL << (nr % BITS_PER_LONG)) &
		(((unsigned long *)addr)[nr / BITS_PER_LONG])) != 0;
}

/**
 * variable_test_bit - 判断位是否被设置（变量版本）
 * @nr: 要测试的位号
 * @addr: 开始计数的地址
 *
 * 该函数用于判断给定地址 @addr 开始处的位号 @nr 是否被设置为1。
 * 该函数适用于位号在运行时才确定的情况。
 *
 * 返回值:
 *   如果位被设置，则返回非零值；否则返回0。
 */
static inline int variable_test_bit(int nr, volatile const unsigned long *addr)
{
	int oldbit;

	// 使用汇编内联代码执行位测试操作
	// BT（位测试）
	// 写法：BT REG16/MEM16,REG16/IMM8;或BT REG32/MEM32,REG32/IMM8;
	// 作用：CF=DEST的第index位，dest不变。
	// SBB（带借位减法）
	// 写法：SBB reg/mem， reg/mem/imm
	// 作用：dest=dest-src-cf；
	asm volatile("bt %2,%1\n\t"
		     "sbb %0,%0"
		     : "=r" (oldbit)
		     : "m" (*(unsigned long *)addr), "Ir" (nr));

	return oldbit;
}

#if 0 /* Fool kernel-doc since it doesn't do macros yet */
/**
 * test_bit - 判断位是否被设置
 * @nr: 要测试的位号
 * @addr: 开始计数的地址
 *
 * 返回值:
 *   如果位被设置，则返回非零值；否则返回0。
 */
static int test_bit(int nr, const volatile unsigned long *addr);
#endif

// 原子地返回addr所指对象的第nr位
#define test_bit(nr, addr)			\
	(__builtin_constant_p((nr))		\
	 ? constant_test_bit((nr), (addr))	\
	 : variable_test_bit((nr), (addr)))

/**
 * __ffs - 在字中查找第一个置位的位
 * @word: 要搜索的字
 *
 * 如果不存在置位的位，则结果未定义，因此代码应先检查是否为0。
 */
// 返回值是第一个被设置的位的位号，只搜索一个字
static inline unsigned long __ffs(unsigned long word)
{
	// BSF(前向位扫描)
	// 写法：BSF reg16/reg32， reg16/reg32/mem16/mem32；（类型须匹配）
	// 作用：dest=src中值为1的最低位编号（从低位向高位搜索）
	asm("bsf %1,%0"
		: "=r" (word)
		: "rm" (word));
	return word;
}

/**
 * ffz - 在字中查找第一个零位
 * @word: 要搜索的字
 *
 * 如果不存在零位，则结果未定义，因此代码应先检查是否为 ~0UL。
 */
// 返回值是第一个未被设置的位的位号，只搜索一个字
static inline unsigned long ffz(unsigned long word)
{
	// BSF(前向位扫描)
	// 写法：BSF reg16/reg32， reg16/reg32/mem16/mem32；（类型须匹配）
	// 作用：dest=src中值为1的最低位编号（从低位向高位搜索）
	asm("bsf %1,%0"
		: "=r" (word)
		: "r" (~word));
	return word;
}

/*
 * __fls - 在字中查找最后一个置位的位
 * @word: 要搜索的字
 *
 * 如果不存在置位的位，则结果未定义，因此代码应先检查是否为0。
 */
static inline unsigned long __fls(unsigned long word)
{
	// BSR(后向位扫描)
	// 写法：BSR reg16/reg32， reg16/reg32/mem16/mem32；（类型须匹配）
	// 作用：dest=src中值为1的最高位编号（从高位向低位搜索）
	asm("bsr %1,%0"
	    : "=r" (word)
	    : "rm" (word));
	return word;
}

#ifdef __KERNEL__
/**
 * ffs - find first set bit in word
 * @x: the word to search
 *
 * This is defined the same way as the libc and compiler builtin ffs
 * routines, therefore differs in spirit from the other bitops.
 *
 * ffs(value) returns 0 if value is 0 or the position of the first
 * set bit if value is nonzero. The first (least significant) bit
 * is at position 1.
 */
/**
 * ffs - 在字中查找第一个置位的位
 * @x: 要搜索的字
 *
 * 此函数与 libc 和编译器内建的 ffs 函数定义相同，因此在精神上与其他位操作函数有所不同。
 *
 * ffs(value) 当 value 为 0 时返回 0，当 value 非零时返回第一个（最低位）置位的位号，从1开始计数。
 */
static inline int ffs(int x)
{
	int r;
#ifdef CONFIG_X86_CMOV
	asm("bsfl %1,%0\n\t"
	    "cmovzl %2,%0"
	    : "=r" (r) : "rm" (x), "r" (-1));
#else
	asm("bsfl %1,%0\n\t"
	    "jnz 1f\n\t"
	    "movl $-1,%0\n"
	    "1:" : "=r" (r) : "rm" (x));
#endif
	return r + 1;
}

/**
 * fls - find last set bit in word
 * @x: the word to search
 *
 * This is defined in a similar way as the libc and compiler builtin
 * ffs, but returns the position of the most significant set bit.
 *
 * fls(value) returns 0 if value is 0 or the position of the last
 * set bit if value is nonzero. The last (most significant) bit is
 * at position 32.
 */
/**
 * fls - 在字中查找最后一个置位的位
 * @x: 要搜索的字
 *
 * 此函数与 libc 和编译器内建的 ffs 函数以类似的方式定义，但返回最高位的位置。
 *
 * fls(value) 当 value 为 0 时返回 0，当 value 非零时返回最后一个（最高位）置位的位号，从1开始计数。
 */
static inline int fls(int x)
{
	int r;
#ifdef CONFIG_X86_CMOV
	asm("bsrl %1,%0\n\t"
	    "cmovzl %2,%0"
	    : "=&r" (r) : "rm" (x), "rm" (-1));
#else
	asm("bsrl %1,%0\n\t"
	    "jnz 1f\n\t"
	    "movl $-1,%0\n"
	    "1:" : "=r" (r) : "rm" (x));
#endif
	return r + 1;
}
#endif /* __KERNEL__ */

#undef ADDR

#ifdef __KERNEL__

#include <asm-generic/bitops/sched.h>

#define ARCH_HAS_FAST_MULTIPLIER 1

#include <asm-generic/bitops/hweight.h>

#endif /* __KERNEL__ */

#include <asm-generic/bitops/fls64.h>

#ifdef __KERNEL__

#include <asm-generic/bitops/ext2-non-atomic.h>

#define ext2_set_bit_atomic(lock, nr, addr)			\
	test_and_set_bit((nr), (unsigned long *)(addr))
#define ext2_clear_bit_atomic(lock, nr, addr)			\
	test_and_clear_bit((nr), (unsigned long *)(addr))

#include <asm-generic/bitops/minix.h>

#endif /* __KERNEL__ */
#endif /* _ASM_X86_BITOPS_H */
