/*
 *	fs/libfs.c
 *	Library for filesystems writers.
 */

#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/mount.h>
#include <linux/vfs.h>
#include <linux/mutex.h>
#include <linux/exportfs.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>

#include <asm/uaccess.h>

// 定义一个名为simple_getattr的函数，接受三个参数：mnt（文件系统挂载点的指针），dentry（目录项的指针），stat（文件状态结构的指针）
int simple_getattr(struct vfsmount *mnt, struct dentry *dentry,
		   struct kstat *stat)
{
	// 从目录项dentry中获取关联的inode，并赋值给inode指针
	struct inode *inode = dentry->d_inode;
	// 调用generic_fillattr函数，使用inode中的信息填充stat结构
	generic_fillattr(inode, stat);
	// 设置stat结构中的blocks字段。blocks是文件所占用的块数。
	// inode->i_mapping->nrpages是文件页的数量，PAGE_CACHE_SHIFT - 9用于将页数转换为块数（通常一页大小是4KB，一个块大小是512字节）
	stat->blocks = inode->i_mapping->nrpages << (PAGE_CACHE_SHIFT - 9);

	// 函数返回0，表示成功完成
	return 0;
}

// 定义一个名为simple_statfs的函数，接受两个参数：dentry（目录项的指针），buf（文件系统统计信息结构的指针）
int simple_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	// 将文件系统类型标识符（通常是魔数，用于唯一标识文件系统类型）赋值给buf->f_type
	// dentry->d_sb指向包含此目录项的超级块结构，s_magic是文件系统类型的标识符
	buf->f_type = dentry->d_sb->s_magic;
	// 设置文件系统的块大小为PAGE_CACHE_SIZE（页面缓存大小）
	// 在Linux中，PAGE_CACHE_SIZE通常是系统页面大小，如4KB
	buf->f_bsize = PAGE_CACHE_SIZE;
	// 设置文件系统支持的最大文件名长度为NAME_MAX
	// NAME_MAX是系统定义的最大文件名长度，通常是255
	buf->f_namelen = NAME_MAX;
	// 函数返回0，表示成功完成
	return 0;
}

/*
 * Retaining negative dentries for an in-memory filesystem just wastes
 * memory and lookup time: arrange for them to be deleted immediately.
 */
/*
 * 为了节省内存和查找时间，应该立即删除负（不存在的文件）目录项。
 * 保留负目录项对于内存文件系统来说只是浪费内存和查找时间：安排它们被立即删除。
 */
static int simple_delete_dentry(struct dentry *dentry)
{
	// 总是返回1，表明目录项总是准备好被删除
	return 1;
}

/*
 * Lookup the data. This is trivial - if the dentry didn't already
 * exist, we know it is negative.  Set d_op to delete negative dentries.
 */
/*
 * 查找数据。这是非常简单的 - 如果目录项不存在，我们知道它是负的。
 * 设置d_op为删除负目录项。
 * 查找数据。这个操作是非常简单的——如果目录项不存在，我们知道它是负的。设置d_op以删除负目录项。
 */
// 用于处理文件查找操作，在简单的内存文件系统中，如果文件不存在，则会立即被认定为负（不存在的）并通过simple_delete_dentry函数标记为可删除。
struct dentry *simple_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd)
{
	// 定义一个静态的目录项操作结构，其中包含了一个删除函数simple_delete_dentry
	static const struct dentry_operations simple_dentry_operations = {
		.d_delete = simple_delete_dentry,
	};

	// 如果目录项的名字长度超过系统允许的最大长度NAME_MAX，则返回错误码
	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);  // 返回错误指针，表示文件名过长

	// 将自定义的目录项操作赋值给目录项，以便处理删除操作
	dentry->d_op = &simple_dentry_operations;

	// 将目录项添加到目录项缓存中，但不关联任何实际的inode
	d_add(dentry, NULL);

	// 返回NULL，表示目录项查找结束但没有找到实际的文件
	return NULL;
}

// 一个用于同步文件的简单函数，通常用于确保文件数据同步到存储设备
// 这里的实现仅返回0，表示成功完成，没有实际的同步操作
int simple_sync_file(struct file * file, struct dentry *dentry, int datasync)
{
	return 0;
}

// 打开目录文件的函数，用于目录的遍历或操作
// 为打开的文件分配一个指向当前目录项的指针，用于后续操作
int dcache_dir_open(struct inode *inode, struct file *file)
{
	// 定义一个静态的qstr结构，代表目录名，这里是当前目录"."
	static struct qstr cursor_name = {.len = 1, .name = "."};

	// 使用d_alloc为文件分配一个新的dentry结构，这个dentry指向当前目录"."
	// file->f_path.dentry 是文件当前的目录项
	file->private_data = d_alloc(file->f_path.dentry, &cursor_name);

	// 如果分配成功，返回0，否则返回-ENOMEM（内存不足错误）
	return file->private_data ? 0 : -ENOMEM;
}

// 关闭目录文件的函数
// 释放在打开时分配的目录项资源
int dcache_dir_close(struct inode *inode, struct file *file)
{
	// 释放之前在dcache_dir_open中分配的dentry
	dput(file->private_data);
	return 0;
}

// 处理文件指针在目录文件中的定位操作。接受三个参数：file指向文件的指针，offset为偏移量，origin为起始位置
loff_t dcache_dir_lseek(struct file *file, loff_t offset, int origin)
{
	// 对文件所属的inode进行加锁，防止在进行操作时数据被改变
	mutex_lock(&file->f_path.dentry->d_inode->i_mutex);

	// 根据起始位置origin决定如何处理偏移量
	switch (origin) {
		case 1: // SEEK_CUR，当前位置加上offset
			offset += file->f_pos;  // file->f_pos是当前文件的位置
		case 0: // SEEK_SET，将文件指针设置到offset指定的位置
			if (offset >= 0)  // 确保偏移量不是负数
				break;
		default:  // 如果origin不是已知的值，则解锁并返回错误
			mutex_unlock(&file->f_path.dentry->d_inode->i_mutex);
			return -EINVAL;  // 无效的参数
	}

	// 如果新的偏移位置与当前文件位置不同，则更新文件位置
	if (offset != file->f_pos) {
		file->f_pos = offset;  // 更新文件位置
		// 如果新位置大于等于2，需要调整目录项的位置
		if (file->f_pos >= 2) {
			struct list_head *p;
			struct dentry *cursor = file->private_data;  // 获取之前保存的目录项
			loff_t n = file->f_pos - 2;  // 计算新的偏移量
			// 对目录项列表加锁
			spin_lock(&dcache_lock);
			// 从列表中删除当前cursor所在的位置
			list_del(&cursor->d_u.d_child);
			// 遍历目录项列表到新的位置
			p = file->f_path.dentry->d_subdirs.next;
			while (n && p != &file->f_path.dentry->d_subdirs) {
				struct dentry *next;
				next = list_entry(p, struct dentry, d_u.d_child);
				// 确保next是有效的，未被删除且有对应的inode
				if (!d_unhashed(next) && next->d_inode)
					n--;
				p = p->next;
			}

			// 在新位置重新插入cursor
			list_add_tail(&cursor->d_u.d_child, p);
			// 解锁目录项列表
			spin_unlock(&dcache_lock);
		}
	}

	// 解锁inode
	mutex_unlock(&file->f_path.dentry->d_inode->i_mutex);
	// 返回新的文件位置
	return offset;
}

/* Relationship between i_mode and the DT_xxx types */
/* i_mode与DT_xxx类型之间的关系 */
/* 在inode结构体中的i_mode字段和DT_xxx类型之间的关系 */
// 用于从文件的inode结构体中获取文件类型。
static inline unsigned char dt_type(struct inode *inode)
{
	// 从inode结构体的i_mode字段中提取文件类型
	// i_mode字段的高12位标识了文件类型
	// 通过右移12位，将类型位移动到最低位，然后通过与15（二进制1111）进行按位与操作，提取出前4位来得到文件类型的数值
	return (inode->i_mode >> 12) & 15;
}

/*
 * Directory is locked and all positive dentries in it are safe, since
 * for ramfs-type trees they can't go away without unlink() or rmdir(),
 * both impossible due to the lock on directory.
 */
/*
 * 目录已锁定并且其中所有正的目录项都是安全的，因为对于ramfs类型的树来说，
 * 它们不可能在没有unlink()或rmdir()的情况下消失，这两者都由于目录上的锁而变得不可能。
 */

// 定义dcache_readdir函数，用于从目录中读取条目信息，是一个目录遍历函数的实现。接受三个参数：filp指向打开的文件，dirent为目录项的数据结构，filldir是一个回调函数
int dcache_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
	struct dentry *dentry = filp->f_path.dentry; // 获取文件指针对应的目录项
	struct dentry *cursor = filp->private_data; // 获取与文件关联的私有数据，这里是一个游标目录项
	struct list_head *p, *q = &cursor->d_u.d_child; // p和q用于遍历目录项
	ino_t ino; // 用于存储inode编号
	int i = filp->f_pos; // 当前的文件位置

	// 根据当前的文件位置选择操作
	switch (i) {
		case 0: // 处理"."，即当前目录
			ino = dentry->d_inode->i_ino; // 获取当前目录的inode编号
			if (filldir(dirent, ".", 1, i, ino, DT_DIR) < 0) // 使用filldir回调填充目录信息，如果失败则终止
				break;
			filp->f_pos++; // 移动文件位置
			i++;
			/* fallthrough */
		case 1: // 处理".."，即父目录
			ino = parent_ino(dentry); // 获取父目录的inode编号
			if (filldir(dirent, "..", 2, i, ino, DT_DIR) < 0) // 使用filldir回调填充父目录信息，如果失败则终止
				break;
			filp->f_pos++; // 移动文件位置
			i++;
			/* fallthrough */
		default: // 处理其他目录项
			spin_lock(&dcache_lock); // 锁定目录项缓存
			if (filp->f_pos == 2)
				list_move(q, &dentry->d_subdirs); // 将游标移动到子目录项的开始

			// 遍历目录项
			for (p = q->next; p != &dentry->d_subdirs; p = p->next) {
				struct dentry *next;
				next = list_entry(p, struct dentry, d_u.d_child); // 获取下一个目录项
				if (d_unhashed(next) || !next->d_inode) // 如果目录项无效或没有对应的inode，则跳过
					continue;

				spin_unlock(&dcache_lock); // 解锁目录项缓存
				if (filldir(dirent, next->d_name.name, 
								next->d_name.len, filp->f_pos, 
								next->d_inode->i_ino, 
								dt_type(next->d_inode)) < 0) // 使用filldir回调填充目录项信息，如果失败则返回
					return 0;
				spin_lock(&dcache_lock); // 重新锁定目录项缓存
				/* next is still alive */
				// 确保目录项依然有效
				list_move(q, p); // 更新游标位置
				p = q;
				filp->f_pos++; // 更新文件位置
			}
			spin_unlock(&dcache_lock); // 最终解锁目录项缓存
	}
	return 0; // 返回0表示成功完成
}

// 定义一个读取目录的通用函数，当尝试读取目录内容为普通文件时返回错误
ssize_t generic_read_dir(struct file *filp, char __user *buf, size_t siz, loff_t *ppos)
{
	return -EISDIR; // 返回-EISDIR错误，表明目标是一个目录，不支持普通读操作
}

// 定义目录操作相关的文件操作结构体
const struct file_operations simple_dir_operations = {
	.open       = dcache_dir_open,    // 打开目录的函数
	.release    = dcache_dir_close,   // 关闭目录的函数
	.llseek     = dcache_dir_lseek,   // 目录文件的定位函数
	.read       = generic_read_dir,   // 读取目录内容的函数（返回错误）
	.readdir    = dcache_readdir,     // 读取目录项的函数
	.fsync      = simple_sync_file,   // 同步文件到存储的函数
};

// 定义目录相关的inode操作结构体
const struct inode_operations simple_dir_inode_operations = {
	.lookup     = simple_lookup,      // 查找目录项的函数
};

// 定义超级块操作的结构体
static const struct super_operations simple_super_operations = {
	.statfs     = simple_statfs,      // 获取文件系统状态的函数
};

/*
 * Common helper for pseudo-filesystems (sockfs, pipefs, bdev - stuff that
 * will never be mountable)
 */
/*
 * 常用辅助函数，用于伪文件系统（如sockfs, pipefs, bdev - 这些永远不会被挂载的文件系统）
 */

// 函数定义，接收文件系统类型、名称、超级块操作结构、魔数和挂载点作为参数
int get_sb_pseudo(struct file_system_type *fs_type, char *name,
	const struct super_operations *ops, unsigned long magic,
	struct vfsmount *mnt)
{
    // 创建一个新的超级块，不需要比较函数和数据指针
	struct super_block *s = sget(fs_type, NULL, set_anon_super, NULL);
	struct dentry *dentry;
	struct inode *root;
	struct qstr d_name = {.name = name, .len = strlen(name)}; // 设置目录名

	if (IS_ERR(s)) // 检查超级块是否创建成功
		return PTR_ERR(s);

	// 设置超级块的各项属性
	s->s_flags = MS_NOUSER; // 标记该文件系统不可由用户挂载
	s->s_maxbytes = MAX_LFS_FILESIZE; // 最大文件尺寸为最大长文件尺寸
	s->s_blocksize = PAGE_SIZE; // 块大小设置为系统页面大小
	s->s_blocksize_bits = PAGE_SHIFT; // 块大小的位数
	s->s_magic = magic; // 设置文件系统的魔数
	s->s_op = ops ? ops : &simple_super_operations; // 设置文件系统操作函数
	s->s_time_gran = 1; // 时间精度设置为1秒

	// 创建根inode
	root = new_inode(s);
	if (!root) // 如果创建失败，跳转到错误处理
		goto Enomem;

	/*
	 * since this is the first inode, make it number 1. New inodes created
	 * after this must take care not to collide with it (by passing
	 * max_reserved of 1 to iunique).
	 */
	/*
	 * 由于这是第一个inode，将其编号设为1。在此之后创建的新inode
	 * 必须注意不要与它冲突（通过向iunique传递max_reserved为1来实现）。
	 */
	// 设置根inode的属性
	root->i_ino = 1; // inode号为1
	root->i_mode = S_IFDIR | S_IRUSR | S_IWUSR; // 目录，只有用户读写权限
	root->i_atime = root->i_mtime = root->i_ctime = CURRENT_TIME; // 设置时间为当前时间
	// 为根目录分配一个目录项
	dentry = d_alloc(NULL, &d_name);
	if (!dentry) { // 如果分配失败，释放inode并跳转到错误处理
		iput(root);
		goto Enomem;
	}
	// 设置目录项属性
	dentry->d_sb = s; // 设置超级块
	dentry->d_parent = dentry; // 父目录项设置为自己，因为它是根目录
	d_instantiate(dentry, root); // 将目录项和inode关联
	// 设置超级块的根目录项
	s->s_root = dentry;
	s->s_flags |= MS_ACTIVE; // 标记文件系统为活跃状态
	simple_set_mnt(mnt, s); // 将文件系统挂载到给定的挂载点
	return 0; // 返回0表示成功

Enomem: // 内存错误处理
	deactivate_locked_super(s); // 停用并锁定超级块
	return -ENOMEM; // 返回内存不足错误
}

// 创建一个硬链接
int simple_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = old_dentry->d_inode; // 从旧的目录项中获取inode

	// 更新时间戳，设置当前inode和目录的修改和状态改变时间为当前时间
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	inc_nlink(inode); // 增加inode的链接计数
	atomic_inc(&inode->i_count); // 原子增加inode的引用计数
	dget(dentry); // 增加新目录项的引用计数
	d_instantiate(dentry, inode); // 将新目录项与inode关联
	return 0; // 返回成功
}

// 检查目录项是否有效（即已经在目录项缓存中并且有关联的inode）
static inline int simple_positive(struct dentry *dentry)
{
	return dentry->d_inode && !d_unhashed(dentry);
}

// 检查目录是否为空
int simple_empty(struct dentry *dentry)
{
	struct dentry *child;
	int ret = 0;

	spin_lock(&dcache_lock); // 加锁目录项缓存，保护目录项列表
	list_for_each_entry(child, &dentry->d_subdirs, d_u.d_child) // 遍历目录下的所有子目录项
		if (simple_positive(child)) // 如果找到有效的子目录项
			goto out; // 跳转到结束处理
	ret = 1; // 如果没有有效的子目录项，设置返回值为1，表示目录为空
out:
	spin_unlock(&dcache_lock); // 解锁目录项缓存
	return ret; // 返回检查结果
}

// 删除一个文件链接
int simple_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode; // 获取待删除文件的inode

	// 更新时间戳，设置被删除文件和其所在目录的修改和状态改变时间为当前时间
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	drop_nlink(inode); // 减少inode的链接计数
	dput(dentry); // 减少目录项的引用计数，可能导致其释放
	return 0; // 返回成功
}

// 删除一个目录
int simple_rmdir(struct inode *dir, struct dentry *dentry)
{
	if (!simple_empty(dentry)) // 首先检查目录是否为空
		return -ENOTEMPTY; // 如果目录不为空，返回错误码表示目录非空

	drop_nlink(dentry->d_inode); // 减少目录的链接计数
	simple_unlink(dir, dentry); // 调用simple_unlink函数删除目录项
	drop_nlink(dir); // 再次减少父目录的链接计数
	return 0; // 返回成功
}

// 重命名或移动一个文件或目录
int simple_rename(struct inode *old_dir, struct dentry *old_dentry,
                  struct inode *new_dir, struct dentry *new_dentry)
{
	struct inode *inode = old_dentry->d_inode; // 获取旧目录项的inode
	int they_are_dirs = S_ISDIR(old_dentry->d_inode->i_mode); // 检查是否为目录

	// 检查新目录项是否为空，如果不为空，返回-ENOTEMPTY错误
	if (!simple_empty(new_dentry))
		return -ENOTEMPTY;

	// 如果新目录项中已存在inode，需要先进行unlink操作
	if (new_dentry->d_inode) {
		simple_unlink(new_dir, new_dentry); // 调用simple_unlink函数删除新目录项
		if (they_are_dirs) // 如果是目录，还需要更新旧目录的链接计数
			drop_nlink(old_dir);
	} else if (they_are_dirs) {
		// 如果是目录且新目录项中没有inode，则更新链接计数
		drop_nlink(old_dir); // 减少旧目录的链接计数
		inc_nlink(new_dir); // 增加新目录的链接计数
	}

	// 更新旧目录、新目录及相关inode的时间戳
	old_dir->i_ctime = old_dir->i_mtime = new_dir->i_ctime =
		new_dir->i_mtime = inode->i_ctime = CURRENT_TIME;

	return 0; // 返回成功
}

// 用于读取一个页面的数据
int simple_readpage(struct file *file, struct page *page)
{
	clear_highpage(page); // 清除页面内容，确保页面数据是干净的
	flush_dcache_page(page); // 刷新数据缓存，确保页面数据在内存中是最新的
	SetPageUptodate(page); // 标记页面为最新状态，表示数据是最新的
	unlock_page(page); // 解锁页面，允许其他进程访问
	return 0; // 返回成功
}

// 准备开始写入数据到页面
int simple_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata)
{
	struct page *page; // 定义页面变量
	pgoff_t index; // 页面索引

	index = pos >> PAGE_CACHE_SHIFT; // 计算页面索引，pos右移页面缓存位数得到页面索引

	page = grab_cache_page_write_begin(mapping, index, flags); // 获取页面进行写操作的准备
	if (!page) // 如果页面获取失败
		return -ENOMEM; // 返回内存不足错误

	*pagep = page; // 将获取到的页面地址存储到pagep指针

	if (!PageUptodate(page) && (len != PAGE_CACHE_SIZE)) { // 如果页面不是最新的并且写入长度不等于页面大小
		unsigned from = pos & (PAGE_CACHE_SIZE - 1); // 计算写入起始位置

		zero_user_segments(page, 0, from, from + len, PAGE_CACHE_SIZE); // 部分清零页面，只清零写入部分之外的区域
	}
	return 0; // 返回成功
}

/**
 * simple_write_end - .write_end helper for non-block-device FSes
 * @available: See .write_end of address_space_operations
 * @file: 		"
 * @mapping: 		"
 * @pos: 		"
 * @len: 		"
 * @copied: 		"
 * @page: 		"
 * @fsdata: 		"
 *
 * simple_write_end does the minimum needed for updating a page after writing is
 * done. It has the same API signature as the .write_end of
 * address_space_operations vector. So it can just be set onto .write_end for
 * FSes that don't need any other processing. i_mutex is assumed to be held.
 * Block based filesystems should use generic_write_end().
 * NOTE: Even though i_size might get updated by this function, mark_inode_dirty
 * is not called, so a filesystem that actually does store data in .write_inode
 * should extend on what's done here with a call to mark_inode_dirty() in the
 * case that i_size has changed.
 */
/**
 * simple_write_end - 非块设备文件系统的.write_end辅助函数
 * available: 见 address_space_operations 的 .write_end
 * file:      参见上文
 * mapping:   参见上文
 * pos:       参见上文
 * len:       参见上文
 * copied:    参见上文
 * page:      参见上文
 * fsdata:    参见上文
 *
 * simple_write_end 为写操作完成后更新页面所做的最小处理。
 * 它具有与 address_space_operations 向量中的 .write_end 相同的API签名。
 * 因此，对于不需要任何其他处理的文件系统，可以直接设置为 .write_end。
 * 假设已持有 i_mutex。基于块的文件系统应使用 generic_write_end()。
 * 注意：尽管 i_size 可能由此函数更新，但不会调用 mark_inode_dirty，
 * 因此，实际存储数据的文件系统应在 i_size 发生变化时，
 * 在此操作基础上扩展 mark_inode_dirty() 的调用。
 */
int simple_write_end(struct file *file, struct address_space *mapping,
                     loff_t pos, unsigned len, unsigned copied,
                     struct page *page, void *fsdata)
{
	struct inode *inode = page->mapping->host; // 获取页面所属的inode
	loff_t last_pos = pos + copied; // 计算写入操作结束后的位置

	/* zero the stale part of the page if we did a short copy */
	/* 如果实际写入字节数少于请求的字节数，将页面的剩余部分置零 */
	if (copied < len) {
			unsigned from = pos & (PAGE_CACHE_SIZE - 1);

			zero_user(page, from + copied, len - copied); // 置零未写满的部分
	}

	/* 如果页面不是最新的，将其标记为最新 */
	if (!PageUptodate(page))
			SetPageUptodate(page);

	/*
	 * No need to use i_size_read() here, the i_size
	 * cannot change under us because we hold the i_mutex.
	 */
	/* 不需要使用 i_size_read()，因为我们持有 i_mutex，i_size 不会变 */
	if (last_pos > inode->i_size)
			i_size_write(inode, last_pos); // 如果写入扩展了文件大小，则更新 i_size

	set_page_dirty(page); // 将页面标记为脏，表示页面已被修改
	unlock_page(page); // 解锁页面，允许其他进程访问
	page_cache_release(page); // 释放页面缓存

	return copied; // 返回实际写入的字节数
}

/*
 * the inodes created here are not hashed. If you use iunique to generate
 * unique inode values later for this filesystem, then you must take care
 * to pass it an appropriate max_reserved value to avoid collisions.
 */
/*
 * 这里创建的inodes没有被哈希处理。如果你稍后对此文件系统使用iunique来生成唯一的inode值，
 * 那么你必须注意传递一个适当的max_reserved值以避免冲突。
 */
int simple_fill_super(struct super_block *s, int magic, struct tree_descr *files)
{
	struct inode *inode; // 用于存储inode对象
	struct dentry *root; // 根目录项
	struct dentry *dentry; // 临时目录项
	int i;

	s->s_blocksize = PAGE_CACHE_SIZE; // 设置块大小为页面缓存大小
	s->s_blocksize_bits = PAGE_CACHE_SHIFT; // 设置块大小的位移量
	s->s_magic = magic; // 设置文件系统的魔数
	s->s_op = &simple_super_operations; // 设置超级块操作
	s->s_time_gran = 1; // 设置时间粒度为1秒

	inode = new_inode(s); // 创建一个新的inode
	if (!inode)
		return -ENOMEM; // 如果inode创建失败，返回内存不足错误

	/*
	 * because the root inode is 1, the files array must not contain an
	 * entry at index 1
	 */
	/*
	 * 因为根inode编号是1，files数组中不得包含索引为1的条目
	 */
	inode->i_ino = 1; // 设置inode的编号为1
	inode->i_mode = S_IFDIR | 0755; // 设置inode为目录类型，并设置权限为0755
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME; // 设置访问时间、修改时间和状态改变时间为当前时间
	inode->i_op = &simple_dir_inode_operations; // 设置inode的操作
	inode->i_fop = &simple_dir_operations; // 设置文件操作
	inode->i_nlink = 2; // 设置链接数为2
	root = d_alloc_root(inode); // 为inode分配一个根目录项
	if (!root) {
		iput(inode); // 释放inode
		return -ENOMEM; // 返回内存不足错误
	}
	for (i = 0; !files->name || files->name[0]; i++, files++) {
		if (!files->name)
			continue;

		/* warn if it tries to conflict with the root inode */
		/* 如果尝试与根inode冲突则发出警告 */
		if (unlikely(i == 1))
			printk(KERN_WARNING "%s: %s passed in a files array"
				"with an index of 1!\n", __func__,
				s->s_type->name);

		dentry = d_alloc_name(root, files->name); // 为每个文件分配一个目录项
		if (!dentry)
			goto out;
		inode = new_inode(s);
		if (!inode)
			goto out;
		inode->i_mode = S_IFREG | files->mode; // 设置inode为普通文件类型，并应用特定的权限
		inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME; // 设置时间属性
		inode->i_fop = files->ops; // 设置文件操作
		inode->i_ino = i; // 设置inode编号
		d_add(dentry, inode); // 将目录项和inode关联
	}
	s->s_root = root; // 设置超级块的根目录项
	return 0;
out:
	d_genocide(root); // 递归删除根目录下所有目录项
	dput(root); // 释放根目录项
	return -ENOMEM; // 返回内存不足错误
}

// 定义一个静态的自旋锁，用于保护文件系统的挂载点
static DEFINE_SPINLOCK(pin_fs_lock);

// 一个函数，用于挂载一个简单的文件系统，并确保其挂载点在多次调用中保持稳定
int simple_pin_fs(struct file_system_type *type, struct vfsmount **mount, int *count)
{
	struct vfsmount *mnt = NULL;  // 用于临时存储挂载点
	// 上锁，防止多线程同时访问
	spin_lock(&pin_fs_lock);

	// 如果挂载点尚未初始化
	if (unlikely(!*mount)) {
		// 先解锁，因为vfs_kern_mount可能睡眠
		spin_unlock(&pin_fs_lock);
		// 挂载文件系统
		mnt = vfs_kern_mount(type, 0, type->name, NULL);
		if (IS_ERR(mnt))
			return PTR_ERR(mnt);  // 如果挂载失败，返回错误
		// 再次上锁，准备设置挂载点
		spin_lock(&pin_fs_lock);
		// 检查挂载点是否仍未被设置（防止在解锁和上锁的间隙被其他线程设置）
		if (!*mount)
			*mount = mnt;
	}
	// 增加挂载点的引用计数
	mntget(*mount);
	++*count;  // 增加计数器，跟踪有多少个引用
	// 解锁
	spin_unlock(&pin_fs_lock);
	// 如果mnt被赋值且不是最终使用的挂载点，则减少其引用计数
	if (mnt && mnt != *mount)
		mntput(mnt);

	return 0;  // 返回成功
}

// 释放通过simple_pin_fs挂载的文件系统
void simple_release_fs(struct vfsmount **mount, int *count)
{
	struct vfsmount *mnt;  // 用于临时存储挂载点
	// 上锁以保护对挂载点和计数器的访问
	spin_lock(&pin_fs_lock);
	mnt = *mount;  // 取得当前挂载点的引用
	// 减少引用计数，并检查是否需要释放挂载点
	if (!--*count)
		*mount = NULL;  // 如果计数为0，清除挂载点
	// 解锁
	spin_unlock(&pin_fs_lock);
	// 减少挂载点的引用计数，可能导致其释放
	mntput(mnt);
}

/**
 * simple_read_from_buffer - copy data from the buffer to user space
 * @to: the user space buffer to read to
 * @count: the maximum number of bytes to read
 * @ppos: the current position in the buffer
 * @from: the buffer to read from
 * @available: the size of the buffer
 *
 * The simple_read_from_buffer() function reads up to @count bytes from the
 * buffer @from at offset @ppos into the user space address starting at @to.
 *
 * On success, the number of bytes read is returned and the offset @ppos is
 * advanced by this number, or negative value is returned on error.
 **/
/**
 * simple_read_from_buffer - 从缓冲区复制数据到用户空间
 * @to: 用户空间的缓冲区目标地址
 * @count: 最大可读取的字节数
 * @ppos: 缓冲区中的当前位置
 * @from: 要读取的缓冲区
 * @available: 缓冲区的大小
 *
 * simple_read_from_buffer() 函数从缓冲区 @from 在偏移 @ppos 处开始，
 * 最多读取 @count 字节到位于 @to 的用户空间地址。
 *
 * 成功时，返回读取的字节数并将偏移 @ppos 增加这个数目，
 * 或在出错时返回负值。
 **/
ssize_t simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos,
                                const void *from, size_t available)
{
	loff_t pos = *ppos;  // 获取当前的读取位置
	size_t ret;

	if (pos < 0)
		return -EINVAL;  // 如果位置小于0，返回无效参数错误
	if (pos >= available || !count)
		return 0;  // 如果位置超出可用范围或没有要读的数据，返回0
	if (count > available - pos)
		count = available - pos;  // 如果请求的数据量大于可用数据，调整count为最大可读数据量

	ret = copy_to_user(to, from + pos, count);  // 尝试将数据从内核空间复制到用户空间
	if (ret == count)
		return -EFAULT;  // 如果没有数据被复制，返回故障错误
	count -= ret;  // 计算实际复制的数据量
	*ppos = pos + count;  // 更新读取位置
	return count;  // 返回实际读取的数据量
}

/**
 * memory_read_from_buffer - copy data from the buffer
 * @to: the kernel space buffer to read to
 * @count: the maximum number of bytes to read
 * @ppos: the current position in the buffer
 * @from: the buffer to read from
 * @available: the size of the buffer
 *
 * The memory_read_from_buffer() function reads up to @count bytes from the
 * buffer @from at offset @ppos into the kernel space address starting at @to.
 *
 * On success, the number of bytes read is returned and the offset @ppos is
 * advanced by this number, or negative value is returned on error.
 **/
/**
 * memory_read_from_buffer - 从缓冲区复制数据
 * @to: 内核空间的缓冲区目标地址
 * @count: 最大可读取的字节数
 * @ppos: 缓冲区中的当前位置
 * @from: 要读取的缓冲区
 * @available: 缓冲区的大小
 *
 * memory_read_from_buffer() 函数从缓冲区 @from 在偏移 @ppos 处开始，
 * 最多读取 @count 字节到位于 @to 的内核空间地址。
 *
 * 成功时，返回读取的字节数并将偏移 @ppos 增加这个数目，
 * 或在出错时返回负值。
 **/
ssize_t memory_read_from_buffer(void *to, size_t count, loff_t *ppos,
                                const void *from, size_t available)
{
	loff_t pos = *ppos;  // 获取当前的读取位置

	if (pos < 0)
		return -EINVAL;  // 如果位置小于0，返回无效参数错误
	if (pos >= available)
		return 0;  // 如果位置超出可用范围，返回0，表示没有数据可读
	if (count > available - pos)
		count = available - pos;  // 如果请求的数据量大于可用数据，调整count为最大可读数据量

	memcpy(to, from + pos, count);  // 从from+pos处开始，复制count字节到to
	*ppos = pos + count;  // 更新读取位置

	return count;  // 返回实际读取的数据量
}

/*
 * Transaction based IO.
 * The file expects a single write which triggers the transaction, and then
 * possibly a read which collects the result - which is stored in a
 * file-local buffer.
 */
/*
 * 基于事务的IO。
 * 文件期望一个触发事务的单一写操作，然后可能是一个收集结果的读操作 - 结果存储在
 * 文件本地缓冲区中。
 */
// 处理基于事务的I/O操作的一部分。它用于设置事务的大小，并确保数据在被读取之前已经准备好。
void simple_transaction_set(struct file *file, size_t n)
{
	struct simple_transaction_argresp *ar = file->private_data; // 获取文件的私有数据

	// 如果设置的大小超过了预设的限制，则触发BUG
	BUG_ON(n > SIMPLE_TRANSACTION_LIMIT);

	/*
	 * The barrier ensures that ar->size will really remain zero until
	 * ar->data is ready for reading.
	 */
	/*
	 * 此内存屏障确保在ar->data准备好读取之前，ar->size真的保持为零。
	 * Memory barrier (内存屏障) 用来保证所有操作的执行顺序，确保之前的写操作
	 * 在这个屏障之后的写操作之前完成。
	 */
	smp_mb(); // 设置一个内存屏障

	ar->size = n; // 设置事务大小
}

/*
 * 用于从用户空间接收数据，并将其存储在内核空间的缓冲区中，这个缓冲区是与文件的 private_data 字段相关联。
 * 这是在一个简单的事务模式下，通常用于内核模块或设备驱动中进行单次写操作的基础。
 */
char *simple_transaction_get(struct file *file, const char __user *buf, size_t size)
{
	struct simple_transaction_argresp *ar; // 定义一个指向事务响应结构的指针
	static DEFINE_SPINLOCK(simple_transaction_lock); // 定义一个静态自旋锁，用于同步访问

	// 如果请求的大小超过了预定的限制，则返回错误
	if (size > SIMPLE_TRANSACTION_LIMIT - 1)
		return ERR_PTR(-EFBIG);

	// 分配一个页面并将其清零，用于存储事务数据
	ar = (struct simple_transaction_argresp *)get_zeroed_page(GFP_KERNEL);
	if (!ar)
		return ERR_PTR(-ENOMEM); // 如果内存分配失败，返回错误

	spin_lock(&simple_transaction_lock); // 加锁以保护对文件private_data的访问

	/* only one write allowed per open */
	/* 只允许每个打开的文件进行一次写操作 */
	if (file->private_data) {
		spin_unlock(&simple_transaction_lock); // 解锁
		free_page((unsigned long)ar); // 释放之前分配的页面
		return ERR_PTR(-EBUSY); // 如果已经写入过，返回忙状态
}

	file->private_data = ar; // 将分配的内存页设置为文件的私有数据

	spin_unlock(&simple_transaction_lock); // 解锁

	// 从用户空间复制数据到内核空间
	if (copy_from_user(ar->data, buf, size))
		return ERR_PTR(-EFAULT); // 如果复制失败，返回错误

	return ar->data; // 返回内核空间中的数据地址
}

// 从事务缓冲区读取数据到用户空间
ssize_t simple_transaction_read(struct file *file, char __user *buf, size_t size, loff_t *pos)
{
	struct simple_transaction_argresp *ar = file->private_data; // 获取文件的私有数据

	if (!ar)
		return 0; // 如果没有数据，返回0
	// 调用simple_read_from_buffer函数从缓冲区读取数据
	return simple_read_from_buffer(buf, size, pos, ar->data, ar->size);
}

// 在文件关闭时释放与文件关联的资源
int simple_transaction_release(struct inode *inode, struct file *file)
{
	// 释放存储事务数据的页面
	free_page((unsigned long)file->private_data);
	return 0; // 返回0表示成功
}

/* Simple attribute files */
/* 简单属性文件 */

// 定义一个结构体用于管理简单属性
struct simple_attr {
	int (*get)(void *, u64 *);  // 函数指针，用于获取属性值。接受一个void指针和一个指向u64的指针
	int (*set)(void *, u64);    // 函数指针，用于设置属性值。接受一个void指针和一个u64值
	/* enough to store a u64 and "\n\0" */
	char get_buf[24];	/* 足以存储一个u64值及其后的 "\n\0" */
	char set_buf[24];   // 两个字符数组，用于存储从用户空间读取或写入到用户空间的数据
	void *data;         // 指向任意数据的指针，可用于存储额外的属性数据或上下文
	/* format for read operation */
	const char *fmt;	/* 读操作的格式字符串 */
	/* protects access to these buffers */
	struct mutex mutex;	/* 保护对这些缓冲区的访问的互斥锁 */
};

/* simple_attr_open is called by an actual attribute open file operation
 * to set the attribute specific access operations. */
/* simple_attr_open 被实际的属性文件打开操作调用
 * 以设置特定于属性的访问操作。 */
// 初始化属性结构并设置特定的文件操作
int simple_attr_open(struct inode *inode, struct file *file,
                     int (*get)(void *, u64 *), int (*set)(void *, u64),
                     const char *fmt)
{
	struct simple_attr *attr;  // 定义一个指向 simple_attr 结构体的指针

	// 为属性结构体分配内存
	attr = kmalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr)
			return -ENOMEM;  // 如果内存分配失败，返回内存不足错误

	// 设置属性结构体的字段
	attr->get = get;          // 获取属性值的函数
	attr->set = set;          // 设置属性值的函数
	attr->data = inode->i_private; // 从inode中获取关联的私有数据
	attr->fmt = fmt;          // 设置格式化字符串
	mutex_init(&attr->mutex); // 初始化互斥锁

	file->private_data = attr; // 将属性结构体设置为文件的私有数据

	// 使用nonseekable_open函数打开文件，该函数设置文件为不可寻址
	return nonseekable_open(inode, file);
}

// 用于释放在打开文件时分配的资源
int simple_attr_release(struct inode *inode, struct file *file)
{
	// 释放文件打开时分配的属性结构体内存
	kfree(file->private_data);
	return 0;  // 返回0表示成功
}

/* read from the buffer that is filled with the get function */
/* 从通过get函数填充的缓冲区中读取 */
// 从通过 get 函数填充的缓冲区中读取数据到用户空间。
ssize_t simple_attr_read(struct file *file, char __user *buf,
                         size_t len, loff_t *ppos)
{
	struct simple_attr *attr;  // 文件私有数据的结构体指针
	size_t size;  // 用于记录生成的输出大小
	ssize_t ret;  // 用于记录函数返回值

	attr = file->private_data;  // 从文件的私有数据中获取simple_attr结构体

	if (!attr->get)  // 如果get函数指针为空，返回访问错误
		return -EACCES;

	// 尝试锁定互斥锁，如果操作被中断则返回错误
	ret = mutex_lock_interruptible(&attr->mutex);
	if (ret)
		return ret;

	/* continued read */
	if (*ppos) {    // 如果是继续读取（非首次读取）
		size = strlen(attr->get_buf);  // 获取缓冲区中字符串的长度
		// 如果是首次读取
	} else {	/* first read */
		u64 val;
		ret = attr->get(attr->data, &val);  // 调用get函数获取值
		if (ret)  // 如果get函数返回错误，直接跳到释放锁
			goto out;

		// 使用格式字符串将获取的值写入get_buf，并记录写入的字符数
		size = scnprintf(attr->get_buf, sizeof(attr->get_buf),
											attr->fmt, (unsigned long long)val);
	}

	// 从get_buf中读取数据到用户空间的buf中
	ret = simple_read_from_buffer(buf, len, ppos, attr->get_buf, size);
out:
	mutex_unlock(&attr->mutex);  // 释放互斥锁
	return ret;  // 返回读取的字节数或错误码
}

/* interpret the buffer as a number to call the set function with */
/* 将缓冲区解释为一个数字，用该数字调用set函数 */
// 用于处理用户通过写入操作传递给文件的数据，将其解释为一个数字并使用该数字调用设定函数。
ssize_t simple_attr_write(struct file *file, const char __user *buf,
                          size_t len, loff_t *ppos)
{
	struct simple_attr *attr;  // 文件的私有数据结构
	u64 val;                   // 用于存储解析的数值
	size_t size;               // 实际要复制的字节数
	ssize_t ret;               // 用于存储返回值

	attr = file->private_data;  // 获取文件的私有数据
	if (!attr->set)             // 如果set函数未设置，返回访问拒绝错误
		return -EACCES;

	ret = mutex_lock_interruptible(&attr->mutex);  // 尝试加锁
	if (ret)
		return ret;  // 如果加锁失败（被中断），返回错误

	ret = -EFAULT;  // 默认设置错误为 -EFAULT
	size = min(sizeof(attr->set_buf) - 1, len);  // 计算要复制的最大字节数，避免溢出
	if (copy_from_user(attr->set_buf, buf, size))  // 从用户空间复制数据到内核空间
		goto out;  // 如果复制失败，跳转到错误处理

	attr->set_buf[size] = '\0';  // 确保字符串以null终止
	val = simple_strtol(attr->set_buf, NULL, 0);  // 将字符串转换为无符号长整型
	ret = attr->set(attr->data, val);  // 调用set函数设置值
	if (ret == 0)
		/* on success, claim we got the whole input */
		ret = len;  // 如果设置成功，返回写入的字节数

out:
	mutex_unlock(&attr->mutex);  // 释放锁
	return ret;  // 返回结果
}

/**
 * generic_fh_to_dentry - generic helper for the fh_to_dentry export operation
 * @sb:		filesystem to do the file handle conversion on
 * @fid:	file handle to convert
 * @fh_len:	length of the file handle in bytes
 * @fh_type:	type of file handle
 * @get_inode:	filesystem callback to retrieve inode
 *
 * This function decodes @fid as long as it has one of the well-known
 * Linux filehandle types and calls @get_inode on it to retrieve the
 * inode for the object specified in the file handle.
 */
/**
 * generic_fh_to_dentry - 通用文件句柄到目录项转换函数
 * @sb:		进行文件句柄转换的文件系统
 * @fid:	要转换的文件句柄
 * @fh_len:	文件句柄的字节长度
 * @fh_type:	文件句柄的类型
 * @get_inode:	文件系统的回调函数，用来检索 inode
 *
 * 此函数解码 @fid，只要它具有已知的 Linux 文件句柄类型之一，并调用 @get_inode
 * 来检索文件句柄中指定对象的 inode。
 */
// 用于将文件句柄转换为目录项 (dentry) 的通用帮助函数。
struct dentry *generic_fh_to_dentry(struct super_block *sb, struct fid *fid,
        int fh_len, int fh_type, struct inode *(*get_inode)
            (struct super_block *sb, u64 ino, u32 gen))
{
	struct inode *inode = NULL;  // 初始化 inode 指针为 NULL

	if (fh_len < 2)  // 如果文件句柄长度小于2，返回 NULL
		return NULL;

	switch (fh_type) {  // 根据文件句柄类型进行处理
	case FILEID_INO32_GEN:  // 32位 inode 编号和生成号的情况
	case FILEID_INO32_GEN_PARENT:  // 32位 inode 编号和生成号及其父目录的情况
		inode = get_inode(sb, fid->i32.ino, fid->i32.gen);  // 调用 get_inode 函数获取 inode
		break;
	}

	return d_obtain_alias(inode);  // 获取或创建与给定 inode 相关联的目录项
}
EXPORT_SYMBOL_GPL(generic_fh_to_dentry);

/**
 * generic_fh_to_dentry - generic helper for the fh_to_parent export operation
 * @sb:		filesystem to do the file handle conversion on
 * @fid:	file handle to convert
 * @fh_len:	length of the file handle in bytes
 * @fh_type:	type of file handle
 * @get_inode:	filesystem callback to retrieve inode
 *
 * This function decodes @fid as long as it has one of the well-known
 * Linux filehandle types and calls @get_inode on it to retrieve the
 * inode for the _parent_ object specified in the file handle if it
 * is specified in the file handle, or NULL otherwise.
 */
/**
 * generic_fh_to_parent - 通用辅助函数，用于执行 fh_to_parent 导出操作
 * @sb:		进行文件句柄转换的文件系统
 * @fid:	要转换的文件句柄
 * @fh_len:	文件句柄的字节长度
 * @fh_type:	文件句柄的类型
 * @get_inode:	文件系统的回调函数，用来检索 inode
 *
 * 此函数解码 @fid，只要它具有已知的 Linux 文件句柄类型之一，并调用 @get_inode
 * 来检索文件句柄中指定的父对象的 inode（如果文件句柄中指定了父对象），
 * 否则返回 NULL。
 */
// 一个用于将文件句柄转换为其父目录项（dentry）的通用辅助函数。
struct dentry *generic_fh_to_parent(struct super_block *sb, struct fid *fid,
		int fh_len, int fh_type, struct inode *(*get_inode)
			(struct super_block *sb, u64 ino, u32 gen))
{
	struct inode *inode = NULL;  // 初始化 inode 指针为 NULL

	if (fh_len <= 2)  // 如果文件句柄长度小于或等于2，返回 NULL
		return NULL;

	switch (fh_type) {  // 根据文件句柄类型进行处理
	case FILEID_INO32_GEN_PARENT:
		// 如果是具有父 inode 编号和生成号的类型，调用 get_inode 获取父 inode
		inode = get_inode(sb, fid->i32.parent_ino,
				  (fh_len > 3 ? fid->i32.parent_gen : 0));
		break;
	}

	return d_obtain_alias(inode);  // 获取或创建与给定 inode 相关联的目录项
}
EXPORT_SYMBOL_GPL(generic_fh_to_parent);

int simple_fsync(struct file *file, struct dentry *dentry, int datasync)
{
	struct writeback_control wbc = {
		// 设置写回控制的同步模式为全部同步
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = 0, /* metadata-only; caller takes care of data */
		/* 仅仅同步元数据；调用者负责数据同步 */
	};
	// 从目录项中获取 inode 结构
	struct inode *inode = dentry->d_inode;
	int err;	// 错误码变量
	int ret;
	
	// 同步 inode 映射到的缓存
	ret = sync_mapping_buffers(inode->i_mapping);
	// 如果inode 没有被标记为脏，直接返回
	if (!(inode->i_state & I_DIRTY))
		return ret;
	// 如果是 datasync 并且 inode 没有为数据同步标记为脏，直接返回
	if (datasync && !(inode->i_state & I_DIRTY_DATASYNC))
		return ret;

	// 否则，同步 inode
	err = sync_inode(inode, &wbc);
	// 如果之前未发生错误则使用新的错误码
	if (ret == 0)
		ret = err;
	return ret;	// 返回最终的错误码或成功码
}
EXPORT_SYMBOL(simple_fsync);

EXPORT_SYMBOL(dcache_dir_close);
EXPORT_SYMBOL(dcache_dir_lseek);
EXPORT_SYMBOL(dcache_dir_open);
EXPORT_SYMBOL(dcache_readdir);
EXPORT_SYMBOL(generic_read_dir);
EXPORT_SYMBOL(get_sb_pseudo);
EXPORT_SYMBOL(simple_write_begin);
EXPORT_SYMBOL(simple_write_end);
EXPORT_SYMBOL(simple_dir_inode_operations);
EXPORT_SYMBOL(simple_dir_operations);
EXPORT_SYMBOL(simple_empty);
EXPORT_SYMBOL(simple_fill_super);
EXPORT_SYMBOL(simple_getattr);
EXPORT_SYMBOL(simple_link);
EXPORT_SYMBOL(simple_lookup);
EXPORT_SYMBOL(simple_pin_fs);
EXPORT_SYMBOL(simple_readpage);
EXPORT_SYMBOL(simple_release_fs);
EXPORT_SYMBOL(simple_rename);
EXPORT_SYMBOL(simple_rmdir);
EXPORT_SYMBOL(simple_statfs);
EXPORT_SYMBOL(simple_sync_file);
EXPORT_SYMBOL(simple_unlink);
EXPORT_SYMBOL(simple_read_from_buffer);
EXPORT_SYMBOL(memory_read_from_buffer);
EXPORT_SYMBOL(simple_transaction_set);
EXPORT_SYMBOL(simple_transaction_get);
EXPORT_SYMBOL(simple_transaction_read);
EXPORT_SYMBOL(simple_transaction_release);
EXPORT_SYMBOL_GPL(simple_attr_open);
EXPORT_SYMBOL_GPL(simple_attr_release);
EXPORT_SYMBOL_GPL(simple_attr_read);
EXPORT_SYMBOL_GPL(simple_attr_write);
