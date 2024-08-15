#include <linux/module.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/path.h>
#include <linux/slab.h>
#include <linux/fs_struct.h>

/*
 * Replace the fs->{rootmnt,root} with {mnt,dentry}. Put the old values.
 * It can block.
 */
/*
 * 将 fs->rootmnt 和 fs->root 替换为 mnt 和 dentry，并释放旧的值。
 * 该操作可能会阻塞。
 */
void set_fs_root(struct fs_struct *fs, struct path *path)
{
	struct path old_root;  // 定义一个结构体变量，用于保存旧的根路径

	write_lock(&fs->lock);  // 获取 fs 结构体上的写锁，防止其他线程同时修改该结构体
	old_root = fs->root;  // 保存当前的根路径到 old_root
	fs->root = *path;  // 将传入的路径设置为新的根路径
	path_get(path);  // 增加新根路径的引用计数，确保其不会被释放
	write_unlock(&fs->lock);  // 释放写锁

	// 如果旧的根路径存在（即 old_root.dentry 不为空），则释放它的引用计数
	if (old_root.dentry)
		path_put(&old_root);
	// path_put 会减少 old_root 的引用计数，如果引用计数变为 0，则释放与之相关的资源
}

/*
 * Replace the fs->{pwdmnt,pwd} with {mnt,dentry}. Put the old values.
 * It can block.
 */
/*
 * 用 {mnt,dentry} 替换 fs->{pwdmnt,pwd}，并释放旧的值。
 * 可能会阻塞。
 */
void set_fs_pwd(struct fs_struct *fs, struct path *path)
{
	struct path old_pwd;  // 用于保存旧的当前工作目录

	write_lock(&fs->lock);  // 加写锁，保护对 fs_struct 的修改操作
	old_pwd = fs->pwd;  // 保存当前的工作目录到 old_pwd
	fs->pwd = *path;  // 设置新的当前工作目录
	path_get(path);  // 增加新工作目录的引用计数，防止其在使用中被释放
	write_unlock(&fs->lock);  // 释放写锁

	if (old_pwd.dentry)  // 如果旧的工作目录存在
		path_put(&old_pwd);  // 减少旧工作目录的引用计数，并在引用计数为 0 时释放相关资源
}

// chroot_fs_refs 用于修改所有进程的根目录和工作目录引用，从旧的根目录更新到新的根目录
void chroot_fs_refs(struct path *old_root, struct path *new_root)
{
	struct task_struct *g, *p;  // 声明进程控制块指针，用于遍历所有进程
	struct fs_struct *fs;       // 指向文件系统结构的指针
	int count = 0;              // 用于计数修改的引用数量

	read_lock(&tasklist_lock);  // 获取进程列表读锁
	do_each_thread(g, p) {      // 遍历所有线程
		task_lock(p);           // 对每个进程加锁，保护进程的 fs 结构
		fs = p->fs;             // 获取当前线程的文件系统结构
		if (fs) {
			write_lock(&fs->lock);  // 获取文件系统结构的写锁
			// 如果进程的根目录与旧的根目录相同
			if (fs->root.dentry == old_root->dentry
			    && fs->root.mnt == old_root->mnt) {
				path_get(new_root);  // 增加新根目录的引用计数
				fs->root = *new_root;  // 更新进程的根目录为新目录
				count++;  // 增加计数器
			}
			// 如果进程的当前工作目录与旧的根目录相同
			if (fs->pwd.dentry == old_root->dentry
			    && fs->pwd.mnt == old_root->mnt) {
				path_get(new_root);  // 增加新根目录的引用计数
				fs->pwd = *new_root;  // 更新进程的当前工作目录为新目录
				count++;  // 增加计数器
			}
			write_unlock(&fs->lock);  // 释放文件系统结构的写锁
		}
		task_unlock(p);  // 释放进程锁
	} while_each_thread(g, p);
	read_unlock(&tasklist_lock);  // 释放进程列表读锁

	while (count--)  // 对于所有增加的新根目录引用
		path_put(old_root);  // 减少旧根目录的引用计数
}

// 释放 fs_struct 结构
void free_fs_struct(struct fs_struct *fs)
{
	path_put(&fs->root);  // 减少根路径的引用计数，可能导致相关资源的释放
	path_put(&fs->pwd);   // 减少当前工作目录的引用计数，可能导致相关资源的释放
	kmem_cache_free(fs_cachep, fs);  // 从内核内存缓存中释放 fs_struct 结构的内存
}

// 在进程退出时处理其文件系统结构
void exit_fs(struct task_struct *tsk)
{
	struct fs_struct *fs = tsk->fs;  // 获取进程的文件系统结构

	if (fs) {  // 如果文件系统结构存在
		int kill;
		task_lock(tsk);              // 锁定任务结构，以同步对其修改
		write_lock(&fs->lock);       // 对文件系统结构加写锁
		tsk->fs = NULL;              // 将进程的文件系统结构指针置空
		kill = !--fs->users;         // 减少文件系统结构的使用计数，并检查是否应该释放它
		write_unlock(&fs->lock);     // 释放文件系统结构的写锁
		task_unlock(tsk);            // 释放任务结构的锁
		if (kill)                    // 如果没有其他用户使用这个文件系统结构
			free_fs_struct(fs);      // 释放文件系统结构
	}
}
	
// 复制一个文件系统结构
struct fs_struct *copy_fs_struct(struct fs_struct *old)
{
    // 从文件系统结构的内存缓存池中分配一个新的 fs_struct 实例
	struct fs_struct *fs = kmem_cache_alloc(fs_cachep, GFP_KERNEL);
	// 我们不需要锁定 fs - 想想为什么 ;-)
	/* We don't need to lock fs - think why ;-) */
	if (fs) {  // 如果内存分配成功
		fs->users = 1;  // 设置新文件系统结构的用户计数为1
		fs->in_exec = 0;  // 设置正在执行标志为0
		rwlock_init(&fs->lock);  // 初始化新文件系统结构的读写锁
		fs->umask = old->umask;  // 复制 umask 值

		read_lock(&old->lock);  // 获取旧文件系统结构的读锁
		fs->root = old->root;  // 复制根目录路径
		path_get(&old->root);  // 增加根目录路径的引用计数
		fs->pwd = old->pwd;  // 复制当前工作目录路径
		path_get(&old->pwd);  // 增加工作目录路径的引用计数
		read_unlock(&old->lock);  // 释放旧文件系统结构的读锁
	}
	return fs;  // 返回新的文件系统结构指针
}

// 使当前进程的文件系统结构不再共享
int unshare_fs_struct(void)
{
	struct fs_struct *fs = current->fs;  // 获取当前进程的文件系统结构
	struct fs_struct *new_fs = copy_fs_struct(fs);  // 创建当前文件系统结构的副本

	int kill;  // 用于标记是否需要释放旧的文件系统结构

	if (!new_fs)  // 如果副本创建失败
		return -ENOMEM;  // 返回内存不足的错误

	task_lock(current);  // 锁定当前进程，以同步修改
	write_lock(&fs->lock);  // 对旧的文件系统结构加写锁
	kill = !--fs->users;  // 减少旧文件系统结构的使用计数，并检查是否还有其他引用
	current->fs = new_fs;  // 将新的文件系统结构设置为当前进程的文件系统结构
	write_unlock(&fs->lock);  // 释放写锁
	task_unlock(current);  // 释放进程锁

	if (kill)  // 如果旧的文件系统结构没有其他引用
		free_fs_struct(fs);  // 释放旧的文件系统结构

	return 0;  // 返回成功
}
EXPORT_SYMBOL_GPL(unshare_fs_struct);

// 获取当前进程的 umask 值
int current_umask(void)
{
	return current->fs->umask;  // 返回当前进程文件系统结构体中的 umask 字段
}
EXPORT_SYMBOL(current_umask);  // 导出 current_umask 符号，使其可以被其他内核模块使用

/* 仅在 INIT_TASK 中提及 */
/* to be mentioned only in INIT_TASK */
struct fs_struct init_fs = {
	.users = 1,  // 用户数初始化为 1
	.lock = __RW_LOCK_UNLOCKED(init_fs.lock),  // 初始化读写锁为未锁定状态
	.umask = 0022,  // 设置默认的文件创建掩码为 0022 (八进制)
};

// 将当前进程的文件系统结构标记为守护进程使用
void daemonize_fs_struct(void)
{
	struct fs_struct *fs = current->fs;  // 获取当前进程的文件系统结构

	if (fs) {  // 如果文件系统结构存在
		int kill;  // 用于标记是否需要释放旧的文件系统结构

		task_lock(current);  // 锁定当前进程，以同步修改

		write_lock(&init_fs.lock);  // 对全局文件系统结构加写锁
		init_fs.users++;  // 增加全局文件系统结构的用户计数
		write_unlock(&init_fs.lock);  // 释放全局文件系统结构的写锁

		write_lock(&fs->lock);  // 对旧的文件系统结构加写锁
		current->fs = &init_fs;  // 将当前进程的文件系统结构指针替换为全局文件系统结构
		kill = !--fs->users;  // 减少旧文件系统结构的使用计数，并检查是否还有其他引用
		write_unlock(&fs->lock);  // 释放旧文件系统结构的写锁

		task_unlock(current);  // 释放进程锁

		if (kill)  // 如果旧的文件系统结构没有其他引用
			free_fs_struct(fs);  // 释放旧的文件系统结构
	}
}
