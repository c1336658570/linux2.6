/*
 *  fs/anon_inodes.c
 *
 *  Copyright (C) 2007  Davide Libenzi <davidel@xmailserver.org>
 *
 *  Thanks to Arnd Bergmann for code review and suggestions.
 *  More changes for Thomas Gleixner suggestions.
 *
 */

#include <linux/cred.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/magic.h>
#include <linux/anon_inodes.h>

#include <asm/uaccess.h>

// 静态全局变量，指向匿名 inode 的虚拟文件系统挂载点
static struct vfsmount *anon_inode_mnt __read_mostly;
// 静态全局变量，指向匿名 inode 结构体
static struct inode *anon_inode_inode;
// 静态结构体，定义匿名 inode 文件操作
static const struct file_operations anon_inode_fops;

// 函数用于获取匿名 inode 文件系统的超级块
static int anon_inodefs_get_sb(struct file_system_type *fs_type, int flags,
			       const char *dev_name, void *data,
			       struct vfsmount *mnt)
{
	// 创建一个伪文件系统，并返回其超级块
	return get_sb_pseudo(fs_type, "anon_inode:", NULL, ANON_INODE_FS_MAGIC,
			     mnt);
}

/*
 * anon_inodefs_dname() is called from d_path().
 */
/*
 * anon_inodefs_dname() 被 d_path() 调用。
 */
// 该函数用于生成匿名 inode 文件系统中的目录项的路径表示
static char *anon_inodefs_dname(struct dentry *dentry, char *buffer, int buflen)
{
	// 使用 dynamic_dname 函数，返回格式为 "anon_inode:%s" 的目录项名称
	return dynamic_dname(dentry, buffer, buflen, "anon_inode:%s",
				dentry->d_name.name);
}

// 定义匿名 inode 文件系统类型
static struct file_system_type anon_inode_fs_type = {
	.name		= "anon_inodefs",    // 文件系统名称
	.get_sb		= anon_inodefs_get_sb, // 指向获取超级块的函数
	.kill_sb	= kill_anon_super, // 销毁超级块的函数
};

// 定义匿名 inode 文件系统的目录项操作
static const struct dentry_operations anon_inodefs_dentry_operations = {
	.d_dname	= anon_inodefs_dname, // 生成目录项路径的函数
};

/*
 * nop .set_page_dirty method so that people can use .page_mkwrite on
 * anon inodes.
 */
/*
 * nop（无操作）的 .set_page_dirty 方法，使得开发者可以在匿名 inode 上使用 .page_mkwrite 方法。
 * 这个函数基本上是一个占位符，表示不需要做任何实际的标记页面脏操作。
 */
static int anon_set_page_dirty(struct page *page)
{
	// 该函数不执行任何操作，总是返回 0，表示成功但没有实际修改
	return 0;
};

// 定义一个结构体，包含地址空间操作的方法集，用于匿名 inode
static const struct address_space_operations anon_aops = {
    // 将上面定义的无操作的 set_page_dirty 函数指定为地址空间操作的一个方法
	.set_page_dirty = anon_set_page_dirty,
};

/**
 * anon_inode_getfd - creates a new file instance by hooking it up to an
 *                    anonymous inode, and a dentry that describe the "class"
 *                    of the file
 *
 * @name:    [in]    name of the "class" of the new file
 * @fops:    [in]    file operations for the new file
 * @priv:    [in]    private data for the new file (will be file's private_data)
 * @flags:   [in]    flags
 *
 * Creates a new file by hooking it on a single inode. This is useful for files
 * that do not need to have a full-fledged inode in order to operate correctly.
 * All the files created with anon_inode_getfile() will share a single inode,
 * hence saving memory and avoiding code duplication for the file/inode/dentry
 * setup.  Returns the newly created file* or an error pointer.
 */
/**
 * anon_inode_getfd - 通过将其挂钩到一个匿名 inode 上，并通过一个目录项描述文件的“类”，来创建一个新的文件实例。
 *
 * @name:    [输入]    新文件的“类”的名称
 * @fops:    [输入]    新文件的文件操作
 * @priv:    [输入]    新文件的私有数据（将成为文件的 private_data）
 * @flags:   [输入]    标志
 *
 * 通过将文件挂钩到一个单一的 inode 上来创建一个新文件。这适用于那些不需要完整 inode 就能正确操作的文件。
 * 使用 anon_inode_getfile() 创建的所有文件将共享一个单一的 inode，从而节省内存并避免重复编码文件/inode/dentry 的设置。
 * 返回新创建的文件指针或错误指针。
 */
struct file *anon_inode_getfile(const char *name,
				const struct file_operations *fops,
				void *priv, int flags)
{
	struct qstr this;  // 用于存储临时的目录项名称
	struct path path;  // 路径结构，包含目录项和挂载点
	struct file *file; // 文件指针
	int error;         // 错误码

	// 检查全局的 anon_inode_inode 是否有错误，如果有则返回错误指针
	if (IS_ERR(anon_inode_inode))
		return ERR_PTR(-ENODEV);

	// 检查文件操作的所有者是否存在，尝试增加其模块引用计数，失败则返回错误
	if (fops->owner && !try_module_get(fops->owner))
		return ERR_PTR(-ENOENT);

	/*
	 * Link the inode to a directory entry by creating a unique name
	 * using the inode sequence number.
	 */
	/*
	 * 通过使用 inode 序列号创建一个唯一的名称，将 inode 链接到一个目录项。
	 */
	error = -ENOMEM;
	this.name = name;   // 设置目录项的名称
	this.len = strlen(name);  // 设置目录项名称长度
	this.hash = 0;      // 目录项名称哈希值初始化
	// 分配并初始化一个目录项
	path.dentry = d_alloc(anon_inode_mnt->mnt_sb->s_root, &this);
	if (!path.dentry)
		goto err_module;

	// 获取对匿名 inode 挂载点的引用
	path.mnt = mntget(anon_inode_mnt);
	/*
	 * We know the anon_inode inode count is always greater than zero,
	 * so we can avoid doing an igrab() and we can use an open-coded
	 * atomic_inc().
	 */
	/*
	 * 由于我们知道匿名 inode 的引用计数总是大于零，因此我们可以避免执行 igrab()
	 * 而使用 open-coded atomic_inc()。
	 */
	atomic_inc(&anon_inode_inode->i_count);  // 原子增加 inode 的引用计数

	path.dentry->d_op = &anon_inodefs_dentry_operations;  // 设置目录项操作
	d_instantiate(path.dentry, anon_inode_inode);  // 实例化目录项与 inode

	error = -ENFILE;
	// 分配一个新的文件结构并进行初始化
	file = alloc_file(&path, OPEN_FMODE(flags), fops);
	if (!file)
		goto err_dput;
	file->f_mapping = anon_inode_inode->i_mapping; // 设置文件映射

	file->f_pos = 0;  // 文件位置初始化
	file->f_flags = flags & (O_ACCMODE | O_NONBLOCK);  // 设置文件标志
	file->f_version = 0;  // 文件版本初始化
	file->private_data = priv;  // 设置文件的私有数据

	return file;  // 返回文件指针

err_dput:
	path_put(&path);  // 清理路径
err_module:
	module_put(fops->owner);  // 释放模块引用
	return ERR_PTR(error);  // 返回错误指针
}
EXPORT_SYMBOL_GPL(anon_inode_getfile);

/**
 * anon_inode_getfd - creates a new file instance by hooking it up to an
 *                    anonymous inode, and a dentry that describe the "class"
 *                    of the file
 *
 * @name:    [in]    name of the "class" of the new file
 * @fops:    [in]    file operations for the new file
 * @priv:    [in]    private data for the new file (will be file's private_data)
 * @flags:   [in]    flags
 *
 * Creates a new file by hooking it on a single inode. This is useful for files
 * that do not need to have a full-fledged inode in order to operate correctly.
 * All the files created with anon_inode_getfd() will share a single inode,
 * hence saving memory and avoiding code duplication for the file/inode/dentry
 * setup.  Returns new descriptor or an error code.
 */
/**
 * anon_inode_getfd - 通过将其挂钩到一个匿名 inode 上，并通过一个目录项描述文件的“类”，来创建一个新的文件实例。
 *
 * @name:    [输入]    新文件的“类”的名称
 * @fops:    [输入]    新文件的文件操作函数
 * @priv:    [输入]    新文件的私有数据（将成为文件的 private_data）
 * @flags:   [输入]    创建文件时使用的标志
 *
 * 创建一个新的文件实例，通过将其挂钩到单一的 inode 上。这对于那些不需要完整 inode 即可正确操作的文件非常有用。
 * 使用 anon_inode_getfd() 创建的所有文件将共享一个单一的 inode，
 * 从而节省内存并避免重复代码，用于文件/inode/dentry 的设置。返回新的文件描述符或错误代码。
 */
int anon_inode_getfd(const char *name, const struct file_operations *fops,
		     void *priv, int flags)
{
	int error, fd; // 定义错误代码和文件描述符变量
	struct file *file; // 定义文件结构指针

	// 获取一个未使用的文件描述符，flags 可以指定如 O_CLOEXEC 等额外的文件状态标志
	error = get_unused_fd_flags(flags);
	if (error < 0) // 如果获取失败，则返回错误代码
		return error;
	fd = error; // 如果成功，error 里的值即为新的文件描述符

	// 调用 anon_inode_getfile 创建一个新的文件实例
	file = anon_inode_getfile(name, fops, priv, flags);
	if (IS_ERR(file)) { // 检查返回的文件实例是否有效
		error = PTR_ERR(file); // 获取错误代码
		goto err_put_unused_fd; // 跳转到错误处理代码块
	}
	fd_install(fd, file); // 将新的文件实例与文件描述符关联

	return fd; // 返回文件描述符

err_put_unused_fd:
	put_unused_fd(fd); // 如果出错，则释放之前获取的文件描述符
	return error; // 返回错误代码
}
EXPORT_SYMBOL_GPL(anon_inode_getfd);

/*
 * A single inode exists for all anon_inode files. Contrary to pipes,
 * anon_inode inodes have no associated per-instance data, so we need
 * only allocate one of them.
 */
/*
 * 所有 anon_inode 文件共享一个单一的 inode。与管道不同，
 * anon_inode 的 inode 没有关联的每个实例数据，所以我们只需要分配一个。
 */
static struct inode *anon_inode_mkinode(void)
{
    // 创建一个新的 inode 结构
	struct inode *inode = new_inode(anon_inode_mnt->mnt_sb);
    // 如果 inode 创建失败，返回错误指针
	if (!inode)
		return ERR_PTR(-ENOMEM);

    // 设置 inode 的文件操作为匿名 inode 的操作
	inode->i_fop = &anon_inode_fops;

    // 设置 inode 的地址空间操作
	inode->i_mapping->a_ops = &anon_aops;

	/*
	 * Mark the inode dirty from the very beginning,
	 * that way it will never be moved to the dirty
	 * list because mark_inode_dirty() will think
	 * that it already _is_ on the dirty list.
	 */
	/*
	 * 从一开始就将 inode 标记为脏，
	 * 这样它就永远不会被移动到脏列表，因为 mark_inode_dirty()
	 * 会认为它已经在脏列表上了。
	 */
	inode->i_state = I_DIRTY;  // 设置 inode 的状态为脏
    // 设置 inode 的权限模式为用户读写
	inode->i_mode = S_IRUSR | S_IWUSR;
    // 设置 inode 的用户 ID 和组 ID
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
    // 设置 inode 的标志，标记为私有
	inode->i_flags |= S_PRIVATE;
    // 设置 inode 的访问时间、修改时间和状态改变时间为当前时间
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	return inode;  // 返回新创建的 inode
}

/*
 * 该函数用于初始化匿名 inode 子系统。
 */
static int __init anon_inode_init(void)
{
	int error;

	// 注册匿名 inode 文件系统
	error = register_filesystem(&anon_inode_fs_type);
	if (error)
		goto err_exit;  // 如果注册失败，跳转到错误处理代码

	// 挂载匿名 inode 文件系统
	anon_inode_mnt = kern_mount(&anon_inode_fs_type);
	if (IS_ERR(anon_inode_mnt)) {
		error = PTR_ERR(anon_inode_mnt);  // 获取错误代码
		goto err_unregister_filesystem;   // 跳转到取消注册文件系统的处理代码
	}

	// 创建一个全局的匿名 inode
	anon_inode_inode = anon_inode_mkinode();
	if (IS_ERR(anon_inode_inode)) {
		error = PTR_ERR(anon_inode_inode);  // 获取错误代码
		goto err_mntput;  // 跳转到解除挂载的处理代码
	}

	return 0;  // 初始化成功

err_mntput:
	mntput(anon_inode_mnt);  // 释放文件系统挂载点的引用
err_unregister_filesystem:
	unregister_filesystem(&anon_inode_fs_type);  // 取消注册文件系统
err_exit:
	panic(KERN_ERR "anon_inode_init() failed (%d)\n", error);  // 如果初始化失败，内核崩溃
}

fs_initcall(anon_inode_init);

