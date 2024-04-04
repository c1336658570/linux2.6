// 有关系统定时器频率（节拍器）的定义
#ifndef __ASM_GENERIC_PARAM_H
#define __ASM_GENERIC_PARAM_H

#ifdef __KERNEL__
// X86默认系统定时器频率是100HZ。因此，时钟中断的频率就是100HZ，即每10ms产生一次中断。
# define HZ		CONFIG_HZ	/* Internal kernel timer frequency */
// USER_HZ，用户态程序使用的HZ
# define USER_HZ	100		/* some user interfaces are */
# define CLOCKS_PER_SEC	(USER_HZ)       /* in "ticks" like times() */
#endif

#ifndef HZ
#define HZ 100
#endif

#ifndef EXEC_PAGESIZE
#define EXEC_PAGESIZE	4096
#endif

#ifndef NOGROUP
#define NOGROUP		(-1)
#endif

#define MAXHOSTNAMELEN	64	/* max length of hostname */

#endif /* __ASM_GENERIC_PARAM_H */
