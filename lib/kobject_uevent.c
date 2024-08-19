/*
 * kernel userspace event delivery
 *
 * Copyright (C) 2004 Red Hat, Inc.  All rights reserved.
 * Copyright (C) 2004 Novell, Inc.  All rights reserved.
 * Copyright (C) 2004 IBM, Inc. All rights reserved.
 *
 * Licensed under the GNU GPL v2.
 *
 * Authors:
 *	Robert Love		<rml@novell.com>
 *	Kay Sievers		<kay.sievers@vrfy.org>
 *	Arjan van de Ven	<arjanv@redhat.com>
 *	Greg Kroah-Hartman	<greg@kroah.com>
 */

#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <linux/socket.h>
#include <linux/skbuff.h>
#include <linux/netlink.h>
#include <net/sock.h>

u64 uevent_seqnum;  // 定义一个全局的 uevent 序列号，用于跟踪每个事件的唯一序号
char uevent_helper[UEVENT_HELPER_PATH_LEN] = CONFIG_UEVENT_HELPER_PATH; // 定义一个数组存储 uevent 辅助程序的路径，该路径在配置时由 CONFIG_UEVENT_HELPER_PATH 指定
static DEFINE_SPINLOCK(sequence_lock); // 静态定义一个自旋锁，用于保护对 uevent 序列号的访问
#if defined(CONFIG_NET) // 如果定义了 CONFIG_NET（网络支持）
static struct sock *uevent_sock; // 定义一个指向套接字的指针，用于网络通信中处理 uevent
#endif

/* the strings here must match the enum in include/linux/kobject.h */
static const char *kobject_actions[] = { // 定义一个静态字符串数组，存储与 kobject 状态改变相关的动作描述
	[KOBJ_ADD] =		"add",        // 对应添加操作
	[KOBJ_REMOVE] =		"remove",     // 对应移除操作
	[KOBJ_CHANGE] =		"change",     // 对应改变操作
	[KOBJ_MOVE] =		"move",       // 对应移动操作
	[KOBJ_ONLINE] =		"online",     // 对应上线操作
	[KOBJ_OFFLINE] =	"offline",    // 对应下线操作
};

/**
 * kobject_action_type - translate action string to numeric type
 *
 * @buf: buffer containing the action string, newline is ignored
 * @len: length of buffer
 * @type: pointer to the location to store the action type
 *
 * Returns 0 if the action string was recognized.
 */
/**
 * kobject_action_type - 将动作字符串转换为数字类型
 *
 * @buf: 包含动作字符串的缓冲区，忽略换行符
 * @len: 缓冲区长度
 * @type: 指向存储动作类型位置的指针
 *
 * 如果动作字符串被识别，返回 0。
 */
int kobject_action_type(const char *buf, size_t count,
			enum kobject_action *type)
{
	enum kobject_action action; // 用于存储临时的动作类型
	int ret = -EINVAL; // 初始化返回值为 -EINVAL，表示无效参数

	// 去除字符串末尾的换行符或空字符
	if (count && (buf[count-1] == '\n' || buf[count-1] == '\0'))
		count--;

	// 如果处理后的字符串长度为0，跳转到 out 标签
	if (!count)
		goto out;

	// 遍历所有可能的动作类型
	for (action = 0; action < ARRAY_SIZE(kobject_actions); action++) {
		// 比较字符串，如果不匹配继续下一个
		if (strncmp(kobject_actions[action], buf, count) != 0)
			continue;
		// 确保完全匹配，即后续没有多余的字符
		if (kobject_actions[action][count] != '\0')
			continue;
		
		// 找到匹配，设置返回类型并更新返回值为 0
		*type = action;
		ret = 0;
		break;
	}
out:
	return ret; // 返回结果，成功为 0，失败为 -EINVAL
}

/**
 * kobject_uevent_env - send an uevent with environmental data
 *
 * @action: action that is happening
 * @kobj: struct kobject that the action is happening to
 * @envp_ext: pointer to environmental data
 *
 * Returns 0 if kobject_uevent() is completed with success or the
 * corresponding error when it fails.
 */
/**
 * kobject_uevent_env - 发送带有环境数据的 uevent
 *
 * @action: 发生的动作
 * @kobj: 发生动作的 struct kobject
 * @envp_ext: 指向环境数据的指针
 *
 * 如果 kobject_uevent() 成功完成返回 0，失败时返回对应的错误码。
 */
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action,
		       char *envp_ext[])
{
	struct kobj_uevent_env *env; // uevent 环境变量缓冲区
	const char *action_string = kobject_actions[action]; // 根据动作类型获取动作的字符串描述
	const char *devpath = NULL; // 设备路径字符串，初始化为空
	const char *subsystem; // 子系统名
	struct kobject *top_kobj; // 用于寻找最顶层的 kobject
	struct kset *kset; // kobject 所属的 kset
	const struct kset_uevent_ops *uevent_ops; // kset 的 uevent 操作函数
	u64 seq; // 事件序列号
	int i = 0; // 循环计数
	int retval = 0; // 返回值

	// 打印调试信息，包括 kobject 的名称、指针和当前函数
	pr_debug("kobject: '%s' (%p): %s\n",
		 kobject_name(kobj), kobj, __func__);

	/* search the kset we belong to */
	/* 寻找我们所属的 kset */
	// 寻找该 kobject 所属的 kset，首先将当前 kobject 赋值给 top_kobj
	top_kobj = kobj;	// 查找属于的 kset
	// 如果当前 kobject 没有 kset 并且有父 kobject，则向上遍历
	while (!top_kobj->kset && top_kobj->parent)
		top_kobj = top_kobj->parent;	// 向上遍历找到最顶层的 kobject

	// 如果最顶层的 kobject 也没有 kset，输出调试信息并返回错误
	if (!top_kobj->kset) {	// 如果顶层 kobject 不属于任何 kset
		pr_debug("kobject: '%s' (%p): %s: attempted to send uevent "
			 "without kset!\n", kobject_name(kobj), kobj,
			 __func__);
		return -EINVAL;	// 返回错误
	}

	// 获取 kset 和 uevent 操作
	kset = top_kobj->kset;	// 获取 kset
	uevent_ops = kset->uevent_ops;	// 获取 uevent 操作接口

	/* skip the event, if uevent_suppress is set*/
	/* 如果 uevent_suppress 被设置，跳过事件 */
	if (kobj->uevent_suppress) {	// 如果设置了禁止发送事件
		pr_debug("kobject: '%s' (%p): %s: uevent_suppress "
				 "caused the event to drop!\n",
				 kobject_name(kobj), kobj, __func__);
		return 0;	// 返回0表示事件被成功抑制
	}
	/* skip the event, if the filter returns zero. */
	/* 如果过滤器返回零，跳过事件 */
	if (uevent_ops && uevent_ops->filter)	// 如果设置了过滤器
		if (!uevent_ops->filter(kset, kobj)) {	// 并且过滤器返回 false
			pr_debug("kobject: '%s' (%p): %s: filter function "
				 "caused the event to drop!\n",
				 kobject_name(kobj), kobj, __func__);
			return 0;	// 返回0表示事件不符合过滤条件，被丢弃
		}

	/* originating subsystem */
	/* 获取起源子系统 */
	if (uevent_ops && uevent_ops->name) // 如果设置了子系统名获取函数
		subsystem = uevent_ops->name(kset, kobj); // 获取子系统名
	else
		subsystem = kobject_name(&kset->kobj); // 默认使用 kset 的 kobject 名作为子系统名
	if (!subsystem) { // 如果子系统名为空
		pr_debug("kobject: '%s' (%p): %s: unset subsystem caused the event to drop!\n",
							kobject_name(kobj), kobj, __func__);
		return 0; // 如果子系统未设置，返回0并放弃发送事件
	}

	/* environment buffer */
	/* 环境缓冲区 */
	env = kzalloc(sizeof(struct kobj_uevent_env), GFP_KERNEL);	// 分配环境变量结构体
	if (!env)
		return -ENOMEM;	// 内存分配失败

	/* complete object path */
	/* 获取完整的对象路径 */
	devpath = kobject_get_path(kobj, GFP_KERNEL);	// 获取 kobject 的路径
	if (!devpath) {
		retval = -ENOENT;	// 如果获取路径失败，设置返回值为不存在错误
		goto exit;	// 跳转到函数退出处理
	}

	/* default keys */
	/* 默认键 */
	// 向环境变量中添加动作类型
	retval = add_uevent_var(env, "ACTION=%s", action_string);	// 添加 ACTION 环境变量
	if (retval)	// 如果添加失败
		goto exit;	// 跳转到函数退出处理
	// 添加设备路径
	retval = add_uevent_var(env, "DEVPATH=%s", devpath);			// 添加 DEVPATH 环境变量
	if (retval)
		goto exit;
	// 添加子系统名称
	retval = add_uevent_var(env, "SUBSYSTEM=%s", subsystem);	// 添加 SUBSYSTEM 环境变量
	if (retval)
		goto exit;

	/* keys passed in from the caller */
	/* 调用者传入的键 */
	// 如果存在额外的环境变量
	if (envp_ext) {	// 如果有额外的环境变量
		// 遍历所有额外环境变量
		for (i = 0; envp_ext[i]; i++) {	// 遍历并添加
			retval = add_uevent_var(env, "%s", envp_ext[i]);	// 添加到环境变量中
			if (retval)	// 如果添加失败
				goto exit;	// 跳转到函数退出处理
		}
	}

	/* let the kset specific function add its stuff */
	/* 让 kset 特定函数添加它的内容 */
	if (uevent_ops && uevent_ops->uevent) {	// 如果设置了 uevent 函数
		retval = uevent_ops->uevent(kset, kobj, env);	// 调用该函数
		if (retval) {	// 如果函数返回错误
			pr_debug("kobject: '%s' (%p): %s: uevent() returned "
				 "%d\n", kobject_name(kobj), kobj,
				 __func__, retval);	// 打印调试信息
			goto exit;	// 跳转到函数退出处理
		}
	}

	/*
	 * Mark "add" and "remove" events in the object to ensure proper
	 * events to userspace during automatic cleanup. If the object did
	 * send an "add" event, "remove" will automatically generated by
	 * the core, if not already done by the caller.
	 */
	/*
	 * 在对象中标记“add”和“remove”事件，以确保在自动清理期间向用户空间发送正确的事件。
	 * 如果对象发送了“add”事件，“remove”将由核心自动生成，除非调用者已经完成。
	 */
	/* 标记 "add" 和 "remove" 事件以确保在自动清理期间向用户空间发送正确的事件 */
	if (action == KOBJ_ADD) // 如果事件是添加事件
		kobj->state_add_uevent_sent = 1; // 标记该 kobject 已发送添加事件
	else if (action == KOBJ_REMOVE) // 如果事件是移除事件
		kobj->state_remove_uevent_sent = 1; // 标记该 kobject 已发送移除事件

	/* we will send an event, so request a new sequence number */
	/* 我们将发送事件，因此请求一个新的序列号 */
	spin_lock(&sequence_lock); // 加锁以安全地更新序列号
	seq = ++uevent_seqnum; // 增加全局事件序列号
	spin_unlock(&sequence_lock); // 解锁
	// 将序列号添加到环境变量中
	retval = add_uevent_var(env, "SEQNUM=%llu", (unsigned long long)seq); // 添加 SEQNUM 环境变量
	if (retval)	// 如果添加失败
		goto exit;	// 跳转到退出处理代码

#if defined(CONFIG_NET)	// 如果定义了 CONFIG_NET（启用了网络支持）
	/* send netlink message */
	/* 发送网络消息 */
	if (uevent_sock) {	// 如果存在用于发送 uevent 的套接字
		struct sk_buff *skb;	// 声明一个指向 sk_buff 结构的指针，用于网络消息
		size_t len;	// 用于计算消息长度

		/* allocate message with the maximum possible size */
		/* 为消息分配最大可能大小的缓冲区 */
		len = strlen(action_string) + strlen(devpath) + 2;	// 计算需要的长度，包括动作字符串、设备路径和两个字符的间隔
		skb = alloc_skb(len + env->buflen, GFP_KERNEL);	// 分配一个足够大的 skb 缓冲区
		if (skb) {	// 如果分配成功
			char *scratch;	// 临时字符指针

			/* add header */
			/* 添加头部 */
			scratch = skb_put(skb, len);	// 在 skb 中预留出足够的空间
			// 格式化并写入动作和设备路径
			sprintf(scratch, "%s@%s", action_string, devpath);	// 格式化缓冲区内容

			/* copy keys to our continuous event payload buffer */
			/* 将键复制到我们的连续事件负载缓冲区 */
			// 遍历所有环境变量
			for (i = 0; i < env->envp_idx; i++) {	// 将环境变量复制到缓冲区
				len = strlen(env->envp[i]) + 1;  // 计算每个环境变量的长度
				scratch = skb_put(skb, len);  // 在 skb 中预留出空间
				strcpy(scratch, env->envp[i]);  // 复制环境变量
			}

			NETLINK_CB(skb).dst_group = 1;	// 设置目标组为 1，表示广播组
			// 广播 skb
			retval = netlink_broadcast(uevent_sock, skb, 0, 1,	// 广播事件
						   GFP_KERNEL);
			/* ENOBUFS should be handled in userspace */
			/* ENOBUFS 应该在用户空间处理 */
			if (retval == -ENOBUFS)	// 如果返回 ENOBUFS（缓冲区不足），则在用户空间处理
				retval = 0;
		} else
			retval = -ENOMEM;	// 如果分配失败，返回内存不足错误
	}
#endif

	/* call uevent_helper, usually only enabled during early boot */
	/* 调用 uevent_helper，通常只在早期启动时启用 */
	// 如果 uevent_helper 字符串被设置
	if (uevent_helper[0]) {	// 如果有 helper 程序
		char *argv[3];  // 创建一个参数数组用于执行用户模式帮助程序

		argv[0] = uevent_helper;  // 帮助程序的路径
		argv[1] = (char *)subsystem;  // 传递子系统作为参数
		argv[2] = NULL;  // 参数列表结束标志

		// 添加环境变量 HOME
		retval = add_uevent_var(env, "HOME=/");
		if (retval)  // 如果添加环境变量失败
			goto exit;  // 跳转到退出处理代码

		// 添加环境变量 PATH
		retval = add_uevent_var(env, "PATH=/sbin:/bin:/usr/sbin:/usr/bin");
		if (retval)  // 如果添加环境变量失败
			goto exit;  // 跳转到退出处理代码

		// 调用用户模式帮助程序
		retval = call_usermodehelper(argv[0], argv, env->envp, UMH_WAIT_EXEC);
	}

exit:
	kfree(devpath); // 释放设备路径
	kfree(env); // 释放环境变量结构体
	return retval; // 返回处理结果
}
EXPORT_SYMBOL_GPL(kobject_uevent_env);

/**
 * kobject_uevent - notify userspace by ending an uevent
 *
 * @action: action that is happening
 * @kobj: struct kobject that the action is happening to
 *
 * Returns 0 if kobject_uevent() is completed with success or the
 * corresponding error when it fails.
 */
/**
 * kobject_uevent - 通过发送一个 uevent 通知用户空间
 *
 * @action: 正在发生的动作
 * @kobj: 动作发生的 kobject
 *
 * 如果 kobject_uevent() 完成并成功则返回 0，失败则返回相应的错误。
 */
int kobject_uevent(struct kobject *kobj, enum kobject_action action)
{
	return kobject_uevent_env(kobj, action, NULL);  // 调用 kobject_uevent_env 函数，不传递额外的环境数据
}
EXPORT_SYMBOL_GPL(kobject_uevent);

/**
 * add_uevent_var - add key value string to the environment buffer
 * @env: environment buffer structure
 * @format: printf format for the key=value pair
 *
 * Returns 0 if environment variable was added successfully or -ENOMEM
 * if no space was available.
 */
/**
 * add_uevent_var - 将键值字符串添加到环境缓冲区
 * @env: 环境缓冲区结构
 * @format: 键值对的 printf 格式
 *
 * 如果环境变量成功添加则返回0，如果没有可用空间则返回 -ENOMEM。
 */
int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...)
{
	va_list args;  // 定义一个用于存储可变参数的变量
	int len;  // 用于存储格式化字符串后的长度

	if (env->envp_idx >= ARRAY_SIZE(env->envp)) {  // 检查环境变量索引是否超出数组大小
		WARN(1, KERN_ERR "add_uevent_var: too many keys\n");  // 如果超出，打印警告信息
		return -ENOMEM;  // 并返回内存不足错误
	}

	va_start(args, format);  // 初始化 args 以获取可变参数列表
	len = vsnprintf(&env->buf[env->buflen],  // 将格式化的字符串写入环境缓冲区
			sizeof(env->buf) - env->buflen,  // 计算缓冲区剩余空间
			format, args);  // 格式化字符串
	va_end(args);  // 结束可变参数的获取

	if (len >= (sizeof(env->buf) - env->buflen)) {  // 检查格式化后的字符串是否超出缓冲区剩余空间
		WARN(1, KERN_ERR "add_uevent_var: buffer size too small\n");  // 如果超出，打印警告信息
		return -ENOMEM;  // 并返回内存不足错误
	}

	env->envp[env->envp_idx++] = &env->buf[env->buflen];  // 将新的环境变量的指针添加到索引数组
	env->buflen += len + 1;  // 更新缓冲区已使用的长度
	return 0;  // 返回成功
}
EXPORT_SYMBOL_GPL(add_uevent_var);

#if defined(CONFIG_NET)  // 如果启用了网络支持
static int __init kobject_uevent_init(void)
{
    // 创建一个 netlink 套接字用于 kobject uevent
	uevent_sock = netlink_kernel_create(&init_net, NETLINK_KOBJECT_UEVENT,
					    1, NULL, NULL, THIS_MODULE);
	if (!uevent_sock) {  // 如果套接字创建失败
		printk(KERN_ERR
		       "kobject_uevent: unable to create netlink socket!\n");  // 打印错误信息
		return -ENODEV;  // 返回设备不存在的错误
	}
	netlink_set_nonroot(NETLINK_KOBJECT_UEVENT, NL_NONROOT_RECV);  // 允许非 root 用户接收该 netlink 消息
	return 0;  // 成功初始化返回0
}

postcore_initcall(kobject_uevent_init);  // 将 kobject_uevent_init 函数注册为内核初始化调用之后执行
#endif
