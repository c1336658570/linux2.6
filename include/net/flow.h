/*
 *
 *	Generic internet FLOW.
 *
 */

#ifndef _NET_FLOW_H
#define _NET_FLOW_H

#include <linux/in6.h>
#include <asm/atomic.h>

// 该结构体用于存储网络流信息。它在路由、过滤和转发决策中非常关键。
struct flowi {
	int	oif;                  // 输出接口索引，用于标识目标数据应发送的网络接口
	int	iif;                  // 输入接口索引，用于标识接收数据的网络接口
	__u32	mark;               // 流标记，可用于在处理数据包时标识或分类数据包

	// 下面定义了一个联合体，包括多种协议的特定字段

	union {
		struct {
			__be32			daddr;        // 目的IPv4地址
			__be32			saddr;        // 源IPv4地址
			__u8			tos;          // 服务类型(Type of Service)，用于服务质量(QoS)控制
			__u8			scope;        // 地址作用范围，控制地址的可见性和使用范围
		} ip4_u;
		
		struct {
			struct in6_addr		daddr;        // 目的IPv6地址
			struct in6_addr		saddr;        // 源IPv6地址
			__be32			flowlabel;    // IPv6流标签，用于标识属于同一通信流的包
		} ip6_u;

		struct {
			__le16			daddr;        // DECnet目的地址
			__le16			saddr;        // DECnet源地址
			__u8			scope;        // 地址范围限制
		} dn_u;
	} nl_u;

	// 宏定义，便于访问联合中的字段
#define fld_dst		nl_u.dn_u.daddr    // 定义宏 fld_dst 用于快速访问 DECnet 目的地址
#define fld_src		nl_u.dn_u.saddr    // 定义宏 fld_src 用于快速访问 DECnet 源地址
#define fld_scope	nl_u.dn_u.scope    // 定义宏 fld_scope 用于快速访问 DECnet 地址的作用范围
#define fl6_dst		nl_u.ip6_u.daddr   // 定义宏 fl6_dst 用于快速访问 IPv6 目的地址
#define fl6_src		nl_u.ip6_u.saddr   // 定义宏 fl6_src 用于快速访问 IPv6 源地址
#define fl6_flowlabel	nl_u.ip6_u.flowlabel // 定义宏 fl6_flowlabel 用于快速访问 IPv6 的流标签
#define fl4_dst		nl_u.ip4_u.daddr   // 定义宏 fl4_dst 用于快速访问 IPv4 目的地址
#define fl4_src		nl_u.ip4_u.saddr   // 定义宏 fl4_src 用于快速访问 IPv4 源地址
#define fl4_tos		nl_u.ip4_u.tos     // 定义宏 fl4_tos 用于快速访问 IPv4 的服务类型（Type of Service）
#define fl4_scope	nl_u.ip4_u.scope   // 定义宏 fl4_scope 用于快速访问 IPv4 地址的作用范围

	__u8	proto;             // 传输层协议类型，通常用于指定TCP、UDP、ICMP等协议
	__u8	flags;             // 标记，用于控制特定的行为或状态，例如 FLOWI_FLAG_ANYSRC 表示接受任何源地址
#define FLOWI_FLAG_ANYSRC 0x01

	union {
		struct {
			__be16	sport;       // 源端口号，适用于TCP/UDP等协议
			__be16	dport;       // 目的端口号，适用于TCP/UDP等协议
		} ports;

		struct {
			__u8	type;        // ICMP消息类型
			__u8	code;        // ICMP消息代码
		} icmpt;

		struct {
			__le16	sport;       // DECnet源端口
			__le16	dport;       // DECnet目的端口
		} dnports;

		__be32		spi;          // 安全参数索引，通常用于IPsec

		struct {
			// 移动主机协议（Mobile Host Protocol）类型
			__u8	type;        // 移动头部类型（用于移动IP）
		} mht;
	} uli_u;	// 联合体，根据不同的协议类型存储不同的协议特定信息
#define fl_ip_sport	uli_u.ports.sport  // 定义宏以方便访问TCP/UDP的源端口
#define fl_ip_dport	uli_u.ports.dport  // 定义宏以方便访问TCP/UDP的目的端口
#define fl_icmp_type	uli_u.icmpt.type   // 定义宏以方便访问ICMP的类型
#define fl_icmp_code	uli_u.icmpt.code   // 定义宏以方便访问ICMP的代码
#define fl_ipsec_spi	uli_u.spi          // 定义宏以方便访问IPsec的安全参数索引（SPI）
#define fl_mh_type	uli_u.mht.type     // 定义宏以方便访问移动主机协议的类型
	// 安全ID，用于XFRM（IPSec）
	/* secid 字段用于存储与 xfrm (IPsec的变种之一) 相关的安全上下文标识符 */
	__u32           secid;	/* used by xfrm; see secid.txt */
} __attribute__((__aligned__(BITS_PER_LONG/8)));  // 确保结构体按长字对齐

#define FLOW_DIR_IN	0    // 定义流量方向为“输入”
#define FLOW_DIR_OUT	1   // 定义流量方向为“输出”
#define FLOW_DIR_FWD	2   // 定义流量方向为“转发”

struct net;              // 前向声明网络命名空间结构体
struct sock;             // 前向声明套接字结构体
// 定义一个函数指针类型，用于解析流信息，返回int类型结果
typedef int (*flow_resolve_t)(struct net *net, struct flowi *key, u16 family,
			      u8 dir, void **objp, atomic_t **obj_refp);

/* 声明一个外部函数，用于查找流缓存，此函数需要网络命名空间、流密钥、家族ID、
   方向和一个解析器函数，返回查找到的对象指针 */
extern void *flow_cache_lookup(struct net *net, struct flowi *key, u16 family,
			       u8 dir, flow_resolve_t resolver);

/* 声明一个外部函数，用于清空流缓存 */
extern void flow_cache_flush(void);

/* 声明一个外部原子变量，用于表示流缓存的当前版本号 */
extern atomic_t flow_cache_genid;

/* 定义一个内联函数，用于比较两个流是否匹配，通过比较它们的协议和uli_u部分来判断。
   返回1表示匹配，0表示不匹配。 */
static inline int flow_cache_uli_match(struct flowi *fl1, struct flowi *fl2)
{
	return (fl1->proto == fl2->proto &&
		!memcmp(&fl1->uli_u, &fl2->uli_u, sizeof(fl1->uli_u)));
}

#endif
