#ifndef _LINUX_IRQRETURN_H
#define _LINUX_IRQRETURN_H

/**
 * enum irqreturn
 * @IRQ_NONE		interrupt was not from this device
 * @IRQ_HANDLED		interrupt was handled by this device
 * @IRQ_WAKE_THREAD	handler requests to wake the handler thread
 */
// 这个枚举是中断处理程序的返回类型
enum irqreturn {
	IRQ_NONE,			// 中断处理程序检测到一个中断，但是该中断对应的设备并不是在注册处理函数期间指定的产生源时返回IRQ_NONE
	IRQ_HANDLED,	// 中断处理程序被正确调用，并且确实是它所对应的设备产生中断，返回IRQ_HANDLED
	IRQ_WAKE_THREAD,
};

typedef enum irqreturn irqreturn_t;
#define IRQ_RETVAL(x)	((x) != IRQ_NONE)

#endif
