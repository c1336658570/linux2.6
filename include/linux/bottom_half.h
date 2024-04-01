#ifndef _LINUX_BH_H
#define _LINUX_BH_H

// 通过增加preempt_count禁止本地中断下半部。有几次local_bh_disable，就需要有几次local_bh_enable
extern void local_bh_disable(void);
extern void _local_bh_enable(void);
// 通过减少preempt_count来激活本地下半部。
// 如果preempt_count返回值为0,则将导致自动激活下半部（即该函数内部判断preempt_count是否为0,如果为0就主动调用do_softirq()）
extern void local_bh_enable(void);
extern void local_bh_enable_ip(unsigned long ip);

#endif /* _LINUX_BH_H */
