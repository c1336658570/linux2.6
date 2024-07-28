/*
 * devtmpfs - kernel-maintained tmpfs-based /dev
 *
 * Copyright (C) 2009, Kay Sievers <kay.sievers@vrfy.org>
 *
 * During bootup, before any driver core device is registered,
 * devtmpfs, a tmpfs-based filesystem is created. Every driver-core
 * device which requests a device node, will add a node in this
 * filesystem.
 * By default, all devices are named after the the name of the
 * device, owned by root and have a default mode of 0600. Subsystems
 * can overwrite the default setting if needed.
 */
/*
 * 在启动过程中，在任何驱动核心设备注册之前，会创建一个基于 tmpfs 的文件系统 devtmpfs。
 * 每个请求设备节点的驱动核心设备都会在这个文件系统中添加一个节点。
 * 默认情况下，所有设备的名称与设备的名称相同，所有者是 root，并且默认权限为 0600。
 * 如果需要，子系统可以覆盖默认设置。
 */

#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/mount.h>
#include <linux/device.h>
#include <linux/genhd.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/shmem_fs.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/init_task.h>
#include <linux/slab.h>

// 定义一个指向虚拟文件系统挂载点的全局变量
static struct vfsmount *dev_mnt;

// 如果已定义CONFIG_DEVTMPFS_MOUNT，则默认挂载devtmpfs
#if defined CONFIG_DEVTMPFS_MOUNT
static int dev_mount = 1;
#else
// 如果未定义CONFIG_DEVTMPFS_MOUNT，则默认不挂载devtmpfs
static int dev_mount;
#endif

// 定义一个互斥锁，用于控制目录操作的并发访问
static DEFINE_MUTEX(dirlock);

// 初始化参数，允许在启动时通过 kernel boot 参数 "devtmpfs.mount=" 设置 dev_mount 的值
static int __init mount_param(char *str)
{
	dev_mount = simple_strtoul(str, NULL, 0);	// 使用simple_strtoul从字符串中解析出数字，设置dev_mount的值
	return 1;
}
__setup("devtmpfs.mount=", mount_param);	// 将"devtmpfs.mount="参数与mount_param函数关联

// 用于获取文件系统的超级块的函数
static int dev_get_sb(struct file_system_type *fs_type, int flags,
		      const char *dev_name, void *data, struct vfsmount *mnt)
{
	// 使用单例模式获取超级块，此处使用了共享内存文件系统的填充函数 shmem_fill_super
	return get_sb_single(fs_type, flags, data, shmem_fill_super, mnt);
}

// 定义devtmpfs文件系统类型
static struct file_system_type dev_fs_type = {
	.name = "devtmpfs",  // 文件系统名称
	.get_sb = dev_get_sb,  // 获取超级块的函数指针
	.kill_sb = kill_litter_super,  // 销毁超级块的函数指针
};

// 如果定义了CONFIG_BLOCK，即支持块设备
#ifdef CONFIG_BLOCK
// 定义一个内联函数is_blockdev，用于检测给定的设备是否为块设备
static inline int is_blockdev(struct device *dev)
{
	// 检查设备的类是否为block_class，是则返回1，表示该设备是块设备
	return dev->class == &block_class;
}
// 如果未定义CONFIG_BLOCK，即不支持块设备
#else
// 定义一个内联函数is_blockdev，对于所有的设备都返回0，表示不是块设备
static inline int is_blockdev(struct device *dev) { return 0; }
#endif

// 用于在 devtmpfs 文件系统中创建新目录。这是一个低级别的文件系统操作，通过调用 VFS (Virtual File System) 层的相关函数来实现。
static int dev_mkdir(const char *name, mode_t mode)
{
	struct nameidata nd;   // 定义用于查找路径的结构体
	struct dentry *dentry; // 目录项结构体指针
	int err;               // 用于保存错误代码

	// 查找路径。这里使用LOOKUP_PARENT是为了找到最后一个目录项的父目录
	err = vfs_path_lookup(dev_mnt->mnt_root, dev_mnt,
			      name, LOOKUP_PARENT, &nd);
	if (err)
		return err;	// 如果查找出错，则返回错误代码

	// 创建一个目录项
	dentry = lookup_create(&nd, 1);
	if (!IS_ERR(dentry)) {	// 检查是否成功创建目录项
		// 创建目录，传入父目录的inode和新目录项
		err = vfs_mkdir(nd.path.dentry->d_inode, dentry, mode);
		if (!err)
			/* mark as kernel-created inode */
			// 标记为内核创建的inode
			dentry->d_inode->i_private = &dev_mnt;
		dput(dentry);	// 减少目录项的引用计数
	} else {
		err = PTR_ERR(dentry);	// 如果创建目录项失败，获取错误代码
	}

	// 解锁inode上的互斥锁
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);
	path_put(&nd.path);	// 释放路径
	return err;	// 返回操作结果
}

// 创建路径，如果不存在任何父目录，它会递归地创建所有必要的目录
static int create_path(const char *nodepath)
{
	int err;	// 用于存储错误代码

	mutex_lock(&dirlock);	// 锁定目录操作，确保线程安全
	err = dev_mkdir(nodepath, 0755);	// 尝试创建指定路径的目录，设置权限为755
	if (err == -ENOENT) {	// 如果错误代码为-ENOENT，意味着部分父目录不存在
		char *path;	// 用于操作路径的变量
		char *s;	// 用于查找路径分隔符的指针

		/* parent directories do not exist, create them */
		// 父目录不存在，需要创建它们
		path = kstrdup(nodepath, GFP_KERNEL);	// 复制路径字符串到新的内存区域
		if (!path) {	// 内存分配失败处理
			err = -ENOMEM;	// 设置错误码为-ENOMEM
			goto out;	// 跳转到函数末尾，释放资源
		}
		s = path;
		for (;;) {	// 无限循环，直到路径中所有分隔符处理完毕
			s = strchr(s, '/');	// 查找路径中的下一个分隔符
			if (!s)	// 如果没有找到分隔符，表示已处理完整个路径
				break;
			s[0] = '\0';	// 临时将分隔符替换为字符串终止符，以便创建当前部分的目录
			err = dev_mkdir(path, 0755);	// 创建当前部分的目录
			if (err && err != -EEXIST)	// 如果创建目录失败且错误不是因为目录已存在
				break;	// 跳出循环，不再处理
			s[0] = '/';	// 恢复分隔符
			s++;	// 移动到下一个字符，继续处理
		}
		kfree(path);	// 释放路径字符串占用的内存
	}
out:
	mutex_unlock(&dirlock);	// 释放目录操作的锁
	return err;
}

// 在 devtmpfs 文件系统中为设备创建一个节点
int devtmpfs_create_node(struct device *dev)
{
	const char *tmp = NULL;  // 用于临时存储设备节点名
	const char *nodename;  // 存储设备节点的名称
	const struct cred *curr_cred;  // 当前的安全凭证
	mode_t mode = 0;  // 节点文件的模式（权限）
	struct nameidata nd;  // 用于查找路径的结构体
	struct dentry *dentry;  // 目录项指针
	int err;  // 错误代码

	// 检查是否已挂载devtmpfs文件系统
	if (!dev_mnt)
		return 0;

	// 获取设备的节点名称和模式，如果失败，返回-ENOMEM
	nodename = device_get_devnode(dev, &mode, &tmp);
	if (!nodename)
		return -ENOMEM;

	// 如果没有指定模式，则默认为0600
	if (mode == 0)
		mode = 0600;
	// 根据设备类型设置文件类型（块设备或字符设备）
	if (is_blockdev(dev))
		mode |= S_IFBLK;
	else
		mode |= S_IFCHR;

	// 使用初始凭证
	curr_cred = override_creds(&init_cred);	// 切换到初始权限凭证

	// 查找设备节点的父目录
	err = vfs_path_lookup(dev_mnt->mnt_root, dev_mnt,
			      nodename, LOOKUP_PARENT, &nd);	// 在devtmpfs文件系统中查找设备节点的父目录
	// 如果父目录不存在，尝试创建路径
	if (err == -ENOENT) {
		create_path(nodename);
		err = vfs_path_lookup(dev_mnt->mnt_root, dev_mnt,
				      nodename, LOOKUP_PARENT, &nd);	// 再次查找
	}
	// 如果有错误，跳转到出错处理
	if (err)
		goto out;

	// 创建目录项
	dentry = lookup_create(&nd, 0);
	if (!IS_ERR(dentry)) {	// 如果目录项创建成功
		// 创建设备节点
		err = vfs_mknod(nd.path.dentry->d_inode,
				dentry, mode, dev->devt);
		if (!err) {	// 如果设备节点创建成功
			struct iattr newattrs;

			/* fixup possibly umasked mode */
			// 设置文件属性，例如修改由 umask 影响的模式
			newattrs.ia_mode = mode;	// 设置新属性的模式
			newattrs.ia_valid = ATTR_MODE;	// 指定模式有效
			mutex_lock(&dentry->d_inode->i_mutex);
			notify_change(dentry, &newattrs);	// 通知系统属性变化
			mutex_unlock(&dentry->d_inode->i_mutex);

			/* mark as kernel-created inode */
			// 标记为内核创建的 inode
			dentry->d_inode->i_private = &dev_mnt;
		}
		dput(dentry);	// 减少目录项的引用计数，释放目录项
	} else {
		err = PTR_ERR(dentry);	// 获取错误代码
	}

	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);	// 解锁并释放路径
	path_put(&nd.path);
out:
	// 清理临时变量，恢复原始凭证，返回错误代码
	kfree(tmp);
	revert_creds(curr_cred);
	return err;
}

// 负责在 devtmpfs 文件系统中删除一个目录
static int dev_rmdir(const char *name)
{
	struct nameidata nd; // 结构体用于存储路径查找过程中的信息
	struct dentry *dentry; // 目录项结构体
	int err;

	// 查找给定名称的目录的父目录
	err = vfs_path_lookup(dev_mnt->mnt_root, dev_mnt,
			      name, LOOKUP_PARENT, &nd);
	if (err)
		return err;	// 如果查找失败，返回错误码

	// 锁定找到的目录项的父目录项的互斥锁
	mutex_lock_nested(&nd.path.dentry->d_inode->i_mutex, I_MUTEX_PARENT);
	// 查找目录项中最后一个组件的名字对应的目录项
	dentry = lookup_one_len(nd.last.name, nd.path.dentry, nd.last.len);
	if (!IS_ERR(dentry)) {	// 如果查找成功
		if (dentry->d_inode) {	// 如果目录项有对应的inode
			if (dentry->d_inode->i_private == &dev_mnt)
				// 如果是由devtmpfs创建的inode，尝试删除目录
				err = vfs_rmdir(nd.path.dentry->d_inode,
						dentry);
			else
				err = -EPERM;	// 如果不是，返回没有权限的错误
		} else {
			err = -ENOENT;	// 如果没有找到inode，返回不存在的错误
		}
		dput(dentry);	 // 减少目录项的引用计数
	} else {
		err = PTR_ERR(dentry);	// 获取错误码
	}

	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);	// 解锁
	path_put(&nd.path);	// 释放路径
	return err;	// 返回结果
}

// 负责递归删除一个目录路径中的所有目录
static int delete_path(const char *nodepath)
{
	const char *path;
	int err = 0;

	path = kstrdup(nodepath, GFP_KERNEL);	// 复制传入的路径字符串，使用内核的常规内存分配标志
	if (!path)
		return -ENOMEM;	// 如果内存分配失败，返回内存不足的错误码

	mutex_lock(&dirlock);	// 锁定目录操作的互斥锁，确保目录操作的原子性
	for (;;) {
		char *base;

		base = strrchr(path, '/');	// 从后向前搜索路径中的第一个'/'字符
		if (!base)
			break;	// 如果没有找到'/'，说明已到达路径的开头，退出循环
		base[0] = '\0';	// 将找到的'/'字符替换为字符串结束符，从而截断路径
		err = dev_rmdir(path);	// 尝试删除截断后的路径（目录）
		if (err)
			break;	// 如果删除失败，跳出循环
	}
	mutex_unlock(&dirlock);	// 解锁目录操作的互斥锁

	kfree(path);	// 释放之前复制的路径字符串占用的内存
	return err;		// 返回操作结果，如果成功则为0，失败则为对应的错误码
}

// 用于检查设备和inode是否匹配的函数
static int dev_mynode(struct device *dev, struct inode *inode, struct kstat *stat)
{
	/* did we create it */
	if (inode->i_private != &dev_mnt)
		return 0;	// 如果inode没有由devtmpfs创建，则直接返回0

	/* does the dev_t match */
	if (is_blockdev(dev)) {	// 检查设备是否为块设备
		if (!S_ISBLK(stat->mode))
			return 0;	// 如果设备是块设备，但inode的类型不是块设备，返回0
	} else {
		if (!S_ISCHR(stat->mode))
			return 0;	// 如果设备不是块设备，但inode的类型不是字符设备，返回0
	}
	if (stat->rdev != dev->devt)
		return 0;	// 如果设备的设备号不匹配，返回0

	/* ours */
	return 1;	// 如果所有检查都通过，返回1，表示匹配成功
}

// 用于删除设备文件节点的功能实现
int devtmpfs_delete_node(struct device *dev)
{
	const char *tmp = NULL;
	const char *nodename;
	const struct cred *curr_cred;
	struct nameidata nd;
	struct dentry *dentry;
	struct kstat stat;
	int deleted = 1;
	int err;

	if (!dev_mnt)	// 如果设备挂载点不存在
		return 0;		// 直接返回0

	nodename = device_get_devnode(dev, NULL, &tmp);	// 获取设备节点名称
	if (!nodename)	// 如果无法获取节点名称
		return -ENOMEM;	// 返回内存不足错误

	curr_cred = override_creds(&init_cred);  // 临时覆盖当前的凭证为初始凭证
	err = vfs_path_lookup(dev_mnt->mnt_root, dev_mnt,  // 在设备的文件系统中查找路径
			      nodename, LOOKUP_PARENT, &nd);  // 搜索父目录
	if (err)  // 如果路径查找失败
		goto out;  // 跳转到错误处理代码

	mutex_lock_nested(&nd.path.dentry->d_inode->i_mutex, I_MUTEX_PARENT);	// 锁定父目录节点
	dentry = lookup_one_len(nd.last.name, nd.path.dentry, nd.last.len);	// 查找目标文件节点
	if (!IS_ERR(dentry)) {	// 如果节点查找成功
		if (dentry->d_inode) {	// 如果目录项有对应的inode结构
			err = vfs_getattr(nd.path.mnt, dentry, &stat);	// 获取文件状态信息
			if (!err && dev_mynode(dev, dentry->d_inode, &stat)) {	// 如果属性获取成功并且节点确实属于当前设备
				struct iattr newattrs;
				/*
				 * before unlinking this node, reset permissions
				 * of possible references like hardlinks
				 */
				// 在删除节点之前，重置可能存在的硬链接的权限
				newattrs.ia_uid = 0;
				newattrs.ia_gid = 0;
				newattrs.ia_mode = stat.mode & ~0777;	// 重置权限，只保留非权限部分的模式位
				newattrs.ia_valid =
					ATTR_UID|ATTR_GID|ATTR_MODE;
				mutex_lock(&dentry->d_inode->i_mutex);
				notify_change(dentry, &newattrs);	// 通知系统属性已改变
				mutex_unlock(&dentry->d_inode->i_mutex);
				err = vfs_unlink(nd.path.dentry->d_inode,	// 删除文件节点
						 dentry);
				if (!err || err == -ENOENT)	// 如果删除成功或文件不存在
					deleted = 1;	// 标记已删除
			}
		} else {
			err = -ENOENT;	// 文件不存在
		}
		dput(dentry);	// 释放路径查找引用
	} else {
		err = PTR_ERR(dentry);	// 获取错误信息
	}
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);	// 解锁

	path_put(&nd.path);	// 释放路径
	if (deleted && strchr(nodename, '/'))	// 如果已删除且路径包含子目录
		delete_path(nodename);	// 删除相关路径
out:
	kfree(tmp);	// 释放临时存储的节点名称
	revert_creds(curr_cred);	// 恢复之前的凭证
	return err;	// 返回操作结果
}

/*
 * If configured, or requested by the commandline, devtmpfs will be
 * auto-mounted after the kernel mounted the root filesystem.
 */
/*
 * 如果配置了自动挂载或通过命令行请求，内核在挂载根文件系统后将自动挂载devtmpfs。
 */
// 实现了在内核挂载根文件系统后自动挂载devtmpfs的功能。
int devtmpfs_mount(const char *mntdir)
{
	int err;

	if (!dev_mount)	// 检查是否启用了devtmpfs的自动挂载功能
		return 0;

	if (!dev_mnt)	// 检查devtmpfs是否已经挂载
		return 0;

	// 尝试挂载devtmpfs到指定的目录
	err = sys_mount("devtmpfs", (char *)mntdir, "devtmpfs", MS_SILENT, NULL);
	if (err)	// 如果挂载失败
		printk(KERN_INFO "devtmpfs: error mounting %i\n", err);	// 打印错误信息
	else
		printk(KERN_INFO "devtmpfs: mounted\n");	// 打印挂载成功信息
	return err;	// 返回挂载操作的结果
}

/*
 * Create devtmpfs instance, driver-core devices will add their device
 * nodes here.
 */
/*
 * 创建devtmpfs实例，驱动核心设备将在此添加它们的设备节点。
 */
int __init devtmpfs_init(void)
{
	int err;  // 用于存储错误代码
	struct vfsmount *mnt;  // 挂载点指针
	char options[] = "mode=0755";  // 设置文件系统的默认权限为0755

	// 尝试注册devtmpfs文件系统
	err = register_filesystem(&dev_fs_type);
	if (err) {	// 如果注册失败
		printk(KERN_ERR "devtmpfs: unable to register devtmpfs "
		       "type %i\n", err);	// 打印错误信息
		return err;	// 返回错误代码
	}

	// 尝试挂载devtmpfs文件系统，使用上面定义的模式选项
	mnt = kern_mount_data(&dev_fs_type, options);
	if (IS_ERR(mnt)) {  // 如果挂载失败
		err = PTR_ERR(mnt);  // 获取错误代码
		printk(KERN_ERR "devtmpfs: unable to create devtmpfs %i\n", err);  // 打印错误信息
		unregister_filesystem(&dev_fs_type);  // 注销已注册的文件系统
		return err;  // 返回错误代码
	}
	dev_mnt = mnt;  // 保存挂载点指针

	printk(KERN_INFO "devtmpfs: initialized\n");  // 打印初始化成功信息
	return 0;  // 返回成功
}
