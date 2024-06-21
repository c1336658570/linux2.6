/*
 * ratelimit.c - Do something with rate limit.
 *
 * Isolated from kernel/printk.c by Dave Young <hidave.darkstar@gmail.com>
 *
 * 2008-05-01 rewrite the function and use a ratelimit_state data struct as
 * parameter. Now every user can use their own standalone ratelimit_state.
 *
 * This file is released under the GPLv2.
 */

#include <linux/ratelimit.h>
#include <linux/jiffies.h>
#include <linux/module.h>

/*
 * __ratelimit - rate limiting
 * @rs: ratelimit_state data
 * @func: name of calling function
 *
 * This enforces a rate limit: not more than @rs->burst callbacks
 * in every @rs->interval
 *
 * RETURNS:
 * 0 means callbacks will be suppressed.
 * 1 means go ahead and do it.
 */
/*
 * __ratelimit - 速率限制功能
 * @rs: ratelimit_state 数据结构
 * @func: 调用函数的名称
 *
 * 该函数实施速率限制：在每个 @rs->interval 时间内最多允许 @rs->burst 个回调
 *
 * 返回值：
 * 0 表示回调将被抑制。
 * 1 表示可以继续执行。
 */
int ___ratelimit(struct ratelimit_state *rs, const char *func)
{
	unsigned long flags;	// 用于保存中断状态
	int ret;		// 函数返回值

	if (!rs->interval)
		return 1;	// 如果没有设置时间间隔，直接允许回调

	/*
	 * If we contend on this state's lock then almost
	 * by definition we are too busy to print a message,
	 * in addition to the one that will be printed by
	 * the entity that is holding the lock already:
	 */
	/*
	 * 如果在获取锁时发生竞争，那么我们几乎可以确定现在打印消息的操作太频繁了，
	 * 除了当前持有锁的实体之外，其他的打印消息应该被抑制：
	 */
	if (!spin_trylock_irqsave(&rs->lock, flags))
		return 0;	// 如果尝试获取自旋锁失败，则返回0，表示此次回调被抑制

	// 如果开始时间未设置，初始化为当前时间
	if (!rs->begin)
		rs->begin = jiffies;

	// 检查当前时间是否超过了间隔期
	if (time_is_before_jiffies(rs->begin + rs->interval)) {
		// 如果有被抑制的回调，打印警告信息
		if (rs->missed)
			printk(KERN_WARNING "%s: %d callbacks suppressed\n",
				func, rs->missed);
		rs->begin   = 0;
		rs->printed = 0;
		rs->missed  = 0;
	}
	if (rs->burst && rs->burst > rs->printed) {
		// 检查是否还有剩余的burst限额，并增加已打印计数
		rs->printed++;
		ret = 1;
	} else {
		// 如果burst限额已用完，增加missed计数，并设置返回值为0
		rs->missed++;
		ret = 0;
	}
	// 释放锁并恢复之前的中断状态
	spin_unlock_irqrestore(&rs->lock, flags);

	// 返回是否允许继续执行的结果
	return ret;
}
EXPORT_SYMBOL(___ratelimit);
