// 此文件是原子位操作相关的代码，与code arch/x86/include/asm/bitops.h相关联
#ifndef _ASM_GENERIC_BITOPS_FIND_H_
#define _ASM_GENERIC_BITOPS_FIND_H_

#ifndef CONFIG_GENERIC_FIND_NEXT_BIT
extern unsigned long find_next_bit(const unsigned long *addr, unsigned long
		size, unsigned long offset);

extern unsigned long find_next_zero_bit(const unsigned long *addr, unsigned
		long size, unsigned long offset);
#endif

// 第一个参数是一个指针，第二个参数是要搜索的总位数，返回值是低一个被设置的位的位号
#define find_first_bit(addr, size) find_next_bit((addr), (size), 0)
// 第一个参数是一个指针，第二个参数是要搜索的总位数，返回值是低一个没被设置的位的位号
#define find_first_zero_bit(addr, size) find_next_zero_bit((addr), (size), 0)

#endif /*_ASM_GENERIC_BITOPS_FIND_H_ */
