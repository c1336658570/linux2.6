#ifndef _LINUX_RATELIMIT_H
#define _LINUX_RATELIMIT_H

#include <linux/param.h>
#include <linux/spinlock_types.h>

// 默认限速间隔，以系统时钟节拍数为单位，这里5*HZ表示5秒
#define DEFAULT_RATELIMIT_INTERVAL	(5 * HZ)
// 默认爆发次数，即在短时间内允许的最大记录数
#define DEFAULT_RATELIMIT_BURST		10

struct ratelimit_state {
	/* 用于保护rate limit状态的自旋锁 */
	spinlock_t	lock;		/* protect the state */

	int		interval;		/* 限制事件的间隔 */
	int		burst;			// 短时间内允许的最大事件数量（爆发量）
	int		printed;		/* 已经打印的消息数 */
	int		missed;			/* 由于速率限制而错过的消息数 */
	unsigned long	begin;	/* 速率限制的开始时间 */
};

// 定义并初始化一个速率限制状态变量
#define DEFINE_RATELIMIT_STATE(name, interval_init, burst_init)		\
									\
	struct ratelimit_state name = {					\
		/* 初始化自旋锁 */	\
		.lock		= __SPIN_LOCK_UNLOCKED(name.lock),	\
		/* 设置速率限制间隔 */	\
		.interval	= interval_init,			\
		/* 设置速率限制突发值 */	\
		.burst		= burst_init,				\
	}/* 初始化自旋锁 */

// 外部定义的速率限制函数
extern int ___ratelimit(struct ratelimit_state *rs, const char *func);
// 定义宏以方便使用当前函数名作为参数调用速率限制函数
#define __ratelimit(state) ___ratelimit(state, __func__)

#endif /* _LINUX_RATELIMIT_H */
