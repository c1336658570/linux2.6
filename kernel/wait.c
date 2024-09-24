/*
 * Generic waiting primitives.
 *
 * (C) 2004 William Irwin, Oracle
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/wait.h>
#include <linux/hash.h>

/**
 * __init_waitqueue_head - 初始化等待队列头
 * @q: 等待队列头的指针
 * @key: 锁类关键字
 *
 * 该函数用于初始化等待队列头，这是在Linux内核中配置等待队列的基础函数。
 */
void __init_waitqueue_head(wait_queue_head_t *q, struct lock_class_key *key)
{
  /* 初始化等待队列头中的自旋锁 */
	spin_lock_init(&q->lock);
  /* 将自旋锁与锁类关键字关联，这在lockdep（一个用于检测内核锁依赖问题的工具）中用于检测死锁 */
	lockdep_set_class(&q->lock, key);
  /* 初始化等待队列的任务列表，使其成为一个空的循环双向链表 */
	INIT_LIST_HEAD(&q->task_list);
}
EXPORT_SYMBOL(__init_waitqueue_head);

/**
 * add_wait_queue - 将等待队列元素添加到等待队列头
 * @q: 等待队列头的指针
 * @wait: 需要添加到队列的等待队列元素
 *
 * 此函数用于将非独占的等待队列元素添加到指定的等待队列头中。
 */
// 向等待队列中添加元素
void add_wait_queue(wait_queue_head_t *q, wait_queue_t *wait)
{
	unsigned long flags;	// 用于保存中断状态

	/* 清除等待队列元素的独占标志，清除该元素的独占标志，这表示该元素在唤醒时不需要独占处理 */
	wait->flags &= ~WQ_FLAG_EXCLUSIVE;
	/* 上锁并保存中断状态，确保操作的原子性和防止中断干扰 */
	spin_lock_irqsave(&q->lock, flags);
	/* 将等待队列元素添加到队列头中 */
	__add_wait_queue(q, wait);
	/* 解锁并恢复之前的中断状态 */
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(add_wait_queue);

/**
 * add_wait_queue_exclusive - 将等待队列元素以独占方式添加到等待队列头
 * @q: 等待队列头的指针
 * @wait: 需要添加到队列的等待队列元素
 *
 * 此函数用于将独占的等待队列元素添加到指定的等待队列头中。
 */
void add_wait_queue_exclusive(wait_queue_head_t *q, wait_queue_t *wait)
{
	unsigned long flags;	// 用于保存中断状态

	/* 设置等待队列元素的独占标志 */
	wait->flags |= WQ_FLAG_EXCLUSIVE;
	/* 上锁并保存中断状态，确保操作的原子性和防止中断干扰 */
	spin_lock_irqsave(&q->lock, flags);
	/* 将等待队列元素添加到队列尾部，适用于独占等待 */
	__add_wait_queue_tail(q, wait);
	/* 解锁并恢复之前的中断状态 */
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(add_wait_queue_exclusive);

/**
 * remove_wait_queue - 从等待队列头中移除等待队列元素
 * @q: 等待队列头的指针
 * @wait: 需要移除的等待队列元素
 *
 * 此函数用于从等待队列头中移除指定的等待队列元素。
 */
void remove_wait_queue(wait_queue_head_t *q, wait_queue_t *wait)
{
	unsigned long flags;  // 用于保存中断状态

	/* 上锁并保存中断状态，确保操作的原子性和防止中断干扰 */
	spin_lock_irqsave(&q->lock, flags);
	/* 从队列中移除指定的等待队列元素 */
	__remove_wait_queue(q, wait);
	/* 解锁并恢复之前的中断状态 */
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(remove_wait_queue);


/*
 * Note: we use "set_current_state()" _after_ the wait-queue add,
 * because we need a memory barrier there on SMP, so that any
 * wake-function that tests for the wait-queue being active
 * will be guaranteed to see waitqueue addition _or_ subsequent
 * tests in this thread will see the wakeup having taken place.
 *
 * The spin_unlock() itself is semi-permeable and only protects
 * one way (it only protects stuff inside the critical region and
 * stops them from bleeding out - it would still allow subsequent
 * loads to move into the critical region).
 */
/*
 * 注意：我们在添加等待队列之后使用“set_current_state()”，
 * 因为我们需要在SMP（对称多处理）上一个内存屏障，
 * 这样任何检查等待队列是否活跃的唤醒函数都将保证看到等待队列添加，
 * 或者在这个线程的后续测试将看到唤醒已经发生。
 *
 * spin_unlock()本身是半透明的，只保护一种方式（它只保护在临界区内的事务，
 * 阻止它们外泄——它仍然会允许随后的加载移动到临界区内）。
 */
/*
 * 在 prepare_to_wait 函数中，首先通过清除 WQ_FLAG_EXCLUSIVE 标志，将等待队列元素设置为非独占模式。
 * 接着在自旋锁保护下检查该元素是否已经在等待队列中，如果不在，则添加至等待队列头。此外，通过 set_current_state 
 * 函数设置进程的状态（如可中断的睡眠状态 TASK_INTERRUPTIBLE 或不可中断的睡眠状态 TASK_UNINTERRUPTIBLE），
 * 确保进程在被唤醒前正确地处于休眠状态。最后，解锁并恢复之前保存的中断状态，以继续执行。
 */
// 修改进程状态，可以修改为TASK_INTERRUPTIBLE或TASK_UNINTERRUPTIBLE
void
prepare_to_wait(wait_queue_head_t *q, wait_queue_t *wait, int state)
{
	unsigned long flags;

	/* 清除等待队列元素的独占标志 */
	wait->flags &= ~WQ_FLAG_EXCLUSIVE;
	/* 上锁并保存中断状态，确保操作的原子性和防止中断干扰 */
	spin_lock_irqsave(&q->lock, flags);
	/* 如果等待队列元素不在任何等待队列中，则添加到等待队列头 */
	if (list_empty(&wait->task_list))
		__add_wait_queue(q, wait);
	/* 设置当前线程的状态为指定状态，确保在等待期间的正确睡眠状态 */
	set_current_state(state);
	/* 解锁并恢复之前的中断状态 */
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(prepare_to_wait);

/*
 * 准备以独占方式等待：此函数用于将当前进程以独占模式添加到等待队列中，并设置进程的状态。
 * 在添加到等待队列之前，进程的状态被设置为指定的状态（如TASK_INTERRUPTIBLE或TASK_UNINTERRUPTIBLE），
 * 以确保在被唤醒之前进程处于正确的休眠状态。
 */
// 独占代表着当此进程被唤醒时，不会唤醒其他等待此条件的进程。
void
prepare_to_wait_exclusive(wait_queue_head_t *q, wait_queue_t *wait, int state)
{
	unsigned long flags;  // 用于保存中断状态

	/* 设置等待队列元素的独占标志 */
	wait->flags |= WQ_FLAG_EXCLUSIVE;
	/* 上锁并保存中断状态，确保操作的原子性和防止中断干扰 */
	spin_lock_irqsave(&q->lock, flags);
	/* 如果等待队列元素不在任何等待队列中，则以独占方式添加到等待队列的尾部 */
	if (list_empty(&wait->task_list))
		__add_wait_queue_tail(q, wait);
	/* 设置当前线程的状态为指定状态，确保在等待期间的正确睡眠状态 */
	set_current_state(state);
	/* 解锁并恢复之前的中断状态 */
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(prepare_to_wait_exclusive);

/*
 * finish_wait - clean up after waiting in a queue
 * @q: waitqueue waited on
 * @wait: wait descriptor
 *
 * Sets current thread back to running state and removes
 * the wait descriptor from the given waitqueue if still
 * queued.
 */
/*
 * finish_wait - 在等待队列中等待后进行清理工作
 * @q: 等待队列
 * @wait: 等待描述符
 *
 * 将当前线程状态设置回运行状态，并从给定的等待队列中移除等待描述符（如果仍在队列中）。
 */
void finish_wait(wait_queue_head_t *q, wait_queue_t *wait)
{
	unsigned long flags;

	/* 设置当前线程状态为运行 */
	__set_current_state(TASK_RUNNING);
	/*
	 * We can check for list emptiness outside the lock
	 * IFF:
	 *  - we use the "careful" check that verifies both
	 *    the next and prev pointers, so that there cannot
	 *    be any half-pending updates in progress on other
	 *    CPU's that we haven't seen yet (and that might
	 *    still change the stack area.
	 * and
	 *  - all other users take the lock (ie we can only
	 *    have _one_ other CPU that looks at or modifies
	 *    the list).
	 */
	/*
   * 我们可以在不加锁的情况下检查列表是否为空，如果：
   *  - 我们使用“小心”检查，验证前后指针都不为空，这样就不会有任何半完成的更新在其他
   *    CPU上进行，我们还未看到（这些更新可能仍会改变堆栈区域）。
   * 并且
   *  - 所有其他用户都加了锁（即我们只能有一个其他CPU查看或修改列表）。
   */
	// 使用 list_empty_careful 进行检查，这是一个“小心”的列表空检查，它确保了在不加锁的情况下能安全地检查列表是否为空。这种检查方法特别适用于多处理器环境中，可以确保没有其他处理器正在半途修改列表。
	if (!list_empty_careful(&wait->task_list)) {
		/* 加锁并保存中断状态，保证操作的原子性和防止中断干扰 */
		spin_lock_irqsave(&q->lock, flags);
		/* 从等待队列中移除等待描述符，并重新初始化描述符的列表节点 */
		list_del_init(&wait->task_list);
		/* 解锁并恢复之前的中断状态 */
		spin_unlock_irqrestore(&q->lock, flags);
	}
}
EXPORT_SYMBOL(finish_wait);

/*
 * abort_exclusive_wait - abort exclusive waiting in a queue
 * @q: waitqueue waited on
 * @wait: wait descriptor
 * @state: runstate of the waiter to be woken
 * @key: key to identify a wait bit queue or %NULL
 *
 * Sets current thread back to running state and removes
 * the wait descriptor from the given waitqueue if still
 * queued.
 *
 * Wakes up the next waiter if the caller is concurrently
 * woken up through the queue.
 *
 * This prevents waiter starvation where an exclusive waiter
 * aborts and is woken up concurrently and noone wakes up
 * the next waiter.
 */
/*
 * abort_exclusive_wait - 中止在队列中的独占等待
 * @q: 正在等待的等待队列
 * @wait: 等待描述符
 * @mode: 被唤醒等待者的运行状态
 * @key: 用于识别等待位队列的键，或为%NULL
 *
 * 将当前线程状态设置回运行状态，并从给定的等待队列中移除等待描述符（如果仍在队列中）。
 *
 * 如果调用者通过队列同时被唤醒，则唤醒下一个等待者。
 *
 * 这防止了独占等待者中止并同时被唤醒，而没有人唤醒下一个等待者的情况下发生的等待者饥饿。
 */
void abort_exclusive_wait(wait_queue_head_t *q, wait_queue_t *wait,
			unsigned int mode, void *key)
{
	unsigned long flags;

	/* 设置当前线程状态为运行 */
	__set_current_state(TASK_RUNNING);
	/* 加锁并保存中断状态，保证操作的原子性和防止中断干扰 */
	spin_lock_irqsave(&q->lock, flags);
	/* 如果等待描述符仍在队列中，则从队列中移除并重新初始化该描述符 */
	if (!list_empty(&wait->task_list))
		list_del_init(&wait->task_list);
	else if (waitqueue_active(q))
		/* 如果等待队列仍然活跃，根据提供的模式和键唤醒下一个等待者 */
		__wake_up_locked_key(q, mode, key);
	/* 解锁并恢复之前的中断状态 */
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(abort_exclusive_wait);

/*
 * autoremove_wake_function - 在等待队列中唤醒函数的自动移除版本
 * @wait: 等待队列元素
 * @mode: 唤醒模式
 * @sync: 同步标志
 * @key: 键值
 *
 * 使用默认的唤醒函数唤醒等待队列中的进程，并在成功唤醒后自动从等待队列中移除该等待元素。
 */
// 等待队列的autoremove_wake_function函数，使用DEFINE_WAIT定义等待队列，默认使用这个函数，DEFINE_WAIT在wait.h中
int autoremove_wake_function(wait_queue_t *wait, unsigned mode, int sync, void *key)
{
	/* 使用默认唤醒函数唤醒等待元素 */
	int ret = default_wake_function(wait, mode, sync, key);		// 在sched中定义的唤醒函数

	/* 如果唤醒成功，从等待队列中移除并重新初始化等待队列元素 */
	if (ret)
		list_del_init(&wait->task_list);
	return ret;
}
EXPORT_SYMBOL(autoremove_wake_function);

/*
 * wake_bit_function - 特定位标志的唤醒函数
 * @wait: 等待队列元素
 * @mode: 唤醒模式
 * @sync: 同步标志
 * @arg: 传递给唤醒函数的参数，包括位标志
 *
 * 检查位标志，如果符合条件，则使用autoremove_wake_function进行唤醒。
 */
int wake_bit_function(wait_queue_t *wait, unsigned mode, int sync, void *arg)
{
	/* 从arg参数中提取位键 */
	struct wait_bit_key *key = arg;
	struct wait_bit_queue *wait_bit
		= container_of(wait, struct wait_bit_queue, wait);

	/* 检查位键是否匹配，并且相应的位已经被设置 */
	if (wait_bit->key.flags != key->flags ||
			wait_bit->key.bit_nr != key->bit_nr ||
			test_bit(key->bit_nr, key->flags))
			return 0;
	else
		/* 如果条件匹配，使用autoremove_wake_function唤醒并尝试移除等待队列元素 */
		return autoremove_wake_function(wait, mode, sync, key);
}
EXPORT_SYMBOL(wake_bit_function);

/*
 * To allow interruptible waiting and asynchronous (i.e. nonblocking)
 * waiting, the actions of __wait_on_bit() and __wait_on_bit_lock() are
 * permitted return codes. Nonzero return codes halt waiting and return.
 */
/*
 * 为了允许中断式等待和异步（即非阻塞）等待，__wait_on_bit() 和 __wait_on_bit_lock() 的操作
 * 允许返回代码。非零返回代码将停止等待并返回。
 *
 * 这个函数用于在特定的位标志上等待，直到位被清除或动作函数决定停止等待。
 */
// 用于处理基于位的同步问题。
int __sched
__wait_on_bit(wait_queue_head_t *wq, struct wait_bit_queue *q,
			int (*action)(void *), unsigned mode)
{
	int ret = 0;

	do {
		/* 准备等待，将当前进程加入等待队列，并设置为指定模式（如可中断或不可中断） */
		prepare_to_wait(wq, &q->wait, mode);
		/* 检查位标志，如果位仍然设置，调用动作函数 */
		if (test_bit(q->key.bit_nr, q->key.flags))
			ret = (*action)(q->key.flags);

	/* 如果位仍然设置且动作函数返回0，则继续等待 */
	} while (test_bit(q->key.bit_nr, q->key.flags) && !ret);
	/* 完成等待，从等待队列中移除当前进程 */
	finish_wait(wq, &q->wait);
	return ret;
}
EXPORT_SYMBOL(__wait_on_bit);

/*
 * out_of_line_wait_on_bit - 在指定的位上进行等待，直到条件满足或通过动作函数中断
 * @word: 指向位的内存地址
 * @bit: 需要检查的具体位
 * @action: 当位为设置状态时调用的函数
 * @mode: 等待模式（例如TASK_INTERRUPTIBLE）
 *
 * 这个函数是一个高层次的封装，用于在一个特定的位上等待。它使用 __wait_on_bit 函数来执行实际的等待。
 */
int __sched out_of_line_wait_on_bit(void *word, int bit,
					int (*action)(void *), unsigned mode)
{
	/* 获取与指定位相关联的等待队列头 */
	wait_queue_head_t *wq = bit_waitqueue(word, bit);
	/* 定义一个等待位，这里使用了一个宏来初始化等待队列元素和位键 */
	DEFINE_WAIT_BIT(wait, word, bit);

	/* 调用 __wait_on_bit 函数进行实际的等待，传入等待队列头、等待位、动作函数和等待模式 */
	return __wait_on_bit(wq, &wait, action, mode);
}
EXPORT_SYMBOL(out_of_line_wait_on_bit);

/*
 * __wait_on_bit_lock - 在获取特定位的锁时等待
 * @wq: 等待队列头
 * @q: 等待位队列结构
 * @action: 当检测到位为设置状态时调用的函数
 * @mode: 等待模式（例如TASK_INTERRUPTIBLE）
 *
 * 这个函数用于等待直到一个位被清除，或者通过特定动作成功获取锁。如果获取锁失败，它将释放锁并返回。
 */
int __sched
__wait_on_bit_lock(wait_queue_head_t *wq, struct wait_bit_queue *q,
			int (*action)(void *), unsigned mode)
{
	do {
		int ret;

		/* 以独占方式准备等待，避免多个线程同时操作 */
		prepare_to_wait_exclusive(wq, &q->wait, mode);
		/* 测试位是否已被清除，如果是，继续循环（尝试获取锁） */
		if (!test_bit(q->key.bit_nr, q->key.flags))
			continue;
		/* 如果位为设置状态，调用action函数处理锁获取 */
		ret = action(q->key.flags);
		/* 如果action函数返回0，说明没有成功处理，继续等待 */
		if (!ret)
			continue;
		/* 如果获取锁失败或不再需要等待，中止独占等待并返回结果 */
		abort_exclusive_wait(wq, &q->wait, mode, &q->key);
		return ret;
	} while (test_and_set_bit(q->key.bit_nr, q->key.flags));  // 在检查并设置位时循环等待
	/* 完成等待，清理等待队列 */
	finish_wait(wq, &q->wait);
	return 0;  // 如果退出循环，返回0表示未获取锁
}
EXPORT_SYMBOL(__wait_on_bit_lock);

/*
 * out_of_line_wait_on_bit_lock - 在指定的位上进行锁定等待，直到条件满足或通过动作函数中断
 * @word: 指向位的内存地址
 * @bit: 需要检查的具体位
 * @action: 当位为设置状态时调用的函数
 * @mode: 等待模式（例如TASK_INTERRUPTIBLE）
 *
 * 这个函数是一个高层次的封装，用于在一个特定的位上等待并尝试获取锁。它使用 __wait_on_bit_lock 函数来执行实际的锁定等待。
 */
int __sched out_of_line_wait_on_bit_lock(void *word, int bit,
					int (*action)(void *), unsigned mode)
{
	/* 获取与指定位相关联的等待队列头 */
	wait_queue_head_t *wq = bit_waitqueue(word, bit);
	/* 定义一个等待位，这里使用了一个宏来初始化等待队列元素和位键 */
	DEFINE_WAIT_BIT(wait, word, bit);

	/* 调用 __wait_on_bit_lock 函数进行实际的锁定等待，传入等待队列头、等待位、动作函数和等待模式 */
	return __wait_on_bit_lock(wq, &wait, action, mode);
}
EXPORT_SYMBOL(out_of_line_wait_on_bit_lock);


/*
 * __wake_up_bit - 唤醒在特定位上等待的所有线程
 * @wq: 等待队列头
 * @word: 指向位的内存地址
 * @bit: 需要唤醒等待线程的具体位
 *
 * 如果等待队列处于活跃状态，该函数将唤醒在指定位上等待的所有线程。
 */
void __wake_up_bit(wait_queue_head_t *wq, void *word, int bit)
{
	/* 初始化等待位键 */
	struct wait_bit_key key = __WAIT_BIT_KEY_INITIALIZER(word, bit);
	/* 如果等待队列是活跃的，唤醒所有在该队列上等待的线程 */
	if (waitqueue_active(wq))
		__wake_up(wq, TASK_NORMAL, 1, &key);
}
EXPORT_SYMBOL(__wake_up_bit);

/**
 * wake_up_bit - wake up a waiter on a bit
 * @word: the word being waited on, a kernel virtual address
 * @bit: the bit of the word being waited on
 *
 * There is a standard hashed waitqueue table for generic use. This
 * is the part of the hashtable's accessor API that wakes up waiters
 * on a bit. For instance, if one were to have waiters on a bitflag,
 * one would call wake_up_bit() after clearing the bit.
 *
 * In order for this to function properly, as it uses waitqueue_active()
 * internally, some kind of memory barrier must be done prior to calling
 * this. Typically, this will be smp_mb__after_clear_bit(), but in some
 * cases where bitflags are manipulated non-atomically under a lock, one
 * may need to use a less regular barrier, such fs/inode.c's smp_mb(),
 * because spin_unlock() does not guarantee a memory barrier.
 */
/**
 * wake_up_bit - 唤醒位上等待的进程
 * @word: 正在等待的word,一个内核虚拟地址
 * @bit: word上正在等待的位
 *
 * 这里有一个标准的散列等待队列表用于普遍用途。这是哈希表访问器API的一部分,唤醒位上等待的进程。
 * 例如,如果有进程等待一个位标志,在清除该位后就可以调用wake_up_bit()。
 *
 * 为了使这个函数正常工作,它在内部使用waitqueue_active(),因此在调用之前必须进行某种内存屏障。
 * 通常这将是smp_mb__after_clear_bit(),但在某些情况下,当位标志在锁保护下非原子性地操作时,
 * 可能需要使用更不规则的屏障,如fs/inode.c中的smp_mb(),因为spin_unlock()不能保证内存屏障。
 */
void wake_up_bit(void *word, int bit)
{
	/* 直接调用 __wake_up_bit 以唤醒在特定位上等待的线程 */
	__wake_up_bit(bit_waitqueue(word, bit), word, bit);
}
EXPORT_SYMBOL(wake_up_bit);

/*
 * bit_waitqueue - 获取与特定内存位关联的等待队列头
 * @word: 指向位的内存地址
 * @bit: 相关的位
 *
 * 根据内存位和位的具体位置确定等待队列头，使用哈希表进行快速定位。
 */
wait_queue_head_t *bit_waitqueue(void *word, int bit)
{
	// 确定一个指向等待队列表的指针
	// 根据系统位宽(32位或64位),决定偏移量
	const int shift = BITS_PER_LONG == 32 ? 5 : 6;	// 确定基于系统位宽（32位或64位）的偏移量
	// 获取word所在页面的zone
	const struct zone *zone = page_zone(virt_to_page(word));	// 获取word所在内存页的zone结构
	// 将word和bit组合成一个值,作为哈希表的索引
	unsigned long val = (unsigned long)word << shift | bit;	// 结合word地址和位号生成一个哈希值

	// 返回等待队列表的地址
	return &zone->wait_table[hash_long(val, zone->wait_table_bits)];	// 使用生成的哈希值找到对应的等待队列头
}
EXPORT_SYMBOL(bit_waitqueue);
