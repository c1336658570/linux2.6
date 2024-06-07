/*
 * Flexible mmap layout support
 *
 * Based on code by Ingo Molnar and Andi Kleen, copyrighted
 * as follows:
 *
 * Copyright 2003-2009 Red Hat Inc.
 * All Rights Reserved.
 * Copyright 2005 Andi Kleen, SUSE Labs.
 * Copyright 2007 Jiri Kosina, SUSE Labs.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/personality.h>
#include <linux/mm.h>
#include <linux/random.h>
#include <linux/limits.h>
#include <linux/sched.h>
#include <asm/elf.h>

static unsigned int stack_maxrandom_size(void)
{
	unsigned int max = 0;
	if ((current->flags & PF_RANDOMIZE) &&
		!(current->personality & ADDR_NO_RANDOMIZE)) {
		max = ((-1U) & STACK_RND_MASK) << PAGE_SHIFT;
	}

	return max;
}


/*
 * Top of mmap area (just below the process stack).
 *
 * Leave an at least ~128 MB hole with possible stack randomization.
 */
#define MIN_GAP (128*1024*1024UL + stack_maxrandom_size())
#define MAX_GAP (TASK_SIZE/6*5)

/*
 * True on X86_32 or when emulating IA32 on X86_64
 */
static int mmap_is_ia32(void)
{
#ifdef CONFIG_X86_32
	return 1;
#endif
#ifdef CONFIG_IA32_EMULATION
	if (test_thread_flag(TIF_IA32))
		return 1;
#endif
	return 0;
}

static int mmap_is_legacy(void)
{
	if (current->personality & ADDR_COMPAT_LAYOUT)
		return 1;

	if (rlimit(RLIMIT_STACK) == RLIM_INFINITY)
		return 1;

	return sysctl_legacy_va_layout;
}

static unsigned long mmap_rnd(void)
{
	unsigned long rnd = 0;

	/*
	*  8 bits of randomness in 32bit mmaps, 20 address space bits
	* 28 bits of randomness in 64bit mmaps, 40 address space bits
	*/
	if (current->flags & PF_RANDOMIZE) {
		if (mmap_is_ia32())
			rnd = (long)get_random_int() % (1<<8);
		else
			rnd = (long)(get_random_int() % (1<<28));
	}
	return rnd << PAGE_SHIFT;
}

static unsigned long mmap_base(void)
{
	unsigned long gap = rlimit(RLIMIT_STACK);

	if (gap < MIN_GAP)
		gap = MIN_GAP;
	else if (gap > MAX_GAP)
		gap = MAX_GAP;

	return PAGE_ALIGN(TASK_SIZE - gap - mmap_rnd());
}

/*
 * Bottom-up (legacy) layout on X86_32 did not support randomization, X86_64
 * does, but not when emulating X86_32
 */
static unsigned long mmap_legacy_base(void)
{
	if (mmap_is_ia32())
		return TASK_UNMAPPED_BASE;
	else
		return TASK_UNMAPPED_BASE + mmap_rnd();
}

// 该函数在创建新进程的虚拟内存映像的非常早期阶段被调用，用于设置使用哪种虚拟内存（VM）布局函数。
/*
 * This function, called very early during the creation of a new
 * process VM image, sets up which VM layout function to use:
 */
/*
 * 该函数在新进程虚拟内存映像创建的非常早期被调用，用于设置使用哪种虚拟内存布局函数：
 */
void arch_pick_mmap_layout(struct mm_struct *mm)
{
	// 检查是否应使用传统的内存映射（mmap）布局
	if (mmap_is_legacy()) {
		// 如果是传统的内存映射布局，设置 mmap 基地址为传统基地址
		mm->mmap_base = mmap_legacy_base();
		// 设置获取未映射区域的函数为标准的底向上映射函数
		mm->get_unmapped_area = arch_get_unmapped_area;
		// 设置取消映射区域的函数为标准的取消映射函数
		mm->unmap_area = arch_unmap_area;
	} else {
		// 如果不使用传统布局，设置 mmap 基地址为默认的基地址
		mm->mmap_base = mmap_base();
		// 设置获取未映射区域的函数为自顶向下映射函数
		mm->get_unmapped_area = arch_get_unmapped_area_topdown;
		// 设置取消映射区域的函数为自顶向下的取消映射函数
		mm->unmap_area = arch_unmap_area_topdown;
	}
}
