#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/init_task.h>
#include <linux/fs.h>
#include <linux/mqueue.h>

#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/desc.h>

// 初始化信号结构
static struct signal_struct init_signals = INIT_SIGNALS(init_signals);
// 初始化信号处理结构
static struct sighand_struct init_sighand = INIT_SIGHAND(init_sighand);

/*
 * Initial thread structure.
 *
 * We need to make sure that this is THREAD_SIZE aligned due to the
 * way process stacks are handled. This is done by having a special
 * "init_task" linker map entry..
 */
/*
 * 初始化线程结构。
 * 需要确保此结构按 THREAD_SIZE 对齐，因为这关系到进程堆栈的处理方式。
 * 这是通过设置一个特别的 “init_task” 链接映射条目来实现的。
 */
union thread_union init_thread_union __init_task_data =
	{ INIT_THREAD_INFO(init_task) };

/*
 * Initial task structure.
 *
 * All other task structs will be allocated on slabs in fork.c
 */
/*
 * 初始化任务结构。
 *
 * 其他所有任务结构将在 fork.c 中的 slab 上分配。
 */
struct task_struct init_task = INIT_TASK(init_task);
EXPORT_SYMBOL(init_task);

/*
 * per-CPU TSS segments. Threads are completely 'soft' on Linux,
 * no more per-task TSS's. The TSS size is kept cacheline-aligned
 * so they are allowed to end up in the .data.cacheline_aligned
 * section. Since TSS's are completely CPU-local, we want them
 * on exact cacheline boundaries, to eliminate cacheline ping-pong.
 */
/*
 * 每CPU的TSS段。线程在Linux上是完全“软”的，
 * 不再有每个任务的TSS。TSS大小保持与缓存线对齐，
 * 因此它们可以位于 .data.cacheline_aligned 部分。
 * 由于TSS完全是CPU本地的，我们希望它们位于确切的缓存行边界上，
 * 以避免缓存行之间的竞争。
 */

DEFINE_PER_CPU_SHARED_ALIGNED(struct tss_struct, init_tss) = INIT_TSS;

