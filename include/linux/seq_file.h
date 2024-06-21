#ifndef _LINUX_SEQ_FILE_H
#define _LINUX_SEQ_FILE_H

#include <linux/types.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/cpumask.h>
#include <linux/nodemask.h>

struct seq_operations;
struct file;
struct path;
struct inode;
struct dentry;

// seq_file结构体定义：用于表示序列文件的内部结构
struct seq_file {
	char *buf;			// 指向缓冲区的指针
	size_t size;		// 缓冲区的总大小
	size_t from;		// 数据读取的起始位置（用于部分读取）
	size_t count;		// 已经写入缓冲区的字节数
	loff_t index;		// 当前记录的索引
	loff_t read_pos;	// 当前读取位置
	u64 version;		// 版本号，用于追踪缓冲区的更新
	struct mutex lock;	// 互斥锁，用于保护seq_file结构
	const struct seq_operations *op;	// 指向操作函数集的指针
	void *private;	// 私有数据指针
};

// seq_operations结构体定义：包含操作序列文件的函数指针
struct seq_operations {
	// 开始遍历序列的函数
	void * (*start) (struct seq_file *m, loff_t *pos);
	// 停止遍历序列的函数
	void (*stop) (struct seq_file *m, void *v);
	// 获取下一个元素的函数
	void * (*next) (struct seq_file *m, void *v, loff_t *pos);
	// 将元素显示到缓冲区的函数
	int (*show) (struct seq_file *m, void *v);
};

#define SEQ_SKIP 1	// 定义SEQ_SKIP常量，用于指示跳过当前序列中的某些部分

/**
 * seq_get_buf - get buffer to write arbitrary data to
 * @m: the seq_file handle
 * @bufp: the beginning of the buffer is stored here
 *
 * Return the number of bytes available in the buffer, or zero if
 * there's no space.
 */
/**
 * 获取缓冲区以写入任意数据
 * @m: seq_file句柄
 * @bufp: 这里存储缓冲区的起始位置
 *
 * 返回缓冲区中的可用字节数，如果没有空间，则返回零。
 */
static inline size_t seq_get_buf(struct seq_file *m, char **bufp)
{
	// 如果已使用的字节数超过总大小，触发BUG
	BUG_ON(m->count > m->size);
	if (m->count < m->size)
		// 设置bufp指向缓冲区当前可写入的位置
		*bufp = m->buf + m->count;
	else
		// 如果缓冲区已满，设置bufp为NULL	
		*bufp = NULL;

	return m->size - m->count;	// 返回缓冲区中剩余的可用字节数
}

/**
 * seq_commit - commit data to the buffer
 * @m: the seq_file handle
 * @num: the number of bytes to commit
 *
 * Commit @num bytes of data written to a buffer previously acquired
 * by seq_buf_get.  To signal an error condition, or that the data
 * didn't fit in the available space, pass a negative @num value.
 */
/**
 * 将数据提交到缓冲区
 * @m: seq_file句柄
 * @num: 要提交的字节数
 *
 * 将通过seq_buf_get之前获取的缓冲区中写入的@num字节的数据提交。
 * 若要表示一个错误条件，或数据没有适应可用空间，传递一个负的@num值。
 */
static inline void seq_commit(struct seq_file *m, int num)
{
	if (num < 0) {
		// 如果num为负，将count设置为缓冲区的总大小，表示缓冲区已满
		m->count = m->size;
	} else {
		// 检查是否超出缓冲区大小，如果是则触发BUG
		BUG_ON(m->count + num > m->size);
		// 将num字节的数据标记为已提交
		m->count += num;
	}
}

// 修改路径字符串，可能涉及转义字符处理
char *mangle_path(char *s, char *p, char *esc);
// 打开序列文件
int seq_open(struct file *, const struct seq_operations *);
// 从序列文件中读取数据
ssize_t seq_read(struct file *, char __user *, size_t, loff_t *);
// 在序列文件中进行位置跳转
loff_t seq_lseek(struct file *, loff_t, int);
// 关闭序列文件
int seq_release(struct inode *, struct file *);
// 在序列文件中转义特定的字符
int seq_escape(struct seq_file *, const char *, const char *);
// 写一个字符到序列文件
int seq_putc(struct seq_file *m, char c);
// 写一个字符串到序列文件
int seq_puts(struct seq_file *m, const char *s);
// 写数据到序列文件
int seq_write(struct seq_file *seq, const void *data, size_t len);

// 格式化输出到序列文件
int seq_printf(struct seq_file *, const char *, ...)
// 属性标记，用于类型检查
	__attribute__ ((format (printf,2,3)));

// 输出路径到序列文件
int seq_path(struct seq_file *, struct path *, char *);
// 输出目录项到序列文件
int seq_dentry(struct seq_file *, struct dentry *, char *);
// 输出根路径到序列文件，可能涉及转义字符处理
int seq_path_root(struct seq_file *m, struct path *path, struct path *root,
		  char *esc);
// 输出位图到序列文件
int seq_bitmap(struct seq_file *m, const unsigned long *bits,
				   unsigned int nr_bits);
static inline int seq_cpumask(struct seq_file *m, const struct cpumask *mask)
{
	// 输出CPU掩码到序列文件
	return seq_bitmap(m, cpumask_bits(mask), nr_cpu_ids);
}

static inline int seq_nodemask(struct seq_file *m, nodemask_t *mask)
{
	// 输出节点掩码到序列文件
	return seq_bitmap(m, mask->bits, MAX_NUMNODES);
}

// 定义函数seq_bitmap_list，该函数用于将位图数据输出到序列文件中。
int seq_bitmap_list(struct seq_file *m, const unsigned long *bits,
		unsigned int nr_bits);

// 内联函数seq_cpumask_list，用于将CPU掩码输出到序列文件中。
static inline int seq_cpumask_list(struct seq_file *m,
				   const struct cpumask *mask)
{
	// 调用seq_bitmap_list函数，传递CPU掩码的位数组和CPU数量。
	return seq_bitmap_list(m, cpumask_bits(mask), nr_cpu_ids);
}

// 内联函数seq_nodemask_list，用于将节点掩码输出到序列文件中。
static inline int seq_nodemask_list(struct seq_file *m, nodemask_t *mask)
{
	// 调用seq_bitmap_list函数，传递节点掩码的位数组和最大节点数。
	return seq_bitmap_list(m, mask->bits, MAX_NUMNODES);
}

// 函数single_open，用于打开一个单一条目的序列文件。
int single_open(struct file *, int (*)(struct seq_file *, void *), void *);
// 函数single_release，用于关闭一个单一条目的序列文件。
int single_release(struct inode *, struct file *);
// 函数__seq_open_private，用于在具有私有数据的序列文件上初始化seq_file结构。
void *__seq_open_private(struct file *, const struct seq_operations *, int);
// 函数seq_open_private，用于打开具有私有数据的序列文件。
int seq_open_private(struct file *, const struct seq_operations *, int);
// 函数seq_release_private，用于关闭具有私有数据的序列文件。
int seq_release_private(struct inode *, struct file *);

// 定义SEQ_START_TOKEN宏，它表示序列文件迭代的起始标记。
#define SEQ_START_TOKEN ((void *)1)

/*
 * Helpers for iteration over list_head-s in seq_files
 */
/*
 * 辅助函数，用于在seq_files中迭代list_head-s
 */

// 函数seq_list_start，从指定位置开始遍历链表。
extern struct list_head *seq_list_start(struct list_head *head,
		loff_t pos);
// 函数seq_list_start_head，从链表的头部开始遍历链表，即使位置为0。
extern struct list_head *seq_list_start_head(struct list_head *head,
		loff_t pos);
// 函数seq_list_next，用于获取链表中的下一个元素，更新位置信息。
extern struct list_head *seq_list_next(void *v, struct list_head *head,
		loff_t *ppos);

/*
 * Helpers for iteration over hlist_head-s in seq_files
 */
/*
 * 辅助函数，用于在seq_files中迭代hlist_head-s
 */

// 函数seq_hlist_start，用于从指定位置开始遍历哈希链表。
extern struct hlist_node *seq_hlist_start(struct hlist_head *head,
					  loff_t pos);
// 函数seq_hlist_start_head，用于从哈希链表的头部开始遍历，即使位置为0。
extern struct hlist_node *seq_hlist_start_head(struct hlist_head *head,
					       loff_t pos);
// 函数seq_hlist_next，用于获取哈希链表中的下一个元素，更新位置信息。
extern struct hlist_node *seq_hlist_next(void *v, struct hlist_head *head,
					 loff_t *ppos);

// 函数seq_hlist_start_rcu，用于在RCU（读-拷贝更新）保护下从指定位置开始遍历哈希链表。
extern struct hlist_node *seq_hlist_start_rcu(struct hlist_head *head,
					      loff_t pos);
// 函数seq_hlist_start_head_rcu，用于在RCU保护下从哈希链表的头部开始遍历，即使位置为0。
extern struct hlist_node *seq_hlist_start_head_rcu(struct hlist_head *head,
						   loff_t pos);
// 函数seq_hlist_next_rcu，用于在RCU保护下获取哈希链表中的下一个元素，更新位置信息。
extern struct hlist_node *seq_hlist_next_rcu(void *v,
						   struct hlist_head *head,
						   loff_t *ppos);
#endif
