/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET  is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the IP router.
 *
 * Version:	@(#)route.h	1.0.4	05/27/93
 *
 * Authors:	Ross Biro
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 * Fixes:
 *		Alan Cox	:	Reformatted. Added ip_rt_local()
 *		Alan Cox	:	Support for TCP parameters.
 *		Alexey Kuznetsov:	Major changes for new routing code.
 *		Mike McLagan    :	Routing by source
 *		Robert Olsson   :	Added rt_cache statistics
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _ROUTE_H
#define _ROUTE_H

#include <net/dst.h>
#include <net/inetpeer.h>
#include <net/flow.h>
#include <net/inet_sock.h>
#include <linux/in_route.h>
#include <linux/rtnetlink.h>
#include <linux/route.h>
#include <linux/ip.h>
#include <linux/cache.h>
#include <linux/security.h>

#ifndef __KERNEL__
#warning This file is not supposed to be used outside of kernel.
#endif

#define RTO_ONLINK	0x01	// 定义 RTO_ONLINK 为 0x01，通常用于指定路由选项，表明目标网络直接可达。

#define RTO_CONN	0
/* RTO_CONN is not used (being alias for 0), but preserved not to break
 * some modules referring to it. */
/* RTO_CONN 不再使用（是0的别名），但保留是为了不破坏引用它的一些模块。*/

// 定义 RT_CONN_FLAGS 宏，用于从 socket 结构中提取传输服务类型 (TOS) 和检查是否设置了本地路由标志。
#define RT_CONN_FLAGS(sk)   (RT_TOS(inet_sk(sk)->tos) | sock_flag(sk, SOCK_LOCALROUTE))

struct fib_nh;
struct inet_peer;
/*
 * u.dst：存储目标条目信息，用于路由决策。
 * fl：存储流信息，如源端口、目标端口、协议类型等，用于查找匹配的路由项。
 * idev：关联的网络设备，用于确定路由的物理出口。
 * rt_genid、rt_flags、rt_type：路由生成ID、路由标志和路由类型，用于路由管理和处理。
 * rt_dst、rt_src、rt_iif：路由的目的地、源地址和输入接口索引。
 * rt_gateway：指定路由的下一跳网关。
 * rt_spec_dst、peer：为一些特殊目的地和存储长期存在的对等信息。
 */
struct rtable {
	/* 用于存储目标条目的联合体 */
	union {
		struct dst_entry	dst;
	} u;

	/* 存储流信息 */
	/* Cache lookup keys */
	struct flowi		fl;

	/* 指向网络设备的指针 */
	struct in_device	*idev;
	
	/* 路由生成 ID */
	int			rt_genid;
	/* 路由标志 */
	unsigned		rt_flags;
	/* 路由类型 */
	__u16			rt_type;

	/* 路径目的地 */
	__be32			rt_dst;	/* Path destination	*/
	/* 路径源 */
	__be32			rt_src;	/* Path source		*/
	/* 输入接口索引 */
	int			rt_iif;

	/* Info on neighbour */
	/* 下一跳网关地址 */
	__be32			rt_gateway;

	/* Miscellaneous cached information */
	/* RFC1122 特定目的地 */
	__be32			rt_spec_dst; /* RFC1122 specific destination */
	/* 长期存在的对等信息 */
	struct inet_peer	*peer; /* long-living peer info */
};

// 定义了一个名为 ip_rt_acct 的结构体，用于存储与 IP 路由相关的计数和统计信息。结构体中包括了出站和入站的字节和包计数。
struct ip_rt_acct {
	__u32 	o_bytes;    /* 出站字节数：统计通过该路由传输的所有出站数据的总字节量。 */
	__u32 	o_packets;  /* 出站数据包数：统计通过该路由发送的出站数据包的总数量。 */
	__u32 	i_bytes;    /* 入站字节数：统计通过该路由接收的所有入站数据的总字节量。 */
	__u32 	i_packets;  /* 入站数据包数：统计通过该路由接收的入站数据包的总数量。 */
};

/**
 * in_hit：表示入站数据包在路由缓存中找到有效路由的次数。
 * in_slow_tot：表示入站数据包通过缓存的慢速路径（通常涉及更复杂的处理或缺少直接缓存命中）处理的总次数。
 * in_slow_mc：表示针对多播地址的入站数据包通过慢速路径处理的次数。
 * in_no_route：表示没有找到有效路由的入站数据包数量。
 * in_brd：表示作为广播处理的入站数据包数量。
 * in_martian_dst：表示入站数据包中具有异常（不符合规范）目的地地址的数量。
 * in_martian_src：表示入站数据包中具有异常源地址的数量。
 * out_hit：表示出站数据包在路由缓存中找到有效路由的次数。
 * out_slow_tot：表示出站数据包通过缓存的慢速路径处理的总次数。
 * out_slow_mc：表示针对多播地址的出站数据包通过慢速路径处理的次数。
 * gc_total：表示进行路由缓存垃圾收集的总尝试次数。
 * gc_ignored：表示垃圾收集过程中忽略的缓存条目数量。
 * gc_goal_miss：表示垃圾收集没有达到预期清理目标的次数。
 * gc_dst_overflow：表示由于目的地溢出而不能存储更多路由的次数。
 * in_hlist_search、out_hlist_search：表示对路由缓存进行散列搜索的入站和出站次数。
 */
// 定义了一个名为 rt_cache_stat 的结构体，用于存储与路由缓存相关的统计信息。这些统计信息帮助监控和分析路由缓存的效率和性能问题。
struct rt_cache_stat {
        unsigned int in_hit;          // 路由缓存命中，入站
        unsigned int in_slow_tot;     // 入站路由缓存的慢速路径总次数
        unsigned int in_slow_mc;      // 入站路由缓存的慢速多播路径次数
        unsigned int in_no_route;     // 入站无路由次数
        unsigned int in_brd;          // 入站广播次数
        unsigned int in_martian_dst;  // 入站目的地地址异常（火星地址）次数
        unsigned int in_martian_src;  // 入站源地址异常（火星地址）次数
        unsigned int out_hit;         // 路由缓存命中，出站
        unsigned int out_slow_tot;    // 出站路由缓存的慢速路径总次数
        unsigned int out_slow_mc;     // 出站路由缓存的慢速多播路径次数
        unsigned int gc_total;        // 路由缓存垃圾收集尝试总次数
        unsigned int gc_ignored;      // 路由缓存垃圾收集忽略的条目数
        unsigned int gc_goal_miss;    // 路由缓存垃圾收集未达到目标次数
        unsigned int gc_dst_overflow; // 路由缓存目的地溢出次数
        unsigned int in_hlist_search; // 入站散列表搜索次数
        unsigned int out_hlist_search;// 出站散列表搜索次数
};

/* 外部声明，指向每个 CPU 的 IP 路由计数器的指针。用于统计每个处理器上 IP 路由的流量。 */
extern struct ip_rt_acct __percpu *ip_rt_acct;

/* 前向声明一个结构体，通常用于存储与网络设备关联的 IP 特定信息。 */
struct in_device;
/* 初始化 IP 路由子系统。 */
extern int		ip_rt_init(void);
/* 实现 IP 重定向功能。用于在路由表中更新旧的网关地址为新的网关地址。 */
extern void		ip_rt_redirect(__be32 old_gw, __be32 dst, __be32 new_gw,
				       __be32 src, struct net_device *dev);
/* 刷新路由缓存。'how' 参数通常指定刷新的方式。 */
extern void		rt_cache_flush(struct net *net, int how);
/* 批量刷新路由缓存，用于优化刷新操作，减少性能开销。 */
extern void		rt_cache_flush_batch(void);
/* 低级路由查找函数，基于给定的 flowi 结构（包含流信息如目标 IP，源 IP 等）查找路由。 */
extern int		__ip_route_output_key(struct net *, struct rtable **, const struct flowi *flp);
/* 从给定的流信息中查找并输出路由表项。 */
extern int		ip_route_output_key(struct net *, struct rtable **, struct flowi *flp);
/* 根据提供的流信息，套接字和标志，查找并输出路由表项。 */
extern int		ip_route_output_flow(struct net *, struct rtable **rp, struct flowi *flp, struct sock *sk, int flags);
/* 处理入站数据包的路由选择。 */
extern int		ip_route_input(struct sk_buff*, __be32 dst, __be32 src, u8 tos, struct net_device *devin);
/* 生成 ICMP 消息，通知发送方 IP 分片需要的 MTU 值。 */
extern unsigned short	ip_rt_frag_needed(struct net *net, struct iphdr *iph, unsigned short new_mtu, struct net_device *dev);
/* 生成并发送 ICMP 重定向消息。 */
extern void		ip_rt_send_redirect(struct sk_buff *skb);

/* 检查 IP 地址的类型（如 UNICAST、BROADCAST 等）。 */
extern unsigned		inet_addr_type(struct net *net, __be32 addr);
/* 检查指定设备上 IP 地址的类型。 */
extern unsigned		inet_dev_addr_type(struct net *net, const struct net_device *dev, __be32 addr);
/* 多播事件的路由处理，通常在多播地址变化时调用。 */
extern void		ip_rt_multicast_event(struct in_device *);
/* 处理与 IP 路由相关的 IOCTL 调用。 */
extern int		ip_rt_ioctl(struct net *, unsigned int cmd, void __user *arg);
/* 从路由表项中获取源 IP 地址。 */
extern void		ip_rt_get_source(u8 *src, struct rtable *rt);
/* 用于 NETLINK 接口，输出路由表信息。 */
extern int		ip_rt_dump(struct sk_buff *skb,  struct netlink_callback *cb);

/* 前向声明，通常用于存储与网络接口关联的 IPv4 地址信息。 */
struct in_ifaddr;
/* 添加一个网络接口地址到 FIB (Forwarding Information Base)。 */
extern void fib_add_ifaddr(struct in_ifaddr *);

static inline void ip_rt_put(struct rtable * rt)
{
	/* 
	 * 释放路由表项。如果传入的 rt（路由表指针）非空，则调用 dst_release 函数来释放与其关联的目的地结构。
	 * 这通常涉及减少引用计数，并可能涉及清理资源。
	 */
	if (rt)
		dst_release(&rt->u.dst);
}

/* 
 * 定义一个宏 IPTOS_RT_MASK，用于处理 IP 头中的服务类型（TOS）字段。
 * 它通过与 ~3 (二进制表示为 ...1100) 的按位与操作，去除 TOS 值的最低两位。这通常用于路由决策中忽略这两位。
 */
#define IPTOS_RT_MASK	(IPTOS_TOS_MASK & ~3)

/*
 * 外部数组声明，用于将IP头部的服务类型（TOS）值映射为对应的优先级。
 * 这个映射允许系统根据TOS值快速查找到相应的优先级设置。
 */
extern const __u8 ip_tos2prio[16];

static inline char rt_tos2priority(u8 tos)
{
	/*
	 * 将 IP 头部的服务类型（TOS）字段转换为内部使用的优先级。
	 * 这里首先将 TOS 值右移一位，这是因为 TOS 通常是8位宽，而 ip_tos2prio 数组大小只有16。
	 * 这个转换允许通过忽略 TOS 值的最低位来使用这个数组。
	 */
	return ip_tos2prio[IPTOS_TOS(tos)>>1];
}

// 基于指定的参数寻找或创建一个路由表项。
static inline int ip_route_connect(struct rtable **rp, __be32 dst,
				   __be32 src, u32 tos, int oif, u8 protocol,
				   __be16 sport, __be16 dport, struct sock *sk,
				   int flags)
{
	 /* 初始化 flowi 结构体，用于描述特定的流，包含了出入接口，协议类型，源目的地址等信息 */
	struct flowi fl = { .oif = oif,
			    .mark = sk->sk_mark,
			    .nl_u = { .ip4_u = { .daddr = dst,
						 .saddr = src,
						 .tos   = tos } },
			    .proto = protocol,
			    .uli_u = { .ports =
				       { .sport = sport,
					 .dport = dport } } };

	int err;
	struct net *net = sock_net(sk);	/* 从套接字获取关联的网络命名空间 */

	if (inet_sk(sk)->transparent)
		fl.flags |= FLOWI_FLAG_ANYSRC;	/* 如果套接字设置为透明模式，则设置流的标志，允许任何源 */

	if (!dst || !src) {
		/* 如果没有指定目的地或源地址，通过流信息尝试获取路由表项 */
		err = __ip_route_output_key(net, rp, &fl);
		if (err)
			return err;	/* 如果获取失败，返回错误码 */
		/* 更新流信息中的源和目的地址为路由表项中的地址 */
		fl.fl4_dst = (*rp)->rt_dst;
		fl.fl4_src = (*rp)->rt_src;
		ip_rt_put(*rp);	/* 释放获取到的路由表项 */
		*rp = NULL;
	}
	security_sk_classify_flow(sk, &fl);	 /* 通过安全模块对流进行分类处理 */
	/* 调用 ip_route_output_flow 函数根据更新后的流信息寻找或创建路由表项 */
	return ip_route_output_flow(net, rp, &fl, sk, flags);
}

// 更新路由表项的端口信息的内联函数 ip_route_newports。此函数用于当套接字的源端口或目的端口变化时，更新路由表项以匹配新的端口配置。
static inline int ip_route_newports(struct rtable **rp, u8 protocol,
                                    __be16 sport, __be16 dport, struct sock *sk)
{
	/* 如果当前路由表项的源端口或目的端口与传入的端口不同，则需要更新路由表项 */
	if (sport != (*rp)->fl.fl_ip_sport ||
			dport != (*rp)->fl.fl_ip_dport) {
		struct flowi fl;

		/* 复制现有路由表项的 flowi 结构到局部变量 fl */
		memcpy(&fl, &(*rp)->fl, sizeof(fl));
		fl.fl_ip_sport = sport;  // 更新源端口
		fl.fl_ip_dport = dport;  // 更新目的端口
		fl.proto = protocol;     // 设置协议类型
		/* 释放当前的路由表项 */
		ip_rt_put(*rp);
		*rp = NULL;  // 清空路由表项指针，准备重新查询
		/* 对套接字关联的流进行安全分类 */
		security_sk_classify_flow(sk, &fl);
		/* 重新输出路由查询 */
		return ip_route_output_flow(sock_net(sk), rp, &fl, sk, 0);
	}
	/* 如果端口未改变，直接返回0，表示无需更新 */
	return 0;
}

/* 外部函数声明，用于将一个 inet_peer 绑定到指定的路由表项 rt。如果 create 参数为真，且 rt 尚未绑定 inet_peer，则新建一个。 */
extern void rt_bind_peer(struct rtable *rt, int create);

static inline struct inet_peer *rt_get_peer(struct rtable *rt)
{
	/* 尝试从给定的路由表项 rt 中获取与之关联的 inet_peer 结构。 */
	if (rt->peer)
			return rt->peer;
	/* 如果路由表项 rt 中没有 inet_peer，调用 rt_bind_peer 尝试绑定一个 inet_peer。 */
	rt_bind_peer(rt, 0);
	/* 返回绑定（或尝试绑定）后的 inet_peer 结构。 */
	return rt->peer;
}

static inline int inet_iif(const struct sk_buff *skb)
{
	/* 从 skb 关联的路由表项中获取输入接口的索引。 */
	return skb_rtable(skb)->rt_iif;
}

#endif	/* _ROUTE_H */
