#ifndef _LINUX_INETDEVICE_H
#define _LINUX_INETDEVICE_H

#ifdef __KERNEL__

#include <linux/bitmap.h>
#include <linux/if.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/timer.h>
#include <linux/sysctl.h>

// 用于表示 IPv4 相关的设备配置选项。这些选项通常用于网络设备或接口的配置，影响 IP 层的行为。
/* 
 * 枚举值被用来配置网络设备或接口的行为，如是否允许 IP 数据包转发、是否处理 ARP 请求、是否记录异常数据包等。
 * 通过设置这些参数，系统管理员可以对网络接口的行为进行精细控制，以适应不同的网络需求和安全策略。
 */
enum
{
	IPV4_DEVCONF_FORWARDING=1,  // 启用或禁用 IPv4 转发
	IPV4_DEVCONF_MC_FORWARDING, // 启用或禁用多播转发
	IPV4_DEVCONF_PROXY_ARP,     // 启用或禁用代理 ARP
	IPV4_DEVCONF_ACCEPT_REDIRECTS, // 是否接受 ICMP 重定向
	IPV4_DEVCONF_SECURE_REDIRECTS, // 是否仅接受来自网关列表的安全重定向
	IPV4_DEVCONF_SEND_REDIRECTS,    // 是否发送 ICMP 重定向
	IPV4_DEVCONF_SHARED_MEDIA,      // 是否考虑多个网络设备共享同一物理介质
	IPV4_DEVCONF_RP_FILTER,         // 启用或禁用反向路径过滤
	IPV4_DEVCONF_ACCEPT_SOURCE_ROUTE, // 是否接受源路由的数据包
	IPV4_DEVCONF_BOOTP_RELAY,       // 启用或禁用 BOOTP 中继功能
	IPV4_DEVCONF_LOG_MARTIANS,      // 记录来自不可达源地址的数据包
	IPV4_DEVCONF_TAG,               // 设备标签（用于路由选择）
	IPV4_DEVCONF_ARPFILTER,         // ARP 过滤
	IPV4_DEVCONF_MEDIUM_ID,         // 媒体标识符
	IPV4_DEVCONF_NOXFRM,            // 禁用 XFRM 转换
	IPV4_DEVCONF_NOPOLICY,          // 禁用策略路由
	IPV4_DEVCONF_FORCE_IGMP_VERSION, // 强制 IGMP 版本
	IPV4_DEVCONF_ARP_ANNOUNCE,      // 控制 ARP 公告的发送
	IPV4_DEVCONF_ARP_IGNORE,        // 控制对 ARP 请求的响应
	IPV4_DEVCONF_PROMOTE_SECONDARIES, // 优先使用辅助 IP 地址
	IPV4_DEVCONF_ARP_ACCEPT,        // 接受 ARP 请求
	IPV4_DEVCONF_ARP_NOTIFY,        // 在 IP 地址变更时发送 ARP 通知
	IPV4_DEVCONF_ACCEPT_LOCAL,      // 接受发往本地地址的数据包
	IPV4_DEVCONF_SRC_VMARK,         // 源验证标记
	IPV4_DEVCONF_PROXY_ARP_PVLAN,   // 代理 ARP 私有 VLAN
	__IPV4_DEVCONF_MAX              // 用于标记枚举的结束
};

// 用于存储 IPv4 设备配置的状态和设置。
struct ipv4_devconf {
	void	*sysctl;  // 指针，通常用于指向与系统控制相关的数据结构
	int	data[__IPV4_DEVCONF_MAX - 1];  // 存储具体的设备配置选项值的数组
	DECLARE_BITMAP(state, __IPV4_DEVCONF_MAX - 1);  // 声明一个位图，用于跟踪每个设备配置选项的状态
};

/*
 * dev: 指向网络设备的指针，通常用于获取设备的物理属性和状态信息。
 * refcnt: 用原子操作管理的引用计数，确保该结构体在多线程环境下安全使用。
 * dead: 用于标记该设备实例是否已经不再使用，通常在设备被卸载或禁用时设置。
 * ifa_list: IPv4地址信息列表，用于存储该设备的所有 IPv4 地址。
 * mc_list, mc_tomb: 管理该设备的 IP 多播成员资格，包括当前活动的多播列表和待删除的多播列表。
 * mr_*: 与 IGMP 协议相关的多播监听状态和定时器。
 * arp_parms: 存储与 ARP 协议相关的参数，例如超时和重试次数。
 * cnf: 存储 IPv4 相关的设备配置，如是否启用转发、如何处理源路由等。
 * 
 * 使用场景：
 * in_device 结构体在网络设备配置 IPv4 时使用，如在设备启动、运行或停止时。
 * 它也用于网络事件处理，例如响应 ARP 请求、处理 IP 多播和 IGMP 报文。
 */
struct in_device {
	struct net_device	*dev;	// 指向关联的网络设备的指针
	atomic_t		refcnt;			// 引用计数，用于管理此结构的生命周期
	int			dead;	// 标记该结构是否不再使用
	// 指向 IPv4 地址链表的指针
	struct in_ifaddr	*ifa_list;	/* IP ifaddr chain		*/
	// 保护多播列表的读写锁
	rwlock_t		mc_list_lock;
	// 指向多播过滤链表的指针
	struct ip_mc_list	*mc_list;	/* IP multicast filter chain    */
	// 已安装的多播地址数量
	int			mc_count;	          /* Number of installed mcasts	*/
	spinlock_t		mc_tomb_lock;	// 保护 mc_tomb 的自旋锁
	struct ip_mc_list	*mc_tomb;             // 用于临时存放待删除的多播列表
	unsigned long		mr_v1_seen;           // 记录最近一次看到 IGMPv1 报文的时间
	unsigned long		mr_v2_seen;           // 记录最近一次看到 IGMPv2 报文的时间
	unsigned long		mr_maxdelay;          // 最大查询响应时间
	unsigned char		mr_qrv;               // 查询的健壮性变量
	unsigned char		mr_gq_running;        // 标记是否正在运行常规查询
	unsigned char		mr_ifc_count;         // 接口变更次数计数
	// 用于常规查询的计时器
	struct timer_list	mr_gq_timer;	/* general query timer */
	// 用于接口变更的计时器
	struct timer_list	mr_ifc_timer;	/* interface change timer */

	struct neigh_parms	*arp_parms;           // 指向与 ARP 相关的参数的指针
	struct ipv4_devconf	cnf;                  // 存储设备级别的 IPv4 配置
	struct rcu_head		rcu_head;             // 用于在 RCU 读取期间保护该结构的头部
};

/* 一个宏定义，用于访问给定 ipv4_devconf 结构中指定属性的配置值。cnf 是 ipv4_devconf 结构，attr 是属性名前缀。 */
#define IPV4_DEVCONF(cnf, attr) ((cnf).data[IPV4_DEVCONF_ ## attr - 1])
/* 一个宏定义，用于访问指定网络命名空间中所有设备的指定 IPv4 配置属性。net 指向 net 命名空间结构，attr 是配置属性的名称。 */
#define IPV4_DEVCONF_ALL(net, attr) \
	IPV4_DEVCONF((*(net)->ipv4.devconf_all), attr)

/* 内联函数，用于获取给定网络设备的指定索引的 IPv4 配置值。 */
static inline int ipv4_devconf_get(struct in_device *in_dev, int index)
{
	index--;  // 调整索引以适应从 0 开始的数组索引
	return in_dev->cnf.data[index];  // 返回指定索引处的配置值
}

/* 内联函数，用于设置给定网络设备的指定索引的 IPv4 配置值。 */
static inline void ipv4_devconf_set(struct in_device *in_dev, int index,
				    int val)
{
	index--;  // 调整索引
	set_bit(index, in_dev->cnf.state);  // 在状态位图中设置相应位，标记该配置项已被设置
	in_dev->cnf.data[index] = val;  // 更新配置值
}

/* 内联函数，用于设置给定网络设备的所有 IPv4 配置项为已设置状态。 */
static inline void ipv4_devconf_setall(struct in_device *in_dev)
{
	bitmap_fill(in_dev->cnf.state, __IPV4_DEVCONF_MAX - 1);	// 填充整个状态位图，标记所有配置项为已设置
}

/* 获取指定网络设备in_dev的特定IPv4配置属性attr的值。*/
#define IN_DEV_CONF_GET(in_dev, attr) \
	ipv4_devconf_get((in_dev), IPV4_DEVCONF_ ## attr)
/* 设置指定网络设备in_dev的特定IPv4配置属性attr为给定的值val。*/
#define IN_DEV_CONF_SET(in_dev, attr, val) \
	ipv4_devconf_set((in_dev), IPV4_DEVCONF_ ## attr, (val))

/* 逻辑与操作：检查全局配置和指定网络设备的特定IPv4配置属性attr是否都被设置。*/
#define IN_DEV_ANDCONF(in_dev, attr) \
	(IPV4_DEVCONF_ALL(dev_net(in_dev->dev), attr) && \
	 IN_DEV_CONF_GET((in_dev), attr))
/* 逻辑或操作：检查全局配置或指定网络设备的特定IPv4配置属性attr是否至少有一个被设置。*/
#define IN_DEV_ORCONF(in_dev, attr) \
	(IPV4_DEVCONF_ALL(dev_net(in_dev->dev), attr) || \
	 IN_DEV_CONF_GET((in_dev), attr))
/* 取最大值：比较全局配置与指定网络设备的特定IPv4配置属性attr的值，返回较大者。*/
#define IN_DEV_MAXCONF(in_dev, attr) \
	(max(IPV4_DEVCONF_ALL(dev_net(in_dev->dev), attr), \
	     IN_DEV_CONF_GET((in_dev), attr)))

/* 获取指定网络设备in_dev是否启用了IP转发。*/
#define IN_DEV_FORWARD(in_dev)		IN_DEV_CONF_GET((in_dev), FORWARDING)
/* 检查指定网络设备in_dev是否同时在全局和设备级别启用了多播转发。*/
#define IN_DEV_MFORWARD(in_dev)		IN_DEV_ANDCONF((in_dev), MC_FORWARDING)
/* 获取指定网络设备in_dev的反向路径过滤设置，返回全局和设备级别中的最严格设置。*/
#define IN_DEV_RPFILTER(in_dev)		IN_DEV_MAXCONF((in_dev), RP_FILTER)
/* 检查指定网络设备in_dev是否在全局或设备级别启用了源验证标记。*/
#define IN_DEV_SRC_VMARK(in_dev)    	IN_DEV_ORCONF((in_dev), SRC_VMARK)
/* 检查指定网络设备in_dev是否同时在全局和设备级别接受源路由。*/
#define IN_DEV_SOURCE_ROUTE(in_dev)	IN_DEV_ANDCONF((in_dev), \
						       ACCEPT_SOURCE_ROUTE)
/* 检查指定网络设备in_dev是否在全局或设备级别接受本地来源的数据包。*/
#define IN_DEV_ACCEPT_LOCAL(in_dev)	IN_DEV_ORCONF((in_dev), ACCEPT_LOCAL)
/* 检查指定网络设备in_dev是否同时在全局和设备级别启用了BOOTP中继。*/
#define IN_DEV_BOOTP_RELAY(in_dev)	IN_DEV_ANDCONF((in_dev), BOOTP_RELAY)

/* 检查是否在全局或设备级别记录来自未知源的数据包（被称为"Martians"）。 */
#define IN_DEV_LOG_MARTIANS(in_dev)	IN_DEV_ORCONF((in_dev), LOG_MARTIANS)
/* 检查是否在全局或设备级别启用了ARP代理。 */
#define IN_DEV_PROXY_ARP(in_dev)	IN_DEV_ORCONF((in_dev), PROXY_ARP)
/* 获取设备级别的ARP代理PVLAN配置。 */
#define IN_DEV_PROXY_ARP_PVLAN(in_dev)	IN_DEV_CONF_GET(in_dev, PROXY_ARP_PVLAN)
/* 检查是否在全局或设备级别认为该设备位于共享媒介上。 */
#define IN_DEV_SHARED_MEDIA(in_dev)	IN_DEV_ORCONF((in_dev), SHARED_MEDIA)
/* 检查是否在全局或设备级别发送ICMP重定向。 */
#define IN_DEV_TX_REDIRECTS(in_dev)	IN_DEV_ORCONF((in_dev), SEND_REDIRECTS)
/* 检查是否在全局或设备级别启用了安全ICMP重定向。 */
#define IN_DEV_SEC_REDIRECTS(in_dev)	IN_DEV_ORCONF((in_dev), \
						      SECURE_REDIRECTS)
/* 获取指定设备的ID标签设置。 */
#define IN_DEV_IDTAG(in_dev)		IN_DEV_CONF_GET(in_dev, TAG)
/* 获取指定设备的介质ID。 */
#define IN_DEV_MEDIUM_ID(in_dev)	IN_DEV_CONF_GET(in_dev, MEDIUM_ID)
/* 检查是否在全局或设备级别推广辅助地址为主地址。 */
#define IN_DEV_PROMOTE_SECONDARIES(in_dev) \
					IN_DEV_ORCONF((in_dev), \
						      PROMOTE_SECONDARIES)

/* 根据设备是否转发数据包来决定是否接受ICMP重定向。如果设备转发数据包，那么全局和设备级别都必须接受重定向；
   如果设备不转发，那么只要全局或设备级别接受重定向即可。 */
#define IN_DEV_RX_REDIRECTS(in_dev) \
	((IN_DEV_FORWARD(in_dev) && \
	  IN_DEV_ANDCONF((in_dev), ACCEPT_REDIRECTS)) \
	 || (!IN_DEV_FORWARD(in_dev) && \
	  IN_DEV_ORCONF((in_dev), ACCEPT_REDIRECTS)))

/* 检查是否在全局或设备级别启用了ARP过滤。 */
#define IN_DEV_ARPFILTER(in_dev)	IN_DEV_ORCONF((in_dev), ARPFILTER)
/* 获取ARP通告的最严格设置（全局或设备级别中的较大值）。 */
#define IN_DEV_ARP_ANNOUNCE(in_dev)	IN_DEV_MAXCONF((in_dev), ARP_ANNOUNCE)
/* 获取ARP忽略的最严格设置（全局或设备级别中的较大值）。 */
#define IN_DEV_ARP_IGNORE(in_dev)	IN_DEV_MAXCONF((in_dev), ARP_IGNORE)
/* 获取ARP通知的最严格设置（全局或设备级别中的较大值）。 */
#define IN_DEV_ARP_NOTIFY(in_dev)	IN_DEV_MAXCONF((in_dev), ARP_NOTIFY)

// 这个结构体主要用于存储和管理网络接口的IPv4设置，包括地址、子网掩码、广播地址等，这些信息对于网络配置和管理至关重要。
// 描述网络设备的IPv4地址信息
struct in_ifaddr {
	struct in_ifaddr	*ifa_next;      /* 指向下一个同类结构的指针，形成链表 */
	struct in_device	*ifa_dev;       /* 指向关联的网络设备 */
	struct rcu_head		rcu_head;       /* 用于RCU同步删除 */
	__be32			ifa_local;      /* 本地接口地址 */
	__be32			ifa_address;    /* 主要的接口地址 */
	__be32			ifa_mask;       /* 网络掩码 */
	__be32			ifa_broadcast;  /* 广播地址 */
	unsigned char		ifa_scope;      /* 地址的作用范围（例如链接、主机） */
	unsigned char		ifa_flags;      /* 接口地址的标志，例如IFA_F_SECONDARY用于辅助地址 */
	unsigned char		ifa_prefixlen;  /* 掩码的前缀长度 */
	char			ifa_label[IFNAMSIZ]; /* 接口标签，通常为设备名 */
};

/* 注册一个网络地址变更的通知器。当网络接口的地址发生变化时，会通知这个注册的nb。*/
extern int register_inetaddr_notifier(struct notifier_block *nb);
/* 注销一个已注册的网络地址变更通知器。 */
extern int unregister_inetaddr_notifier(struct notifier_block *nb);

/* 在指定的网络命名空间中查找具有指定IP地址的网络设备。 */
extern struct net_device *ip_dev_find(struct net *net, __be32 addr);
/* 检查两个IP地址是否在同一个链路上。 */
extern int		inet_addr_onlink(struct in_device *in_dev, __be32 a, __be32 b);
/* 执行网络设备的IO控制操作，用于设备配置。 */
extern int		devinet_ioctl(struct net *net, unsigned int cmd, void __user *);
/* 初始化网络设备接口的IPv4部分。 */
extern void		devinet_init(void);
/* 通过网络设备的索引在指定网络命名空间中查找对应的网络设备接口。 */
extern struct in_device	*inetdev_by_index(struct net *, int);
/* 为给定的目的地址和范围选择最合适的本地IPv4地址。 */
extern __be32		inet_select_addr(const struct net_device *dev, __be32 dst, int scope);
/* 确认指定的IPv4地址是否有效，并返回相应的本地地址。 */
extern __be32		inet_confirm_addr(struct in_device *in_dev, __be32 dst, __be32 local, int scope);
/* 在给定设备中查找具有指定前缀和掩码的IPv4地址配置。 */
extern struct in_ifaddr *inet_ifa_byprefix(struct in_device *in_dev, __be32 prefix, __be32 mask);

/* 检查指定的IPv4地址是否与给定的地址配置（通过地址和掩码）匹配。 */
static __inline__ int inet_ifa_match(__be32 addr, struct in_ifaddr *ifa)
{
	return !((addr^ifa->ifa_address)&ifa->ifa_mask);
}

/*
 *	Check if a mask is acceptable.
 */

/* 声明一个静态内联函数 `bad_mask`，接收两个参数：`mask` 是网络掩码，`addr` 是对应的IP地址。 */
/*
 * 此函数用于验证给定的网络掩码是否合法。网络掩码的要求是，从左边开始应连续为1，到某一位后全部为0。
 * 例如，255.255.255.0 是合法的，而 255.0.255.0 则不是。这个函数通过对掩码的位操作来检查其合法性，确保掩码是连续的1后跟连续的0。
 */
static __inline__ int bad_mask(__be32 mask, __be32 addr)
{
	__u32 hmask;	/* 定义一个无符号32位整数 `hmask` 来存储转换为主机字节序的掩码。 */
	if (addr & (mask = ~mask))
		return 1;	/* 首先对掩码进行按位取反操作，然后与地址进行按位与操作。如果结果非零，说明地址与掩码不兼容，返回1。 */
	hmask = ntohl(mask);	/* 将网络字节序的掩码转换为主机字节序，存储在 `hmask` 中。 */
	if (hmask & (hmask+1))
		return 1;	/* 检查掩码是否为连续的1后跟连续的0，这是一个有效的子网掩码的标准形式。如果不是，返回1。 */
	return 0;	/* 如果掩码通过了上述所有检查，说明它是有效的，返回0。 */
}

// 这个宏用于遍历网络设备 in_dev 的所有主要（非次要）IPv4地址。它初始化一个指向 in_ifaddr 结构的指针 ifa，遍历 in_dev->ifa_list 链表，跳过标记为次要的地址（IFA_F_SECONDARY）。
#define for_primary_ifa(in_dev)	{ struct in_ifaddr *ifa; \
  for (ifa = (in_dev)->ifa_list; ifa && !(ifa->ifa_flags&IFA_F_SECONDARY); ifa = ifa->ifa_next)

// 这个宏用于遍历网络设备 in_dev 的所有IPv4地址。它初始化一个指向 in_ifaddr 结构的指针 ifa 并遍历整个 in_dev->ifa_list 链表。
#define for_ifa(in_dev)	{ struct in_ifaddr *ifa; \
  for (ifa = (in_dev)->ifa_list; ifa; ifa = ifa->ifa_next)

// 这个宏用于结束 for_primary_ifa 和 for_ifa 宏创建的循环。
#define endfor_ifa(in_dev) }

/* 内联函数：用于通过RCU机制安全获取与给定网络设备关联的in_device结构 */
/*
 *  这个函数用于获取与网络设备 dev 关联的 in_device 结构。首先从 dev->ip_ptr 获取 in_device 指针，
 * 如果非空则通过 rcu_dereference 进行 RCU 安全的引用。这确保在读取侧使用RCU锁的情况下，能够安全地访问这个指针。
 */
static inline struct in_device *__in_dev_get_rcu(const struct net_device *dev)
{
	struct in_device *in_dev = dev->ip_ptr;
	if (in_dev)
		in_dev = rcu_dereference(in_dev);	/* RCU解引用，确保在RCU读侧临界区内安全使用 */

	return in_dev;
}

/* 获取指定网络设备的IPv4设备结构体指针，并递增其引用计数 */
static __inline__ struct in_device *
in_dev_get(const struct net_device *dev)
{
	struct in_device *in_dev;

	rcu_read_lock();  // 开启RCU读锁，确保之后的读操作是安全的
	in_dev = __in_dev_get_rcu(dev);  // 安全地获取与网络设备相关联的in_device结构体
	if (in_dev)
		atomic_inc(&in_dev->refcnt);  // 如果in_device存在，递增其引用计数
	rcu_read_unlock();  // 释放RCU读锁
	return in_dev;  // 返回获取到的in_device结构体指针
}

/* 获取网络设备的IPv4设备结构体指针，不考虑同步机制（通常在保护区域内调用） */
static __inline__ struct in_device *
__in_dev_get_rtnl(const struct net_device *dev)
{
	return (struct in_device*)dev->ip_ptr;  // 直接返回dev结构中的ip_ptr指针
}

/* 外部声明的函数，用于完成in_device结构的最终销毁 */
extern void in_dev_finish_destroy(struct in_device *idev);

/* 释放一个in_device实例的引用，如果引用计数为0，则销毁它 */
static inline void in_dev_put(struct in_device *idev)
{
	if (atomic_dec_and_test(&idev->refcnt))  // 如果递减后引用计数为0
		in_dev_finish_destroy(idev);  // 调用销毁函数
}

/* 递减指定in_device的引用计数 */
#define __in_dev_put(idev)  atomic_dec(&(idev)->refcnt)
/* 递增指定in_device的引用计数 */
#define in_dev_hold(idev)   atomic_inc(&(idev)->refcnt)

#endif /* __KERNEL__ */

/* 根据给定的掩码长度logmask创建一个网络字节序的IPv4地址掩码 */
static __inline__ __be32 inet_make_mask(int logmask)
{
	if (logmask)  // 如果掩码长度非零
		return htonl(~((1<<(32-logmask))-1));  // 计算掩码值并转换为网络字节序
	return 0;  // 如果掩码长度为0，返回0
}

/* 根据给定的网络字节序掩码计算掩码长度 */
static __inline__ int inet_mask_len(__be32 mask)
{
	__u32 hmask = ntohl(mask);  // 将掩码转换为主机字节序
	if (!hmask)  // 如果掩码为0
		return 0;  // 返回0表示没有有效位
	return 32 - ffz(~hmask);  // 计算掩码中连续1的个数，即掩码长度
}

#endif /* _LINUX_INETDEVICE_H */
