/*
 * linux/fs/seq_file.c
 *
 * helper functions for making synthetic files from sequences of records.
 * initial implementation -- AV, Oct 2001.
 */

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include <asm/uaccess.h>
#include <asm/page.h>

/**
 *	seq_open -	initialize sequential file
 *	@file: file we initialize
 *	@op: method table describing the sequence
 *
 *	seq_open() sets @file, associating it with a sequence described
 *	by @op.  @op->start() sets the iterator up and returns the first
 *	element of sequence. @op->stop() shuts it down.  @op->next()
 *	returns the next element of sequence.  @op->show() prints element
 *	into the buffer.  In case of error ->start() and ->next() return
 *	ERR_PTR(error).  In the end of sequence they return %NULL. ->show()
 *	returns 0 in case of success and negative number in case of error.
 *	Returning SEQ_SKIP means "discard this element and move on".
 */
/**
 * seq_open - 初始化序列文件
 * @file: 要初始化的文件
 * @op: 描述序列的方法表
 *
 * seq_open() 设置 @file，将其与由 @op 描述的序列关联。
 * @op->start() 设置迭代器并返回序列的第一个元素。
 * @op->stop() 用于关闭迭代器。
 * @op->next() 返回序列的下一个元素。
 * @op->show() 将元素输出到缓冲区。
 * 如果发生错误，->start() 和 ->next() 返回 ERR_PTR(error)。
 * 在序列结束时它们返回 %NULL。
 * ->show() 在成功时返回 0，在错误时返回负数。
 * 返回 SEQ_SKIP 表示“放弃此元素并继续”。
 */
// seq_open函数定义：用于打开一个序列文件
int seq_open(struct file *file, const struct seq_operations *op)
{
	// 获取文件的私有数据，此处用作seq_file结构
	struct seq_file *p = file->private_data;

	if (!p) {	// 如果私有数据为空，表示需要新建seq_file结构
		// 为seq_file结构分配内存
		p = kmalloc(sizeof(*p), GFP_KERNEL);
		if (!p)	// 如果内存分配失败
			return -ENOMEM;	// 返回内存不足错误码
		// 将新建的seq_file结构赋给文件的私有数据
		file->private_data = p;
	}
	memset(p, 0, sizeof(*p));	// 初始化seq_file结构的内存区域
	mutex_init(&p->lock);			// 初始化互斥锁
	p->op = op;	// 设置操作结构

	/*
	 * Wrappers around seq_open(e.g. swaps_open) need to be
	 * aware of this. If they set f_version themselves, they
	 * should call seq_open first and then set f_version.
	 */
	/*
	 * 封装函数需要注意seq_open的使用（例如swaps_open）。如果它们设置了f_version，
	 * 应该先调用seq_open然后再设置f_version。
	 */
	file->f_version = 0;

	/*
	 * seq_files support lseek() and pread().  They do not implement
	 * write() at all, but we clear FMODE_PWRITE here for historical
	 * reasons.
	 *
	 * If a client of seq_files a) implements file.write() and b) wishes to
	 * support pwrite() then that client will need to implement its own
	 * file.open() which calls seq_open() and then sets FMODE_PWRITE.
	 */
	/*
	 * seq_files支持lseek()和pread()。它们不实现write()，但出于历史原因，
	 * 我们在这里清除FMODE_PWRITE标志。
	 *
	 * 如果seq_files的客户端a) 实现了file.write()并且b) 希望支持pwrite()，
	 * 则该客户端需要实现自己的file.open()，调用seq_open()然后设置FMODE_PWRITE。
	 */
	// 清除文件模式中的FMODE_PWRITE位，表明不支持pwrite()
	file->f_mode &= ~FMODE_PWRITE;
	return 0;	// 返回成功
}
EXPORT_SYMBOL(seq_open);

// 函数traverse定义，用于遍历seq_file结构，并填充缓冲区
static int traverse(struct seq_file *m, loff_t offset)
{
	loff_t pos = 0, index; // pos记录当前位置，index用于记录条目索引
	int error = 0; // 错误码，默认为0
	void *p; // 通用指针，用于遍历

	m->version = 0;	// 初始化版本号
	index = 0;		// 初始化条目索引
	// 初始化count和from为0，表示缓冲区从0开始且无数据
	m->count = m->from = 0;
	if (!offset) {	// 如果偏移量为0，即从头开始
		// 设置序列文件的索引为当前索引
		m->index = index;
		return 0;	// 直接返回0，无需处理
	}
	if (!m->buf) {	// 如果缓冲区未初始化
		// 分配一个页面大小的缓冲区
		m->buf = kmalloc(m->size = PAGE_SIZE, GFP_KERNEL);
		if (!m->buf)	// 如果分配失败
			return -ENOMEM;	// 返回内存不足的错误
	}
	// 调用start操作，开始遍历
	p = m->op->start(m, &index);
	while (p) {	// 当节点有效时继续
		error = PTR_ERR(p);	// 获取指针错误码
		if (IS_ERR(p))	// 如果p是错误指针
			break;	// 中断循环
		// 调用show操作，处理当前节点
		error = m->op->show(m, p);
		if (error < 0)	// 如果处理出错
			break;	// 中断循环
		// 如果返回了错误但不是负值（比如警告）
		if (unlikely(error)) {
			error = 0;	// 重置错误码
			m->count = 0;	// 重置count，忽略当前输出
		}
		if (m->count == m->size)	// 如果缓冲区已满
			goto Eoverflow;	// 跳转到Eoverflow处理
		// 如果当前位置加上count超过了偏移量
		if (pos + m->count > offset) {
			m->from = offset - pos; // 计算from，确定输出起点
			m->count -= m->from; // 调整count，确定输出数量
			m->index = index; // 更新序列文件的索引
			break; // 结束循环
		}
		pos += m->count;	// 累加pos，准备下一次迭代
		m->count = 0;			// 重置count
		if (pos == offset) {	// 如果位置刚好等于偏移量
			index++;	// 索引递增
			m->index = index;	// 更新序列文件的索引
			break;	// 结束循环
		}
		// 获取下一个节点
		p = m->op->next(m, p, &index);
	}
	m->op->stop(m, p);	// 调用stop操作，结束遍历
	m->index = index;		// 更新序列文件的索引
	return error;				// 返回错误码

Eoverflow:	// 缓冲区溢出处理
	m->op->stop(m, p);	// 先调用stop操作
	kfree(m->buf);	// 释放原有缓冲区
	// 重新分配大小加倍的缓冲区
	m->buf = kmalloc(m->size <<= 1, GFP_KERNEL);
	// 如果分配失败返回内存不足，否则返回需重试
	return !m->buf ? -ENOMEM : -EAGAIN;
}

/**
 *	seq_read -	->read() method for sequential files.
 *	@file: the file to read from
 *	@buf: the buffer to read to
 *	@size: the maximum number of bytes to read
 *	@ppos: the current position in the file
 *
 *	Ready-made ->f_op->read()
 */
/**
 * seq_read - 序列文件的 ->read() 方法。
 * @file: 要从中读取的文件
 * @buf: 读取数据的缓冲区
 * @size: 最大读取字节数
 * @ppos: 文件中的当前位置
 *
 * 现成的 ->f_op->read() 方法
 */
ssize_t seq_read(struct file *file, char __user *buf, size_t size, loff_t *ppos)
{
	// 从文件中获取seq_file结构
	struct seq_file *m = (struct seq_file *)file->private_data;
	size_t copied = 0; // 已拷贝到用户空间的字节数
	loff_t pos; // 本地偏移变量
	size_t n; // 临时变量，用于计算拷贝长度
	void *p; // 用于遍历的指针
	int err = 0; // 错误代码

	mutex_lock(&m->lock);	// 锁定seq_file结构，保证线程安全

	/* Don't assume *ppos is where we left it */
	// 如果当前位置不是预期的读取位置
	if (unlikely(*ppos != m->read_pos)) {
		m->read_pos = *ppos;	// 更新读取位置
		// 重新遍历直到不需要重试
		while ((err = traverse(m, *ppos)) == -EAGAIN)
			;
		if (err) {	// 如果遍历过程中出现错误
			/* With prejudice... */
			m->read_pos = 0; // 重置读取位置
			m->version = 0; // 重置版本号
			m->index = 0; // 重置索引
			m->count = 0; // 重置计数器
			goto Done; // 跳转到完成标签
		}
	}

	/*
	 * seq_file->op->..m_start/m_stop/m_next may do special actions
	 * or optimisations based on the file->f_version, so we want to
	 * pass the file->f_version to those methods.
	 *
	 * seq_file->version is just copy of f_version, and seq_file
	 * methods can treat it simply as file version.
	 * It is copied in first and copied out after all operations.
	 * It is convenient to have it as  part of structure to avoid the
	 * need of passing another argument to all the seq_file methods.
	 */
	/*
	 * seq_file的op方法（m_start/m_stop/m_next）可能会基于file->f_version进行特殊操作或优化，
	 * 所以我们将file->f_version传递给这些方法。
	 * 
	 * seq_file->version仅是f_version的副本，seq_file方法可以简单地将其视为文件版本。
	 * 它在第一次复制进来，并在所有操作后复制出去。
	 * 将其作为结构的一部分，可以避免向所有seq_file方法传递另一个参数。
	 */
	m->version = file->f_version;	// 将文件版本同步到seq_file结构
	/* grab buffer if we didn't have one */
	if (!m->buf) {	// 如果没有缓冲区
		// 分配一页内存作为缓冲区
		m->buf = kmalloc(m->size = PAGE_SIZE, GFP_KERNEL);
		if (!m->buf)		// 如果分配失败
			goto Enomem;	// 跳转到内存不足处理
	}
	/* if not empty - flush it first */
	if (m->count) {	// 如果缓冲区中有数据
		n = min(m->count, size);	// 计算本次可拷贝的最大长度
		// 将数据拷贝到用户空间
		err = copy_to_user(buf, m->buf + m->from, n);
		if (err)	// 如果拷贝出错
			goto Efault;	// 跳转到错误处理
		m->count -= n;	// 更新缓冲区中的剩余数据长度
		m->from += n;		// 更新缓冲区的读取起始位置
		size -= n;			// 更新剩余需要读取的长度
		buf += n;				// 移动用户空间的缓冲区指针
		copied += n;		// 更新已拷贝的总长度
		if (!m->count)	// 如果缓冲区数据已经全部读取
			m->index++;		// 移动到下一个记录
		if (!size)			// 如果已经满足用户请求的长度
			goto Done;		// 跳转到完成标签
	}
	/* we need at least one record in buffer */
	pos = m->index;	// 设置当前索引
	// 通过start操作初始化遍历
	p = m->op->start(m, &pos);
	while (1) {
		err = PTR_ERR(p);	// 获取指针错误
		if (!p || IS_ERR(p))	// 如果指针无效或是错误指针
			break;	// 跳出循环
		// 调用show操作处理当前记录
		err = m->op->show(m, p);
		if (err < 0)	// 如果处理出现错误
			break;	// 跳出循环
		if (unlikely(err))	// 如果有错误但不致命
			m->count = 0;			// 重置计数器，忽略当前输出
		if (unlikely(!m->count)) {	// 如果没有有效数据
			// 获取下一个记录
			p = m->op->next(m, p, &pos);
			m->index = pos;	// 更新索引
			continue;	// 继续下一次循环
		}
		if (m->count < m->size)	// 如果缓冲区未满
			goto Fill;	// 跳转到填充数据标签
		m->op->stop(m, p);	// 调用stop操作停止遍历
		kfree(m->buf);	// 释放缓冲区
		// 重新分配更大的缓冲区
		m->buf = kmalloc(m->size <<= 1, GFP_KERNEL);
		if (!m->buf)	// 如果分配失败
			goto Enomem;	// 跳转到内存不足处理
		m->count = 0;		// 重置计数器
		m->version = 0;	// 重置版本号
		pos = m->index;	// 重置位置
		p = m->op->start(m, &pos);	// 重新开始遍历
	}
	m->op->stop(m, p);	// 停止遍历
	m->count = 0;				// 重置计数器
	goto Done;					// 跳转到完成标签
Fill:
	/* they want more? let's try to get some more */
	while (m->count < size) {	// 当缓冲区数据少于请求长度时
		// 记录当前偏移
		size_t offs = m->count;
		loff_t next = pos;	// 记录下一个位置
		// 获取下一个记录
		p = m->op->next(m, p, &next);
		if (!p || IS_ERR(p)) {	// 如果指针无效或是错误指针
			err = PTR_ERR(p);			// 获取错误码
			break;		// 跳出循环
		}
		err = m->op->show(m, p);	// 处理记录
		// 如果缓冲区已满或有错误
		if (m->count == m->size || err) {
			m->count = offs;	// 恢复之前的偏移
			if (likely(err <= 0))	// 如果错误不是致命的
				break;	// 跳出循环
		}
		pos = next;	// 更新位置
	}

	m->op->stop(m, p); // 停止遍历
	n = min(m->count, size); // 计算本次可拷贝的最大长度
	err = copy_to_user(buf, m->buf, n); // 将数据拷贝到用户空间
	if (err) // 如果拷贝出错
		goto Efault; // 跳转到错误处理
	copied += n; // 更新已拷贝的总长度
	m->count -= n; // 更新缓冲区中的剩余数据长度
	if (m->count) // 如果缓冲区中还有数据
		m->from = n; // 更新缓冲区的读取起始位置
	else
		pos++; // 否则移动到下一个记录
	m->index = pos; // 更新索引

Done:
	if (!copied)	// 如果没有数据被拷贝
		copied = err;	// 返回错误码
	else {
		*ppos += copied;	// 更新文件偏移
		m->read_pos += copied;	// 更新读取位置
	}
	file->f_version = m->version;	// 同步版本号到文件
	mutex_unlock(&m->lock);	// 解锁
	return copied;		// 返回拷贝的字节数
Enomem:
	err = -ENOMEM;		// 设置内存不足错误码
	goto Done;				// 跳转到完成标签
Efault:
	err = -EFAULT;		// 设置拷贝错误码
	goto Done;				// 跳转到完成标签
}
EXPORT_SYMBOL(seq_read);

/**
 *	seq_lseek -	->llseek() method for sequential files.
 *	@file: the file in question
 *	@offset: new position
 *	@origin: 0 for absolute, 1 for relative position
 *
 *	Ready-made ->f_op->llseek()
 */
/**
 * seq_lseek - 序列文件的 ->llseek() 方法。
 * @file: 相关文件
 * @offset: 新位置
 * @origin: 如果是0表示绝对位置，如果是1表示相对位置
 *
 * 现成的 ->f_op->llseek() 方法
 */

loff_t seq_lseek(struct file *file, loff_t offset, int origin)
{
	struct seq_file *m = (struct seq_file *)file->private_data;
	loff_t retval = -EINVAL;

	mutex_lock(&m->lock);
	m->version = file->f_version;
	switch (origin) {
		case 1:
			offset += file->f_pos;
		case 0:
			if (offset < 0)
				break;
			retval = offset;
			if (offset != m->read_pos) {
				while ((retval=traverse(m, offset)) == -EAGAIN)
					;
				if (retval) {
					/* with extreme prejudice... */
					file->f_pos = 0;
					m->read_pos = 0;
					m->version = 0;
					m->index = 0;
					m->count = 0;
				} else {
					m->read_pos = offset;
					retval = file->f_pos = offset;
				}
			}
	}
	file->f_version = m->version;
	mutex_unlock(&m->lock);
	return retval;
}
EXPORT_SYMBOL(seq_lseek);

/**
 *	seq_release -	free the structures associated with sequential file.
 *	@file: file in question
 *	@inode: file->f_path.dentry->d_inode
 *
 *	Frees the structures associated with sequential file; can be used
 *	as ->f_op->release() if you don't have private data to destroy.
 */
int seq_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = (struct seq_file *)file->private_data;
	kfree(m->buf);
	kfree(m);
	return 0;
}
EXPORT_SYMBOL(seq_release);

/**
 *	seq_escape -	print string into buffer, escaping some characters
 *	@m:	target buffer
 *	@s:	string
 *	@esc:	set of characters that need escaping
 *
 *	Puts string into buffer, replacing each occurrence of character from
 *	@esc with usual octal escape.  Returns 0 in case of success, -1 - in
 *	case of overflow.
 */
int seq_escape(struct seq_file *m, const char *s, const char *esc)
{
	char *end = m->buf + m->size;
        char *p;
	char c;

        for (p = m->buf + m->count; (c = *s) != '\0' && p < end; s++) {
		if (!strchr(esc, c)) {
			*p++ = c;
			continue;
		}
		if (p + 3 < end) {
			*p++ = '\\';
			*p++ = '0' + ((c & 0300) >> 6);
			*p++ = '0' + ((c & 070) >> 3);
			*p++ = '0' + (c & 07);
			continue;
		}
		m->count = m->size;
		return -1;
        }
	m->count = p - m->buf;
        return 0;
}
EXPORT_SYMBOL(seq_escape);

int seq_printf(struct seq_file *m, const char *f, ...)
{
	va_list args;
	int len;

	if (m->count < m->size) {
		va_start(args, f);
		len = vsnprintf(m->buf + m->count, m->size - m->count, f, args);
		va_end(args);
		if (m->count + len < m->size) {
			m->count += len;
			return 0;
		}
	}
	m->count = m->size;
	return -1;
}
EXPORT_SYMBOL(seq_printf);

/**
 *	mangle_path -	mangle and copy path to buffer beginning
 *	@s: buffer start
 *	@p: beginning of path in above buffer
 *	@esc: set of characters that need escaping
 *
 *      Copy the path from @p to @s, replacing each occurrence of character from
 *      @esc with usual octal escape.
 *      Returns pointer past last written character in @s, or NULL in case of
 *      failure.
 */
char *mangle_path(char *s, char *p, char *esc)
{
	while (s <= p) {
		char c = *p++;
		if (!c) {
			return s;
		} else if (!strchr(esc, c)) {
			*s++ = c;
		} else if (s + 4 > p) {
			break;
		} else {
			*s++ = '\\';
			*s++ = '0' + ((c & 0300) >> 6);
			*s++ = '0' + ((c & 070) >> 3);
			*s++ = '0' + (c & 07);
		}
	}
	return NULL;
}
EXPORT_SYMBOL(mangle_path);

/**
 * seq_path - seq_file interface to print a pathname
 * @m: the seq_file handle
 * @path: the struct path to print
 * @esc: set of characters to escape in the output
 *
 * return the absolute path of 'path', as represented by the
 * dentry / mnt pair in the path parameter.
 */
int seq_path(struct seq_file *m, struct path *path, char *esc)
{
	char *buf;
	size_t size = seq_get_buf(m, &buf);
	int res = -1;

	if (size) {
		char *p = d_path(path, buf, size);
		if (!IS_ERR(p)) {
			char *end = mangle_path(buf, p, esc);
			if (end)
				res = end - buf;
		}
	}
	seq_commit(m, res);

	return res;
}
EXPORT_SYMBOL(seq_path);

/*
 * Same as seq_path, but relative to supplied root.
 *
 * root may be changed, see __d_path().
 */
int seq_path_root(struct seq_file *m, struct path *path, struct path *root,
		  char *esc)
{
	char *buf;
	size_t size = seq_get_buf(m, &buf);
	int res = -ENAMETOOLONG;

	if (size) {
		char *p;

		spin_lock(&dcache_lock);
		p = __d_path(path, root, buf, size);
		spin_unlock(&dcache_lock);
		res = PTR_ERR(p);
		if (!IS_ERR(p)) {
			char *end = mangle_path(buf, p, esc);
			if (end)
				res = end - buf;
			else
				res = -ENAMETOOLONG;
		}
	}
	seq_commit(m, res);

	return res < 0 ? res : 0;
}

/*
 * returns the path of the 'dentry' from the root of its filesystem.
 */
int seq_dentry(struct seq_file *m, struct dentry *dentry, char *esc)
{
	char *buf;
	size_t size = seq_get_buf(m, &buf);
	int res = -1;

	if (size) {
		char *p = dentry_path(dentry, buf, size);
		if (!IS_ERR(p)) {
			char *end = mangle_path(buf, p, esc);
			if (end)
				res = end - buf;
		}
	}
	seq_commit(m, res);

	return res;
}

int seq_bitmap(struct seq_file *m, const unsigned long *bits,
				   unsigned int nr_bits)
{
	if (m->count < m->size) {
		int len = bitmap_scnprintf(m->buf + m->count,
				m->size - m->count, bits, nr_bits);
		if (m->count + len < m->size) {
			m->count += len;
			return 0;
		}
	}
	m->count = m->size;
	return -1;
}
EXPORT_SYMBOL(seq_bitmap);

int seq_bitmap_list(struct seq_file *m, const unsigned long *bits,
		unsigned int nr_bits)
{
	if (m->count < m->size) {
		int len = bitmap_scnlistprintf(m->buf + m->count,
				m->size - m->count, bits, nr_bits);
		if (m->count + len < m->size) {
			m->count += len;
			return 0;
		}
	}
	m->count = m->size;
	return -1;
}
EXPORT_SYMBOL(seq_bitmap_list);

static void *single_start(struct seq_file *p, loff_t *pos)
{
	return NULL + (*pos == 0);
}

static void *single_next(struct seq_file *p, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void single_stop(struct seq_file *p, void *v)
{
}

int single_open(struct file *file, int (*show)(struct seq_file *, void *),
		void *data)
{
	struct seq_operations *op = kmalloc(sizeof(*op), GFP_KERNEL);
	int res = -ENOMEM;

	if (op) {
		op->start = single_start;
		op->next = single_next;
		op->stop = single_stop;
		op->show = show;
		res = seq_open(file, op);
		if (!res)
			((struct seq_file *)file->private_data)->private = data;
		else
			kfree(op);
	}
	return res;
}
EXPORT_SYMBOL(single_open);

int single_release(struct inode *inode, struct file *file)
{
	const struct seq_operations *op = ((struct seq_file *)file->private_data)->op;
	int res = seq_release(inode, file);
	kfree(op);
	return res;
}
EXPORT_SYMBOL(single_release);

int seq_release_private(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;

	kfree(seq->private);
	seq->private = NULL;
	return seq_release(inode, file);
}
EXPORT_SYMBOL(seq_release_private);

void *__seq_open_private(struct file *f, const struct seq_operations *ops,
		int psize)
{
	int rc;
	void *private;
	struct seq_file *seq;

	private = kzalloc(psize, GFP_KERNEL);
	if (private == NULL)
		goto out;

	rc = seq_open(f, ops);
	if (rc < 0)
		goto out_free;

	seq = f->private_data;
	seq->private = private;
	return private;

out_free:
	kfree(private);
out:
	return NULL;
}
EXPORT_SYMBOL(__seq_open_private);

int seq_open_private(struct file *filp, const struct seq_operations *ops,
		int psize)
{
	return __seq_open_private(filp, ops, psize) ? 0 : -ENOMEM;
}
EXPORT_SYMBOL(seq_open_private);

int seq_putc(struct seq_file *m, char c)
{
	if (m->count < m->size) {
		m->buf[m->count++] = c;
		return 0;
	}
	return -1;
}
EXPORT_SYMBOL(seq_putc);

int seq_puts(struct seq_file *m, const char *s)
{
	int len = strlen(s);
	if (m->count + len < m->size) {
		memcpy(m->buf + m->count, s, len);
		m->count += len;
		return 0;
	}
	m->count = m->size;
	return -1;
}
EXPORT_SYMBOL(seq_puts);

/**
 * seq_write - write arbitrary data to buffer
 * @seq: seq_file identifying the buffer to which data should be written
 * @data: data address
 * @len: number of bytes
 *
 * Return 0 on success, non-zero otherwise.
 */
int seq_write(struct seq_file *seq, const void *data, size_t len)
{
	if (seq->count + len < seq->size) {
		memcpy(seq->buf + seq->count, data, len);
		seq->count += len;
		return 0;
	}
	seq->count = seq->size;
	return -1;
}
EXPORT_SYMBOL(seq_write);

struct list_head *seq_list_start(struct list_head *head, loff_t pos)
{
	struct list_head *lh;

	list_for_each(lh, head)
		if (pos-- == 0)
			return lh;

	return NULL;
}
EXPORT_SYMBOL(seq_list_start);

struct list_head *seq_list_start_head(struct list_head *head, loff_t pos)
{
	if (!pos)
		return head;

	return seq_list_start(head, pos - 1);
}
EXPORT_SYMBOL(seq_list_start_head);

struct list_head *seq_list_next(void *v, struct list_head *head, loff_t *ppos)
{
	struct list_head *lh;

	lh = ((struct list_head *)v)->next;
	++*ppos;
	return lh == head ? NULL : lh;
}
EXPORT_SYMBOL(seq_list_next);

/**
 * seq_hlist_start - start an iteration of a hlist
 * @head: the head of the hlist
 * @pos:  the start position of the sequence
 *
 * Called at seq_file->op->start().
 */
struct hlist_node *seq_hlist_start(struct hlist_head *head, loff_t pos)
{
	struct hlist_node *node;

	hlist_for_each(node, head)
		if (pos-- == 0)
			return node;
	return NULL;
}
EXPORT_SYMBOL(seq_hlist_start);

/**
 * seq_hlist_start_head - start an iteration of a hlist
 * @head: the head of the hlist
 * @pos:  the start position of the sequence
 *
 * Called at seq_file->op->start(). Call this function if you want to
 * print a header at the top of the output.
 */
struct hlist_node *seq_hlist_start_head(struct hlist_head *head, loff_t pos)
{
	if (!pos)
		return SEQ_START_TOKEN;

	return seq_hlist_start(head, pos - 1);
}
EXPORT_SYMBOL(seq_hlist_start_head);

/**
 * seq_hlist_next - move to the next position of the hlist
 * @v:    the current iterator
 * @head: the head of the hlist
 * @ppos: the current position
 *
 * Called at seq_file->op->next().
 */
struct hlist_node *seq_hlist_next(void *v, struct hlist_head *head,
				  loff_t *ppos)
{
	struct hlist_node *node = v;

	++*ppos;
	if (v == SEQ_START_TOKEN)
		return head->first;
	else
		return node->next;
}
EXPORT_SYMBOL(seq_hlist_next);

/**
 * seq_hlist_start_rcu - start an iteration of a hlist protected by RCU
 * @head: the head of the hlist
 * @pos:  the start position of the sequence
 *
 * Called at seq_file->op->start().
 *
 * This list-traversal primitive may safely run concurrently with
 * the _rcu list-mutation primitives such as hlist_add_head_rcu()
 * as long as the traversal is guarded by rcu_read_lock().
 */
struct hlist_node *seq_hlist_start_rcu(struct hlist_head *head,
				       loff_t pos)
{
	struct hlist_node *node;

	__hlist_for_each_rcu(node, head)
		if (pos-- == 0)
			return node;
	return NULL;
}
EXPORT_SYMBOL(seq_hlist_start_rcu);

/**
 * seq_hlist_start_head_rcu - start an iteration of a hlist protected by RCU
 * @head: the head of the hlist
 * @pos:  the start position of the sequence
 *
 * Called at seq_file->op->start(). Call this function if you want to
 * print a header at the top of the output.
 *
 * This list-traversal primitive may safely run concurrently with
 * the _rcu list-mutation primitives such as hlist_add_head_rcu()
 * as long as the traversal is guarded by rcu_read_lock().
 */
struct hlist_node *seq_hlist_start_head_rcu(struct hlist_head *head,
					    loff_t pos)
{
	if (!pos)
		return SEQ_START_TOKEN;

	return seq_hlist_start_rcu(head, pos - 1);
}
EXPORT_SYMBOL(seq_hlist_start_head_rcu);

/**
 * seq_hlist_next_rcu - move to the next position of the hlist protected by RCU
 * @v:    the current iterator
 * @head: the head of the hlist
 * @ppos: the current position
 *
 * Called at seq_file->op->next().
 *
 * This list-traversal primitive may safely run concurrently with
 * the _rcu list-mutation primitives such as hlist_add_head_rcu()
 * as long as the traversal is guarded by rcu_read_lock().
 */
struct hlist_node *seq_hlist_next_rcu(void *v,
				      struct hlist_head *head,
				      loff_t *ppos)
{
	struct hlist_node *node = v;

	++*ppos;
	if (v == SEQ_START_TOKEN)
		return rcu_dereference(head->first);
	else
		return rcu_dereference(node->next);
}
EXPORT_SYMBOL(seq_hlist_next_rcu);
