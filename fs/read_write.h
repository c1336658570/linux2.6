/*
 * This file is only for sharing some helpers from read_write.c with compat.c.
 * Don't use anywhere else.
 */
/*
 * 此文件仅用于从 read_write.c 分享一些助手函数给 compat.c。
 * 不要在其他地方使用。
 * 中文注释：此文件仅用于将 read_write.c 中的一些辅助函数共享给 compat.c。
 * 不要在其他地方使用。
 */

/* 定义一个函数指针类型 io_fn_t，用于文件的读写操作。
 * 参数包括：
 *   - struct file *: 文件指针，指向需要进行操作的文件。
 *   - char __user *: 用户空间的数据缓冲区，用于存放读取的数据或从中写入数据。
 *   - size_t: 操作的数据长度。
 *   - loff_t *: 文件操作的起始偏移量的指针。
 * 函数返回类型为 ssize_t，表示实际读写的字节数，负值表示错误。
 */
typedef ssize_t (*io_fn_t)(struct file *, char __user *, size_t, loff_t *);
/* 定义一个函数指针类型 iov_fn_t，用于基于 iovec 结构体的向量化读写操作。
 * 参数包括：
 *   - struct kiocb *: 异步 I/O 控制块的指针，通常包含 I/O 操作的上下文信息。
 *   - const struct iovec *: 指向一组 iovec 结构的指针，每个 iovec 描述一个内存区域。
 *   - unsigned long: iovec 结构的数量，即内存区域的数量。
 *   - loff_t: 文件操作的起始偏移量。
 * 函数返回类型为 ssize_t，表示实际处理的数据总长度，负值表示错误。
 */
typedef ssize_t (*iov_fn_t)(struct kiocb *, const struct iovec *,
		unsigned long, loff_t);

/* 定义一个函数 do_sync_readv_writev，用于执行同步的向量化读写操作。
 * 参数包括：
 *   - struct file *filp: 目标文件的文件指针。
 *   - const struct iovec *iov: 指向 iovec 数组的指针，描述了多个待读写的内存区域。
 *   - unsigned long nr_segs: iovec 数组中元素的数量。
 *   - size_t len: 预计读写的总数据长度。
 *   - loff_t *ppos: 指向文件中起始偏移量的指针。
 *   - iov_fn_t fn: 指向具体执行读写操作的函数的指针。
 * 函数返回类型为 ssize_t，表示实际读写的数据总长度，负值表示错误。
 */
ssize_t do_sync_readv_writev(struct file *filp, const struct iovec *iov,
		unsigned long nr_segs, size_t len, loff_t *ppos, iov_fn_t fn);
/* 定义一个函数 do_loop_readv_writev，用于循环执行基于单个 I/O 缓冲区的读写操作。
 * 参数包括：
 *   - struct file *filp: 目标文件的文件指针。
 *   - struct iovec *iov: 指向单个 iovec 结构的指针，描述待读写的内存区域。
 *   - unsigned long nr_segs: iov 结构的数量，通常为 1。
 *   - loff_t *ppos: 指向文件中起始偏移量的指针。
 *   - io_fn_t fn: 指向具体执行读写操作的函数的指针。
 * 函数返回类型为 ssize_t，表示实际读写的数据总长度，负值表示错误。
 */
ssize_t do_loop_readv_writev(struct file *filp, struct iovec *iov,
		unsigned long nr_segs, loff_t *ppos, io_fn_t fn);
