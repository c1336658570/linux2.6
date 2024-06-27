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
// seq_lseek函数定义：实现文件的lseek操作，用于定位序列文件的读取位置
loff_t seq_lseek(struct file *file, loff_t offset, int origin)
{
	// 从文件中获取seq_file结构
	struct seq_file *m = (struct seq_file *)file->private_data;
	// 初始化返回值为无效参数错误
	loff_t retval = -EINVAL;

	mutex_lock(&m->lock);	// 锁定seq_file结构，保证线程安全
	// 将文件版本同步到seq_file结构
	m->version = file->f_version;
	// 根据移动文件指针的起点进行不同处理
	switch (origin) {
		case 1:	// SEEK_CUR：当前位置基础上偏移
			offset += file->f_pos;	// 计算新的偏移位置
		case 0:	// SEEK_SET：文件开始处偏移
			if (offset < 0)	// 如果偏移量为负，则退出
				break;
			retval = offset;	// 预设返回值为新的偏移量
			// 如果新偏移量与当前读取位置不同
			if (offset != m->read_pos) {
				// 重新遍历直到不需要重试
				while ((retval=traverse(m, offset)) == -EAGAIN)
					;
				if (retval) {	// 如果遍历过程中出现错误
					/* with extreme prejudice... */
					file->f_pos = 0; // 重置文件位置
					m->read_pos = 0; // 重置读取位置
					m->version = 0; // 重置版本号
					m->index = 0; // 重置索引
					m->count = 0; // 重置计数器
				} else {
					// 更新读取位置为新偏移量
					m->read_pos = offset;
					// 更新文件位置，并设置返回值为新偏移量
					retval = file->f_pos = offset;
				}
			}
	}
	file->f_version = m->version;	// 同步版本号到文件
	mutex_unlock(&m->lock);	// 解锁
	return retval;	// 返回结果
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
/**
 *	seq_release - 释放与序列文件关联的结构。
 *	file: 相关的文件
 *	inode: file->f_path.dentry->d_inode 文件的inode节点
 *
 *	释放与序列文件关联的结构；如果你没有私有数据需要销毁，
 *	可以用作 ->f_op->release()。
 */
int seq_release(struct inode *inode, struct file *file)
{
	// 从文件中获取seq_file结构
	struct seq_file *m = (struct seq_file *)file->private_data;
	kfree(m->buf);	// 释放seq_file结构中的缓冲区
	kfree(m);	// 释放seq_file结构本身
	return 0;	// 返回0表示成功
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
/**
 *	seq_escape - 将字符串打印到缓冲区中，对某些字符进行转义
 *	m: 目标缓冲区
 *	s: 字符串
 *	esc: 需要转义的字符集
 *
 *	将字符串放入缓冲区，将 @esc 中的每个字符替换为常用的八进制转义序列。
 *	成功返回0，缓冲区溢出返回-1。
 */
int seq_escape(struct seq_file *m, const char *s, const char *esc)
{
	// 计算缓冲区的结束位置
	char *end = m->buf + m->size;
	char *p;	// 用于遍历缓冲区的指针
	char c;		// 当前处理的字符

		// 遍历字符串s
		for (p = m->buf + m->count; (c = *s) != '\0' && p < end; s++) {
		if (!strchr(esc, c)) {	// 如果当前字符不需要转义
			*p++ = c;	// 直接添加到缓冲区
			continue;
		}
		if (p + 3 < end) {	// 如果缓冲区有足够空间进行转义
			*p++ = '\\';	// 添加转义符
			*p++ = '0' + ((c & 0300) >> 6);	// 计算并添加字符的八进制表示
			*p++ = '0' + ((c & 070) >> 3);
			*p++ = '0' + (c & 07);
			continue;
		}
		m->count = m->size;	// 如果空间不足，设置count为size，表示缓冲区满
		return -1;	// 返回-1表示溢出
	}
	m->count = p - m->buf;	// 更新count为已写入的字符数
	return 0;	// 返回0表示成功
}
EXPORT_SYMBOL(seq_escape);

/**
 * seq_printf - 格式化输出字符串到序列文件
 * m: 目标序列文件
 * f: 格式化字符串
 * ...: 可变参数列表
 *
 * 如果缓冲区有足够空间，按照格式化字符串和参数输出到序列文件。
 * 成功返回0，失败（通常是因为缓冲区空间不足）返回-1。
 */
int seq_printf(struct seq_file *m, const char *f, ...)
{
	va_list args; // 定义可变参数列表
	int len; // 输出的长度

	// 如果缓冲区还有空间
	if (m->count < m->size) {
		// 初始化args指向第一个可变参数
		va_start(args, f);
		// 格式化输出到缓冲区
		len = vsnprintf(m->buf + m->count, m->size - m->count, f, args);
		va_end(args);	// 结束可变参数的获取
		if (m->count + len < m->size) {	// 如果输出后还有空间
			m->count += len;	// 更新count
			return 0;	// 返回0表示成功
		}
	}
	m->count = m->size;	// 如果空间不足，设置count为size
	return -1;	// 返回-1表示失败
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
/**
 *	mangle_path - 修改并复制路径到缓冲区开始位置
 *	s: 缓冲区起始位置
 *	p: 在上述缓冲区中路径的开始位置
 *	esc: 需要转义的字符集
 *
 *  从 @p 复制路径到 @s，替换 @esc 中的每个字符为常见的八进制转义。
 *  返回指向 @s 中最后一个写入字符之后的指针，如果失败则返回 NULL。
 */
char *mangle_path(char *s, char *p, char *esc)
{
	while (s <= p) {	// 当输出位置在输入位置之前时继续
		char c = *p++;	// 读取一个字符并移动指针
		if (!c) {	// 如果字符是字符串结束符
			return s;	// 返回当前位置（复制完成）
		} else if (!strchr(esc, c)) {	// 如果字符不在转义列表中
			*s++ = c;	// 直接复制字符
		} else if (s + 4 > p) {	// 如果缓冲区剩余空间不足以存储转义序列
			break;	// 终止循环
		} else {
			*s++ = '\\';	// 插入转义字符 '\\'
			// 插入字符的高两位的八进制表示
			*s++ = '0' + ((c & 0300) >> 6);
			// 插入字符的中间三位的八进制表示
			*s++ = '0' + ((c & 070) >> 3);
			// 插入字符的低三位的八进制表示
			*s++ = '0' + (c & 07);
		}
	}
	return NULL;	// 如果循环因空间不足终止，返回 NULL
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
/**
 * seq_path - 使用 seq_file 接口打印路径名
 * m: seq_file 句柄
 * path: 要打印的 struct path 结构体
 * esc: 输出中需要转义的字符集
 *
 * 返回 'path' 的绝对路径，该路径由 path 参数中的 dentry / mnt 对表示。
 */
int seq_path(struct seq_file *m, struct path *path, char *esc)
{
	char *buf; // 缓冲区指针
	size_t size = seq_get_buf(m, &buf); // 获取缓冲区并返回大小
	int res = -1; // 默认返回值设置为-1，表示失败

	if (size) {	// 如果获取到了缓冲区
		// 将路径转换为字符串形式
		char *p = d_path(path, buf, size);
		if (!IS_ERR(p)) {	// 如果转换成功
			// 修改路径，对需要转义的字符进行处理
			char *end = mangle_path(buf, p, esc);
			if (end)	// 如果修改成功
				// 计算结果字符串的长度
				res = end - buf;
		}
	}
	seq_commit(m, res);	// 提交缓冲区内容

	return res;
}
EXPORT_SYMBOL(seq_path);

/*
 * Same as seq_path, but relative to supplied root.
 *
 * root may be changed, see __d_path().
 */
/*
 * 与 seq_path 相同，但相对于提供的 root。
 *
 * root 可能会改变，参见 __d_path()。
 */
int seq_path_root(struct seq_file *m, struct path *path, struct path *root,
		  char *esc)
{
	char *buf; // 缓冲区指针
	size_t size = seq_get_buf(m, &buf); // 获取缓冲区并返回大小
	int res = -ENAMETOOLONG; // 默认返回值设置为路径名过长

	if (size) {	// 如果获取到了缓冲区
		char *p;

		spin_lock(&dcache_lock);	// 加锁，防止数据竞态
		// 获取相对于 root 的路径
		p = __d_path(path, root, buf, size);
		// 解锁
		spin_unlock(&dcache_lock);
		res = PTR_ERR(p);	// 获取错误码
		if (!IS_ERR(p)) {	// 如果路径获取成功
			// 修改路径，对需要转义的字符进行处理
			char *end = mangle_path(buf, p, esc);
			if (end)	// 如果修改成功
				res = end - buf;	// 计算结果字符串的长度
			else
				res = -ENAMETOOLONG;	// 修改失败，返回路径名过长错误
		}
	}
	seq_commit(m, res);	// 提交缓冲区内容

	return res < 0 ? res : 0;	// 如果有错误返回错误码，否则返回0
}

/*
 * returns the path of the 'dentry' from the root of its filesystem.
 */
/**
 * 返回从其文件系统根目录开始的 'dentry' 的路径。
 */
int seq_dentry(struct seq_file *m, struct dentry *dentry, char *esc)
{
	char *buf; // 缓冲区指针
	size_t size = seq_get_buf(m, &buf); // 获取缓冲区并返回大小
	int res = -1; // 默认返回值设置为-1，表示失败

	if (size) {	// 如果获取到了缓冲区
		// 获取dentry的路径字符串
		char *p = dentry_path(dentry, buf, size);
		if (!IS_ERR(p)) {	// 如果获取路径成功
			// 对路径中需要转义的字符进行处理
			char *end = mangle_path(buf, p, esc);
			if (end)	// 如果处理成功
				res = end - buf;	// 计算处理后的路径长度
		}
	}
	seq_commit(m, res);	// 提交缓冲区内容

	return res;
}

/**
 * 将位图打印到 seq_file。
 */
int seq_bitmap(struct seq_file *m, const unsigned long *bits,
				   unsigned int nr_bits)
{
	if (m->count < m->size) {	// 如果缓冲区还有空间
		// 打印位图到缓冲区
		int len = bitmap_scnprintf(m->buf + m->count,
				m->size - m->count, bits, nr_bits);
		if (m->count + len < m->size) {	// 如果打印后还有空间
			m->count += len;	// 更新已使用的缓冲区长度
			return 0;		// 返回0表示成功
		}
	}
	m->count = m->size;	// 缓冲区空间不足，设置count为size
	return -1;		// 返回-1表示失败
}
EXPORT_SYMBOL(seq_bitmap);

/**
 * 将位图以列表形式打印到 seq_file。
 */
int seq_bitmap_list(struct seq_file *m, const unsigned long *bits,
		unsigned int nr_bits)
{
	if (m->count < m->size) {	// 如果缓冲区还有空间
		// 以列表形式打印位图到缓冲区
		int len = bitmap_scnlistprintf(m->buf + m->count,
				m->size - m->count, bits, nr_bits);
		if (m->count + len < m->size) {	// 如果打印后还有空间
			m->count += len;	// 更新已使用的缓冲区长度
			return 0;		// 返回0表示成功
		}
	}
	m->count = m->size;	// 缓冲区空间不足，设置count为size
	return -1;	// 返回-1表示失败
}
EXPORT_SYMBOL(seq_bitmap_list);

// single_start: 用于开始序列文件的遍历，返回NULL表示没有内容或者已完成遍历。
static void *single_start(struct seq_file *p, loff_t *pos)
{
	// 只在位置为0时返回非NULL（实际是1），表示有内容需要处理。
	return NULL + (*pos == 0);
}

// single_next: 用于获取序列文件的下一条记录，在这种单条记录的情况下，始终返回NULL。
static void *single_next(struct seq_file *p, void *v, loff_t *pos)
{
	++*pos;		// 增加位置索引
	return NULL;	// 没有下一条记录
}

// single_stop: 结束遍历时的操作，这里没有任何操作。
static void single_stop(struct seq_file *p, void *v)
{
}

// single_open: 打开单条记录的序列文件。
int single_open(struct file *file, int (*show)(struct seq_file *, void *),
		void *data)
{
	// 分配序列操作结构
	struct seq_operations *op = kmalloc(sizeof(*op), GFP_KERNEL);
	int res = -ENOMEM;	// 默认返回内存不足错误

	if (op) {	// 如果内存分配成功
		op->start = single_start; // 设置开始函数
		op->next = single_next; // 设置下一记录函数
		op->stop = single_stop; // 设置结束函数
		op->show = show; // 设置显示函数
		res = seq_open(file, op); // 使用设置好的操作结构打开序列文件
		if (!res)	// 如果打开成功
			// 设置私有数据
			((struct seq_file *)file->private_data)->private = data;
		else
			kfree(op);	// 如果打开失败，释放操作结构内存
	}
	return res;
}
EXPORT_SYMBOL(single_open);

// single_release: 释放单条记录的序列文件
int single_release(struct inode *inode, struct file *file)
{
	// 获取序列操作结构
	const struct seq_operations *op = ((struct seq_file *)file->private_data)->op;
	int res = seq_release(inode, file);	// 调用通用的释放函数
	kfree(op);		// 释放序列操作结构
	return res;		// 返回结果
}
EXPORT_SYMBOL(single_release);

// seq_release_private: 释放带有私有数据的序列文件。
int seq_release_private(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;	// 获取序列文件结构

	kfree(seq->private); // 释放私有数据
	seq->private = NULL; // 将私有数据指针设置为NULL
	return seq_release(inode, file); // 调用通用的释放函数
}
EXPORT_SYMBOL(seq_release_private);

// 定义一个函数来打开序列文件，并为其分配私有数据空间。
void *__seq_open_private(struct file *f, const struct seq_operations *ops,
		int psize)
{
	int rc; // 用于存储返回代码
	void *private; // 私有数据指针
	struct seq_file *seq;

	// 分配并清零指定大小的内存
	private = kzalloc(psize, GFP_KERNEL);
	if (private == NULL)	// 如果内存分配失败
		goto out;	// 跳转到结束标签

	rc = seq_open(f, ops);	// 调用seq_open来打开序列文件
	if (rc < 0)		// 如果打开失败
		goto out_free;	// 跳转到释放内存标签

	seq = f->private_data; // 从文件结构中获取seq_file结构
	seq->private = private; // 设置私有数据
	return private; // 返回私有数据指针

out_free:
	kfree(private);	// 释放已分配的内存
out:
	return NULL;		// 返回NULL表示失败
}
EXPORT_SYMBOL(__seq_open_private);

// 一个简化版的seq_open，自动处理私有数据分配和释放。
int seq_open_private(struct file *filp, const struct seq_operations *ops,
		int psize)
{
	// 如果成功返回0，失败返回-ENOMEM
	return __seq_open_private(filp, ops, psize) ? 0 : -ENOMEM;
}
EXPORT_SYMBOL(seq_open_private);

// 向序列文件写入单个字符
int seq_putc(struct seq_file *m, char c)
{
	// 如果缓冲区还有空间
	if (m->count < m->size) {
		// 将字符写入缓冲区并更新位置
		m->buf[m->count++] = c;
		return 0;	// 返回0表示成功
	}
	return -1;	// 返回-1表示缓冲区已满
}
EXPORT_SYMBOL(seq_putc);

// 向序列文件写入字符串
int seq_puts(struct seq_file *m, const char *s)
{
	int len = strlen(s);	// 获取字符串长度
	if (m->count + len < m->size) {	// 如果缓冲区足够大
		memcpy(m->buf + m->count, s, len);	// 将字符串复制到缓冲区
		m->count += len;	// 更新写入位置
		return 0;	// 返回0表示成功
	}
	m->count = m->size;	// 如果空间不足，设置count为size
	return -1;	// 返回-1表示失败
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
/**
 * seq_write - 向缓冲区写入任意数据
 * seq: 指定要写入数据的缓冲区的seq_file
 * data: 数据地址
 * len: 字节数
 *
 * 成功返回0，否则返回非零。
 */
int seq_write(struct seq_file *seq, const void *data, size_t len)
{
	// 检查缓冲区是否有足够的空间
	if (seq->count + len < seq->size) {
		// 将数据复制到缓冲区
		memcpy(seq->buf + seq->count, data, len);
		seq->count += len;	// 更新缓冲区中已使用的字节数
		return 0;	// 成功时返回0
	}
	seq->count = seq->size;	// 如果空间不足，设置已使用字节数为缓冲区大小
	return -1;	// 返回-1表示失败
}
EXPORT_SYMBOL(seq_write);

/**
 * 从链表头开始遍历链表
 * head: 链表头
 * pos: 开始位置的偏移量
 *
 * 返回链表中对应位置的元素，如果位置无效则返回NULL。
 */
struct list_head *seq_list_start(struct list_head *head, loff_t pos)
{
	struct list_head *lh;

	// 遍历链表
	list_for_each(lh, head)
		if (pos-- == 0)	// 当计数减至0时，返回当前元素
			return lh;

	return NULL;	// 如果遍历完成仍未找到，返回NULL
}
EXPORT_SYMBOL(seq_list_start);

/**
 * 从链表头开始或指定位置开始遍历链表
 * head: 链表头
 * pos: 开始位置的偏移量
 *
 * 如果pos为0，直接返回链表头；否则调用seq_list_start返回指定位置的元素。
 */
struct list_head *seq_list_start_head(struct list_head *head, loff_t pos)
{
	if (!pos)	// 如果位置为0
		return head;	// 直接返回链表头

	// 否则调用seq_list_start获取前一个位置的元素
	return seq_list_start(head, pos - 1);
}
EXPORT_SYMBOL(seq_list_start_head);

/**
 * 获取链表中的下一个元素
 * v: 当前元素
 * head: 链表头
 * ppos: 位置指针
 *
 * 返回链表中的下一个元素，如果到达链表尾部则返回NULL。
 */
struct list_head *seq_list_next(void *v, struct list_head *head, loff_t *ppos)
{
	struct list_head *lh;

	// 获取当前元素的下一个元素
	lh = ((struct list_head *)v)->next;
	++*ppos;	// 位置增加
	// 如果下一个元素是链表头，则表示遍历完成，返回NULL
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
/**
 * seq_hlist_start - 开始遍历哈希链表
 * head: 哈希链表的头节点
 * pos: 开始遍历的位置
 *
 * 该函数在 seq_file->op->start() 被调用。
 */
struct hlist_node *seq_hlist_start(struct hlist_head *head, loff_t pos)
{
	struct hlist_node *node;	// 定义哈希链表节点指针

	hlist_for_each(node, head)	// 遍历哈希链表
		if (pos-- == 0)	// 当位置减到0时，返回当前节点
			return node;
	return NULL;			// 如果遍历完成仍未找到对应位置，返回NULL
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
/**
 * seq_hlist_start_head - 从哈希链表的头节点开始遍历
 * head: 哈希链表的头节点
 * pos: 开始遍历的位置
 *
 * 该函数在 seq_file->op->start() 被调用。如果你想在输出的顶部打印一个头部，调用这个函数。
 */
struct hlist_node *seq_hlist_start_head(struct hlist_head *head, loff_t pos)
{
	if (!pos)	// 如果位置为0
		return SEQ_START_TOKEN;	// 返回一个特殊的开始标记

	// 否则，调用seq_hlist_start获取前一个位置的节点
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
/**
 * seq_hlist_next - 移动到哈希链表的下一个位置
 * v: 当前迭代器位置
 * head: 哈希链表的头节点
 * ppos: 当前位置
 *
 * 该函数在 seq_file->op->next() 被调用。
 */
struct hlist_node *seq_hlist_next(void *v, struct hlist_head *head,
				  loff_t *ppos)
{
	// 从迭代器获取当前节点
	struct hlist_node *node = v;

	++*ppos;	// 位置递增
	if (v == SEQ_START_TOKEN)	// 如果当前位置是开始标记
		return head->first;	// 返回链表的第一个节点
	else
		return node->next;	// 否则返回当前节点的下一个节点
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
/**
 * seq_hlist_start_rcu - 从RCU保护的哈希链表开始迭代
 * head: 哈希链表的头节点
 * pos: 序列的开始位置
 *
 * 在 seq_file->op->start() 调用此函数。
 *
 * 这个列表遍历原语可以安全地与 _rcu 列表修改原语（如 hlist_add_head_rcu()）
 * 并发运行，只要遍历被 rcu_read_lock() 保护。
 */
struct hlist_node *seq_hlist_start_rcu(struct hlist_head *head,
				       loff_t pos)
{
	struct hlist_node *node;

	__hlist_for_each_rcu(node, head)	// 使用 RCU 机制安全遍历链表
		if (pos-- == 0)	// 当位置计数到0时返回当前节点
			return node;
	return NULL;	// 如果未找到节点，返回 NULL
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
/**
 * seq_hlist_start_head_rcu - 从RCU保护的哈希链表的头开始迭代
 * head: 哈希链表的头节点
 * pos: 序列的开始位置
 *
 * 在 seq_file->op->start() 调用此函数。如果你想在输出的顶部打印一个头部，调用这个函数。
 *
 * 这个列表遍历原语可以安全地与 _rcu 列表修改原语（如 hlist_add_head_rcu()）
 * 并发运行，只要遍历被 rcu_read_lock() 保护。
 */
struct hlist_node *seq_hlist_start_head_rcu(struct hlist_head *head,
					    loff_t pos)
{
	if (!pos)	// 如果位置为0
		return SEQ_START_TOKEN;	// 返回一个特殊的开始标记

	// 否则调用 seq_hlist_start_rcu 获取前一个位置的节点
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
/**
 * seq_hlist_next_rcu - 移动到RCU保护的哈希链表的下一个位置
 * v: 当前迭代器位置
 * head: 哈希链表的头节点
 * ppos: 当前位置
 *
 * 在 seq_file->op->next() 调用此函数。
 *
 * 这个列表遍历原语可以安全地与 _rcu 列表修改原语（如 hlist_add_head_rcu()）
 * 并发运行，只要遍历被 rcu_read_lock() 保护。
 */
struct hlist_node *seq_hlist_next_rcu(void *v,
				      struct hlist_head *head,
				      loff_t *ppos)
{
	struct hlist_node *node = v;

	++*ppos;	// 位置递增
	if (v == SEQ_START_TOKEN)
		// 如果是开始标记，返回列表的第一个节点
		return rcu_dereference(head->first);
	else
		// 否则返回下一个节点
		return rcu_dereference(node->next);
}
EXPORT_SYMBOL(seq_hlist_next_rcu);
