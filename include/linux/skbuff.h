/*
 *	Definitions for the 'struct sk_buff' memory handlers.
 *
 *	Authors:
 *		Alan Cox, <gw4pts@gw4pts.ampr.org>
 *		Florian La Roche, <rzsfl@rz.uni-sb.de>
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#ifndef _LINUX_SKBUFF_H
#define _LINUX_SKBUFF_H

#include <linux/kernel.h>
#include <linux/kmemcheck.h>
#include <linux/compiler.h>
#include <linux/time.h>
#include <linux/cache.h>

#include <asm/atomic.h>
#include <asm/types.h>
#include <linux/spinlock.h>
#include <linux/net.h>
#include <linux/textsearch.h>
#include <net/checksum.h>
#include <linux/rcupdate.h>
#include <linux/dmaengine.h>
#include <linux/hrtimer.h>

/* Don't change this without changing skb_csum_unnecessary! */
/* 不要在不修改 skb_csum_unnecessary 的情况下更改这些定义 */
#define CHECKSUM_NONE 0         /* 指示数据包没有校验和 */
#define CHECKSUM_UNNECESSARY 1  /* 指示数据包不需要校验和 */
#define CHECKSUM_COMPLETE 2     /* 指示数据包的校验和已完全计算 */
#define CHECKSUM_PARTIAL 3      /* 指示数据包的校验和部分计算，需进一步处理 */

/* 对 X 进行对齐，以符合 SMP 缓存行的大小 */
#define SKB_DATA_ALIGN(X)	(((X) + (SMP_CACHE_BYTES - 1)) & \
				 ~(SMP_CACHE_BYTES - 1))
/* 计算实际可用于存储数据的空间，减去存储 skb_shared_info 结构所需的空间 */
#define SKB_WITH_OVERHEAD(X)	\
	((X) - SKB_DATA_ALIGN(sizeof(struct skb_shared_info)))
/* 计算在给定 ORDER 页内存块的情况下，最大可分配的 skb 数据区大小 */
#define SKB_MAX_ORDER(X, ORDER) \
	SKB_WITH_OVERHEAD((PAGE_SIZE << (ORDER)) - (X))
/* 计算在单个页面内可用的最大 skb 头部空间 */
#define SKB_MAX_HEAD(X)		(SKB_MAX_ORDER((X), 0))
/* 计算可以分配的最大 skb 的总大小，此处以两个页面大小为上限 */
#define SKB_MAX_ALLOC		(SKB_MAX_ORDER(0, 2))

/* A. Checksumming of received packets by device.
 *
 *	NONE: device failed to checksum this packet.
 *		skb->csum is undefined.
 *
 *	UNNECESSARY: device parsed packet and wouldbe verified checksum.
 *		skb->csum is undefined.
 *	      It is bad option, but, unfortunately, many of vendors do this.
 *	      Apparently with secret goal to sell you new device, when you
 *	      will add new protocol to your host. F.e. IPv6. 8)
 *
 *	COMPLETE: the most generic way. Device supplied checksum of _all_
 *	    the packet as seen by netif_rx in skb->csum.
 *	    NOTE: Even if device supports only some protocols, but
 *	    is able to produce some skb->csum, it MUST use COMPLETE,
 *	    not UNNECESSARY.
 *
 *	PARTIAL: identical to the case for output below.  This may occur
 *	    on a packet received directly from another Linux OS, e.g.,
 *	    a virtualised Linux kernel on the same host.  The packet can
 *	    be treated in the same way as UNNECESSARY except that on
 *	    output (i.e., forwarding) the checksum must be filled in
 *	    by the OS or the hardware.
 *
 * B. Checksumming on output.
 *
 *	NONE: skb is checksummed by protocol or csum is not required.
 *
 *	PARTIAL: device is required to csum packet as seen by hard_start_xmit
 *	from skb->csum_start to the end and to record the checksum
 *	at skb->csum_start + skb->csum_offset.
 *
 *	Device must show its capabilities in dev->features, set
 *	at device setup time.
 *	NETIF_F_HW_CSUM	- it is clever device, it is able to checksum
 *			  everything.
 *	NETIF_F_NO_CSUM - loopback or reliable single hop media.
 *	NETIF_F_IP_CSUM - device is dumb. It is able to csum only
 *			  TCP/UDP over IPv4. Sigh. Vendors like this
 *			  way by an unknown reason. Though, see comment above
 *			  about CHECKSUM_UNNECESSARY. 8)
 *	NETIF_F_IPV6_CSUM about as dumb as the last one but does IPv6 instead.
 *
 *	Any questions? No questions, good. 		--ANK
 */
/* A. 接收设备的数据包校验
 *
 *	NONE: 设备未能校验此数据包。
 *		skb->csum 是未定义的。
 *
 *	UNNECESSARY: 设备解析了数据包并且确认了校验和。
 *		skb->csum 是未定义的。
 *	      这是一个糟糕的选择，但不幸的是，许多供应商这样做。
 *	      显然是为了在你的主机添加新协议时（例如IPv6）促使你购买新设备。
 *
 *	COMPLETE: 最通用的方式。设备提供了 skb->csum 中整个数据包的校验和，
 *	    正如 netif_rx 所见。
 *	    注意：即使设备只支持某些协议，但能产生一些 skb->csum，它必须使用 COMPLETE，
 *	    而不是 UNNECESSARY。
 *
 *	PARTIAL: 与下面输出的情况相同。这可能发生在从另一个 Linux OS 直接接收的数据包上，
 *	    例如，在同一主机上的虚拟化 Linux 内核。这种数据包可以像处理 UNNECESSARY 一样处理，
 *	    除了在输出（即转发）时必须由 OS 或硬件填充校验和。
 *
 * B. 输出时的校验和
 *
 *	NONE: 由协议校验 skb 或不需要校验和。
 *
 *	PARTIAL: 设备必须从 skb->csum_start 到结尾对数据包进行校验和计算，并在
 *	skb->csum_start + skb->csum_offset 处记录校验和。
 *
 *	设备必须在 dev->features 中显示其能力，这在设备设置时确定。
 *	NETIF_F_HW_CSUM - 它是一个智能设备，能够校验所有内容。
 *	NETIF_F_NO_CSUM - 环回或可靠的单跳媒体。
 *	NETIF_F_IP_CSUM - 设备是笨的。它只能对 IPv4 上的 TCP/UDP 进行校验。唉。
 *			  供应商出于未知原因喜欢这种方式。然而，请参见上面关于
 *			  CHECKSUM_UNNECESSARY 的评论。
 *	NETIF_F_IPV6_CSUM - 与上一个一样笨，但处理的是 IPv6。
 *
 *	有问题吗？没问题，很好。		--ANK
 */

struct net_device;  // 定义网络设备结构体
struct scatterlist; // 定义散列表结构体，用于管理内存散列
struct pipe_inode_info; // 定义管道 inode 信息结构体

/* 如果定义了 CONFIG_NF_CONNTRACK（网络连接跟踪支持）或 CONFIG_NF_CONNTRACK_MODULE（模块支持），则包含以下代码 */
#if defined(CONFIG_NF_CONNTRACK) || defined(CONFIG_NF_CONNTRACK_MODULE)
struct nf_conntrack {
	atomic_t use; // 使用原子操作的计数器，用于跟踪该结构体的引用次数
};
#endif

/* 如果定义了 CONFIG_BRIDGE_NETFILTER（桥接网络过滤支持），则包含以下代码 */
#ifdef CONFIG_BRIDGE_NETFILTER
struct nf_bridge_info {
	atomic_t use;                  // 使用原子操作的计数器，同样用于跟踪引用次数
	struct net_device *physindev;  // 指向物理入口设备的指针
	struct net_device *physoutdev; // 指向物理出口设备的指针
	unsigned int mask;             // 用于掩码操作，可能涉及特定过滤或处理标志
	unsigned long data[32 / sizeof(unsigned long)]; // 用于存储额外的数据或状态信息
};
#endif

struct sk_buff_head {
	/* These two members must be first. */
	/* 这两个成员必须是首先定义的。 */
	struct sk_buff *next;    // 指向链表中下一个 sk_buff 的指针
	struct sk_buff *prev;    // 指向链表中上一个 sk_buff 的指针

	__u32 qlen;              // 队列中的 skb 数量
	spinlock_t lock;         // 自旋锁，用于保护此结构（防止并发访问问题）
};

struct sk_buff; // 声明 sk_buff 结构体，它是网络数据包的主要容器

/* To allow 64K frame to be packed as single skb without frag_list */
/* 为了允许将 64K 的帧作为单个 skb 打包而不使用 frag_list */
#define MAX_SKB_FRAGS (65536/PAGE_SIZE + 2) // 计算最大 skb 片段数，用于大型数据包的处理

typedef struct skb_frag_struct skb_frag_t; // 定义 skb_frag_t 类型，指向 skb_frag_struct 结构

struct skb_frag_struct {
	struct page *page;         // 指向包含数据的页结构的指针
	__u32 page_offset;         // 数据在页中的偏移量
	__u32 size;                // 片段大小
};

#define HAVE_HW_TIME_STAMP // 定义一个宏，可能用于标识硬件时间戳的支持

/**
 * struct skb_shared_hwtstamps - hardware time stamps
 * @hwtstamp:	hardware time stamp transformed into duration
 *		since arbitrary point in time
 * @syststamp:	hwtstamp transformed to system time base
 *
 * Software time stamps generated by ktime_get_real() are stored in
 * skb->tstamp. The relation between the different kinds of time
 * stamps is as follows:
 *
 * syststamp and tstamp can be compared against each other in
 * arbitrary combinations.  The accuracy of a
 * syststamp/tstamp/"syststamp from other device" comparison is
 * limited by the accuracy of the transformation into system time
 * base. This depends on the device driver and its underlying
 * hardware.
 *
 * hwtstamps can only be compared against other hwtstamps from
 * the same device.
 *
 * This structure is attached to packets as part of the
 * &skb_shared_info. Use skb_hwtstamps() to get a pointer.
 */
/**
 * struct skb_shared_hwtstamps - 硬件时间戳
 * @hwtstamp:	转换成从某个任意时间点起的持续时间的硬件时间戳
 * @syststamp:	hwtstamp 转换为系统时间基准
 *
 * 由 ktime_get_real() 生成的软件时间戳存储在 skb->tstamp 中。不同类型的时间戳之间的关系如下：
 *
 * syststamp 和 tstamp 可以在任意组合中相互比较。syststamp/tstamp/“来自其他设备的 syststamp”比较的
 * 精确性受到转换到系统时间基准的精确性的限制。这依赖于设备驱动程序及其底层硬件。
 *
 * hwtstamps 只能与来自同一设备的其他 hwtstamps 比较。
 *
 * 此结构附加到数据包作为 &skb_shared_info 的一部分。使用 skb_hwtstamps() 获取指针。
 */
struct skb_shared_hwtstamps {
	ktime_t	hwtstamp;   // 硬件时间戳，表示数据包在硬件层面被处理的精确时间
	ktime_t	syststamp;  // 系统时间戳，将硬件时间戳转换为对应的系统时间
};

/**
 * struct skb_shared_tx - instructions for time stamping of outgoing packets
 * @hardware:		generate hardware time stamp
 * @software:		generate software time stamp
 * @in_progress:	device driver is going to provide
 *			hardware time stamp
 * @flags:		all shared_tx flags
 *
 * These flags are attached to packets as part of the
 * &skb_shared_info. Use skb_tx() to get a pointer.
 */
/**
 * struct skb_shared_tx - 发出包的时间戳指令
 * @hardware:		生成硬件时间戳
 * @software:		生成软件时间戳
 * @in_progress:	设备驱动程序将要提供硬件时间戳
 * @flags:		所有 shared_tx 标志
 *
 * 这些标志作为 &skb_shared_info 的一部分附加到数据包上。使用 skb_tx() 获取指针。
 */
union skb_shared_tx {
	struct {
		__u8	hardware:1,       // 1位，标志是否生成硬件时间戳
			software:1,       // 1位，标志是否生成软件时间戳
			in_progress:1;    // 1位，标志硬件时间戳的生成是否正在进行
	};
	__u8 flags;                 // 同样的标志，作为一个单独的字节，便于操作和访问
};

/* This data is invariant across clones and lives at
 * the end of the header data, ie. at skb->end.
 */
/* 这些数据在克隆中是不变的，并存放在头数据的末端，即 skb->end 处。 */
struct skb_shared_info {
	atomic_t	dataref;              // 原子引用计数，用于跟踪对此 skb 的共享信息的引用数量
	unsigned short	nr_frags;            // 分片数，指 skb 包含的分片数量
	unsigned short	gso_size;            // 分段发送的大小，用于大型数据包的分段处理
	/* Warning: this field is not always filled in (UFO)! */
	/* 警告：此字段并非总是被填充（UFO）！ */
	unsigned short	gso_segs;            // 分段发送的段数，表示通过 GSO 分割的数据包数量
	unsigned short  gso_type;            // 分段发送的类型，定义了 GSO 操作的具体类型
	__be32          ip6_frag_id;         // IPv6 分片的标识符，用于标识 IPv6 分片
	union skb_shared_tx tx_flags;        // 发送标志，控制数据包发送时的时间戳生成
	/*
	 * 如果一个报文特别大的话，线性存储区放不下就需要多个skb来存储，这就是下面frag_list的作用，
	 * 保存连续的skb，但是如果内核支持分散聚集技术的话，并且报文长度刚好又不大于mtu,
	 * 就不必重新分配一个skb来存储，可以使用一些内存碎片来存储，就是下面的frags数组表示的内存页面片段。
	 */
	struct sk_buff	*frag_list;          // 指向更多分片的指针，用于组织多个分片或分段的数据包
	struct skb_shared_hwtstamps hwtstamps; // 硬件时间戳信息，用于记录数据包的发送和接收时间
	skb_frag_t	frags[MAX_SKB_FRAGS];  // 分片数组，存储指向数据包分片的指针和相关信息
	/* Intermediate layers must ensure that destructor_arg
	 * remains valid until skb destructor */
	/* 中间层必须确保 destructor_arg 在 skb 析构函数执行前保持有效 */
	void *		destructor_arg;       // 析构函数参数，用于在 skb 被销毁时传递额外的上下文信息
};

/* We divide dataref into two halves.  The higher 16 bits hold references
 * to the payload part of skb->data.  The lower 16 bits hold references to
 * the entire skb->data.  A clone of a headerless skb holds the length of
 * the header in skb->hdr_len.
 *
 * All users must obey the rule that the skb->data reference count must be
 * greater than or equal to the payload reference count.
 *
 * Holding a reference to the payload part means that the user does not
 * care about modifications to the header part of skb->data.
 */
/* 我们将 dataref 分为两部分。高 16 位保存对 skb->data 负载部分的引用。
 * 低 16 位保存对整个 skb->data 的引用。
 * 一个没有头部的 skb 克隆将头部的长度保存在 skb->hdr_len 中。
 *
 * 所有用户必须遵守的规则是：skb->data 的引用计数必须大于或等于负载的引用计数。
 *
 * 持有对负载部分的引用意味着用户不关心对 skb->data 的头部部分的修改。
 */
#define SKB_DATAREF_SHIFT 16  // 定义引用计数的位移值，将 dataref 分成高 16 位和低 16 位
#define SKB_DATAREF_MASK ((1 << SKB_DATAREF_SHIFT) - 1) // 用于提取低 16 位的掩码

enum {
	SKB_FCLONE_UNAVAILABLE, // 表示没有可用的克隆
	SKB_FCLONE_ORIG,        // 表示原始的 skb
	SKB_FCLONE_CLONE,       // 表示克隆的 skb
};

enum {
	SKB_GSO_TCPV4 = 1 << 0,  // GSO 标志：IPv4 TCP 分段卸载标志
	SKB_GSO_UDP = 1 << 1,    // GSO 标志：UDP 分段卸载标志

	/* This indicates the skb is from an untrusted source. */
	/* 这表示 skb 来自一个不可信的来源。 */
	SKB_GSO_DODGY = 1 << 2,  // GSO 标志：不可信来源的数据包

	/* This indicates the tcp segment has CWR set. */
	/* 这表示 TCP 段已设置 CWR 标志。 */
	SKB_GSO_TCP_ECN = 1 << 3,  // GSO 标志：TCP 分段卸载（ECN 已设置），CWR 已设置

	SKB_GSO_TCPV6 = 1 << 4,  // GSO 标志：IPv6 TCP 分段卸载标志

	SKB_GSO_FCOE = 1 << 5,   // GSO 标志：FCoE（以太网光纤通道）分段卸载标志
};

/* 如果系统位数超过 32 位，则定义 NET_SKBUFF_DATA_USES_OFFSET */
#if BITS_PER_LONG > 32
#define NET_SKBUFF_DATA_USES_OFFSET 1  /* 在 64 位系统中使用偏移量而不是直接指针 */
#endif

/* 如果定义了 NET_SKBUFF_DATA_USES_OFFSET，则 sk_buff_data_t 类型为 unsigned int，否则为 unsigned char* */
#ifdef NET_SKBUFF_DATA_USES_OFFSET
// 64 位系统中使用无符号整数表示偏移
typedef unsigned int sk_buff_data_t;  // 如果使用偏移量，定义 sk_buff_data_t 为无符号整数
#else
// 32 位或更低位数系统中使用指针表示数据位置
typedef unsigned char *sk_buff_data_t;  // 否则，定义 sk_buff_data_t 为无符号字符指针
#endif

/** 
 *	struct sk_buff - socket buffer
 *	@next: Next buffer in list
 *	@prev: Previous buffer in list
 *	@sk: Socket we are owned by
 *	@tstamp: Time we arrived
 *	@dev: Device we arrived on/are leaving by
 *	@transport_header: Transport layer header
 *	@network_header: Network layer header
 *	@mac_header: Link layer header
 *	@_skb_dst: destination entry
 *	@sp: the security path, used for xfrm
 *	@cb: Control buffer. Free for use by every layer. Put private vars here
 *	@len: Length of actual data
 *	@data_len: Data length
 *	@mac_len: Length of link layer header
 *	@hdr_len: writable header length of cloned skb
 *	@csum: Checksum (must include start/offset pair)
 *	@csum_start: Offset from skb->head where checksumming should start
 *	@csum_offset: Offset from csum_start where checksum should be stored
 *	@local_df: allow local fragmentation
 *	@cloned: Head may be cloned (check refcnt to be sure)
 *	@nohdr: Payload reference only, must not modify header
 *	@pkt_type: Packet class
 *	@fclone: skbuff clone status
 *	@ip_summed: Driver fed us an IP checksum
 *	@priority: Packet queueing priority
 *	@users: User count - see {datagram,tcp}.c
 *	@protocol: Packet protocol from driver
 *	@truesize: Buffer size 
 *	@head: Head of buffer
 *	@data: Data head pointer
 *	@tail: Tail pointer
 *	@end: End pointer
 *	@destructor: Destruct function
 *	@mark: Generic packet mark
 *	@nfct: Associated connection, if any
 *	@ipvs_property: skbuff is owned by ipvs
 *	@peeked: this packet has been seen already, so stats have been
 *		done for it, don't do them again
 *	@nf_trace: netfilter packet trace flag
 *	@nfctinfo: Relationship of this skb to the connection
 *	@nfct_reasm: netfilter conntrack re-assembly pointer
 *	@nf_bridge: Saved data about a bridged frame - see br_netfilter.c
 *	@skb_iif: ifindex of device we arrived on
 *	@queue_mapping: Queue mapping for multiqueue devices
 *	@tc_index: Traffic control index
 *	@tc_verd: traffic control verdict
 *	@ndisc_nodetype: router type (from link layer)
 *	@dma_cookie: a cookie to one of several possible DMA operations
 *		done by skb DMA functions
 *	@secmark: security marking
 *	@vlan_tci: vlan tag control information
 */
/**
 *	struct sk_buff - socket buffer
 *	@next: 链表中的下一个缓冲区
 *	@prev: 链表中的前一个缓冲区
 *	@sk: 我们所属的套接字
 *	@tstamp: 我们到达的时间
 *	@dev: 我们到达/离开的设备
 *	@transport_header: 传输层头部
 *	@network_header: 网络层头部
 *	@mac_header: 链路层头部
 *	@_skb_dst: 目的地条目
 *	@sp: 安全路径，用于 xfrm
 *	@cb: 控制缓冲区。每一层都可自由使用。在这里放置私有变量
 *	@len: 实际数据的长度
 *	@data_len: 数据长度
 *	@mac_len: 链路层头部的长度
 *	@hdr_len: 克隆的 skb 可写头部的长度
 *	@csum: 校验和（必须包括起始/偏移对）
 *	@csum_start: 校验和应开始计算的 skb->head 的偏移位置
 *	@csum_offset: 校验和应存储的从 csum_start 的偏移位置
 *	@local_df: 允许本地分片
 *	@cloned: 头部可能被克隆（检查 refcnt 以确保）
 *	@nohdr: 只引用有效载荷，不能修改头部
 *	@pkt_type: 数据包类型
 *	@fclone: skbuff 克隆状态
 *	@ip_summed: 驱动提供的 IP 校验和
 *	@priority: 数据包队列优先级
 *	@users: 用户计数 - 见 {datagram,tcp}.c
 *	@protocol: 来自驱动的数据包协议
 *	@truesize: 缓冲区大小
 *	@head: 缓冲区头部
 *	@data: 数据头指针
 *	@tail: 尾部指针
 *	@end: 结束指针
 *	@destructor: 析构函数
 *	@mark: 通用数据包标记
 *	@nfct: 相关联的连接（如果有）
 *	@ipvs_property: skbuff 由 ipvs 拥有
 *	@peeked: 这个数据包已经被查看过了，统计已经完成，
 *		不要再次进行统计
 *	@nf_trace: netfilter 数据包跟踪标志
 *	@nfctinfo: 这个 skb 与连接的关系
 *	@nfct_reasm: netfilter conntrack 重组指针
 *	@nf_bridge: 保存有关桥接帧的数据 - 见 br_netfilter.c
 *	@skb_iif: 我们到达的设备的接口索引
 *	@queue_mapping: 多队列设备的队列映射
 *	@tc_index: 流量控制索引
 *	@tc_verd: 流量控制裁决
 *	@ndisc_nodetype: 路由器类型（来自链路层）
 *	@dma_cookie: skb DMA 功能执行的多种可能 DMA 操作之一的 cookie
 *	@secmark: 安全标记
 *	@vlan_tci: vlan 标签控制信息
 */
struct sk_buff {
	/* These two members must be first. */
	/* 这两个成员必须是首先定义的。 */
	struct sk_buff		*next;   // 指向链表中的下一个 sk_buff
	struct sk_buff		*prev;   // 指向链表中的前一个 sk_buff

	ktime_t			tstamp;  // 数据包的时间戳，记录数据包到达的时间

	struct sock		*sk;     // 拥有此数据包的套接字
	struct net_device	*dev;    // 数据包到达或离开的设备

	/*
	 * This is the control buffer. It is free to use for every
	 * layer. Please put your private variables there. If you
	 * want to keep them across layers you have to do a skb_clone()
	 * first. This is owned by whoever has the skb queued ATM.
	 */
	/*
	 * 这是控制缓冲区。它对每一层都是自由使用的。
	 * 请在此处放置您的私有变量。如果您想在各层之间保持这些变量，
	 * 您必须首先执行 skb_clone()。当前队列中的 skb 的拥有者拥有此缓冲区。
	 */
	/* 控制缓冲区，每一层都可以自由使用。如果你想在层与层之间保留私有变量，你需要先执行一个 skb_clone()。这由当前排队的人拥有。 */
	char			cb[48] __aligned(8); // 为各层协议使用的控制缓冲区，可以被各层自由使用，48字节，8字节对齐

	// 目标网络路径（用于路由决策）
	unsigned long		_skb_dst;  // 目的地条目
#ifdef CONFIG_XFRM
	// 指向安全路径的指针，用于处理 IPsec 加密和解密
	struct	sec_path	*sp;        // 安全路径，用于 xfrm（IPsec 传输和认证）
#endif
	/*
	 * skb->len 是data长度，包含所带分片长度
	 * skb->data_len 是paged data长度, 即分片数据的长度，也就是skb_shared_info中的长度
	 * skb_headlen skb->len - skb->data_len 是当前片（unpaged data）长度
	 */
	// len-data_len: 当前协议层中的线性区长度
	// 大哥、小弟和兄弟的总和,即data的总长度，线性和非线性的总和。
	unsigned int		len,       // 线性区和分片区域的总长度
	// 小弟和兄弟的总和，即大哥缺少的份额，非线性数据长度。
				data_len;  // 分片区域frag page中的数据长度
	__u16			mac_len,   // MAC（链路层）头部的长度
				hdr_len;   // 克隆的 skb 的可写头部长度
	union {
		__wsum		csum;      // 整个数据包的校验和
		struct {
			// 校验和计算开始的位置（相对于 skb->head）
			__u16	csum_start; // 校验和计算应该开始的地方
			// 校验和存储的位置（相对于 csum_start）
			__u16	csum_offset; // 校验和应该存储的地方
		};
	};
	// 数据包的优先级
	__u32			priority;  // 数据包队列优先级

	// 标志位字段开始
	kmemcheck_bitfield_begin(flags1);	// 开始使用位字段检查
	__u8			local_df:1,   // 允许本地分片
				// 指示该 skb 是否被克隆
				cloned:1,     // 头部可能被克隆（检查引用计数）
				// 指示 IP 校验和的状态：0 未检查，1 校验和正确，2 校验和错误
				ip_summed:2,  // 驱动是否提供了 IP 校验和
				// 指示该 skb 只包含有效载荷，无法修改头部信息
				nohdr:1,      // 只引用负载，不能修改头部
				// 与 netfilter 连接跟踪相关的信息
				nfctinfo:3;   // skb与连接的关系
	__u8			pkt_type:3,	// 包类型，如广播、多播或单播
				fclone:2,     // skb 克隆状态
				ipvs_property:1, // 指示 skb 是否属于 IPVS
				// 指示该 skb 是否已被查看，避免重复统计
				peeked:1,     // 此包已被查看，不再统计
				nf_trace:1;   // netfilter 数据包跟踪标志
	kmemcheck_bitfield_end(flags1);	// 结束使用位字段检查
	// skb 携带的协议类型，采用网络字节顺序
	__be16			protocol;     // 来自驱动程序的数据包协议

	void			(*destructor)(struct sk_buff *skb); // skb 的析构函数，用于在释放 skb 时执行清理工作
#if defined(CONFIG_NF_CONNTRACK) || defined(CONFIG_NF_CONNTRACK_MODULE)
	struct nf_conntrack	*nfct;      // 关联的连接跟踪
	// 用于 netfilter 连接跟踪的重组 skb 指针
	struct sk_buff		*nfct_reasm; // netfilter conntrack 重组指针
#endif
#ifdef CONFIG_BRIDGE_NETFILTER
	struct nf_bridge_info	*nf_bridge; // 保存桥接相关信息的结构
#endif

	// 到达设备的接口索引
	// 接收该 skb 的接口索引
	int			skb_iif;   // 到达设备的 ifindex
#ifdef CONFIG_NET_SCHED
	// 流量控制索引
	__u16			tc_index;  /* traffic control index */
#ifdef CONFIG_NET_CLS_ACT
	// 流量控制决策
	__u16			tc_verd;   /* traffic control verdict */
#endif
#endif

	// 标志位字段
	kmemcheck_bitfield_begin(flags2);	// 开始第二个位字段检查
	 // 队列映射，用于多队列设备
	__u16			queue_mapping:16; // 多队列设备的队列映射
#ifdef CONFIG_IPV6_NDISC_NODETYPE
	// 节点类型，用于 IPv6 邻居发现
	__u8			ndisc_nodetype:2; // 链路层的路由器类型
#endif
	kmemcheck_bitfield_end(flags2);	// 结束使用位字段检查

	/* 0/14 bit hole */
	// 未使用的位，留作未来使用或对齐

#ifdef CONFIG_NET_DMA
	dma_cookie_t		dma_cookie; // DMA 操作的标识符，用于追踪 DMA 传输状态
#endif
#ifdef CONFIG_NETWORK_SECMARK
	__u32			secmark;    // 安全标记，用于网络安全策略的实施
#endif
	union {
		// 用于各种标记，例如用于过滤或路由决策
		__u32		mark;       // 通用包标记
		// 用于统计被丢弃的数据包数量
		__u32		dropcount;  // 丢包计数
	};

	__u16			vlan_tci;   // VLAN 标签控制信息，包括优先级和VLAN ID

	sk_buff_data_t		transport_header; // 传输层头部的位置（偏移量或指针）
	sk_buff_data_t		network_header;   // 网络层头部的位置（偏移量或指针）
	sk_buff_data_t		mac_header;       // 链路层头部的位置（偏移量或指针）
	/*
	 * head：线性区的起始地址
	 * data：数据的起始地址
	 * tail：数据的结束地址
	 * end：线性区的结束地址
	 */
	/* These elements must be at the end, see alloc_skb() for details.  */
	/* 这些元素必须位于结构体的末尾，具体原因请参考 alloc_skb() 函数的文档。 */
	// tail可能保存的是可能是相对于head的偏移量，也可能是绝对地址，就是tail位置
	sk_buff_data_t		tail;             // 指向 skb 数据部分的尾部
	sk_buff_data_t		end;              // 指向 skb 缓冲区的末尾
	unsigned char		*head,            // 指向 skb 缓冲区的头部
				*data;            	// 指向 skb 数据部分的起始位置
	unsigned int		truesize;         // skb 缓冲区的实际大小（包括所有预留和已使用的空间）
	// 引用计数，用于管理 skb 的生命周期
	atomic_t		users;            // 用户计数
};

#ifdef __KERNEL__
/*
 *	Handling routines are only of interest to the kernel
 */
// 以下的处理例程只对内核有兴趣
#include <linux/slab.h>

#include <asm/system.h>

// 获取 sk_buff 结构中的目的地（dst_entry 结构）。
static inline struct dst_entry *skb_dst(const struct sk_buff *skb)
{
	return (struct dst_entry *)skb->_skb_dst;	// 返回 skb 的目的地条目
}

// 设置 sk_buff 结构中的目的地（dst_entry 结构）。
static inline void skb_dst_set(struct sk_buff *skb, struct dst_entry *dst)
{
	skb->_skb_dst = (unsigned long)dst;				// 设置 skb 的目的地条目
}

// 返回关联于特定 sk_buff 的路由表项，其实质是调用 skb_dst 并进行类型转换。
static inline struct rtable *skb_rtable(const struct sk_buff *skb)
{
	return (struct rtable *)skb_dst(skb);			// 返回与 skb 关联的路由表条目
}

extern void kfree_skb(struct sk_buff *skb);  // 声明用于释放 skb 的函数
extern void consume_skb(struct sk_buff *skb);  // 声明用于处理并最终释放 skb 的函数
extern void __kfree_skb(struct sk_buff *skb);  // 声明一个内部使用的释放 skb 的函数
extern struct sk_buff *__alloc_skb(unsigned int size,
				   gfp_t priority, int fclone, int node);
// 声明一个用于分配 skb 的函数，参数包括大小、内存分配标志、是否克隆、节点标识
static inline struct sk_buff *alloc_skb(unsigned int size,
					gfp_t priority)
{
	return __alloc_skb(size, priority, 0, -1);	// 简化版的 skb 分配函数，不使用克隆和节点指定
}

static inline struct sk_buff *alloc_skb_fclone(unsigned int size,
					       gfp_t priority)
{
	return __alloc_skb(size, priority, 1, -1);	// 分配一个支持克隆的 skb
}

// 检查 skb 是否可以回收使用，确保其大小适当
extern int skb_recycle_check(struct sk_buff *skb, int skb_size);

// 将 src skb 的内容转移到 dst skb，并释放 src
extern struct sk_buff *skb_morph(struct sk_buff *dst, struct sk_buff *src);
// 克隆一个 skb，新 skb 和原 skb 共享数据部分
extern struct sk_buff *skb_clone(struct sk_buff *skb,
				 gfp_t priority);
// 完全复制一个 skb，包括其数据
extern struct sk_buff *skb_copy(const struct sk_buff *skb,
				gfp_t priority);
// 部分复制 skb，通常用于当数据包需要在网络层处理时
extern struct sk_buff *pskb_copy(struct sk_buff *skb,
				 gfp_t gfp_mask);
// 扩展 skb 的头部和尾部空间，通常用于增加数据处理所需的空间
extern int	       pskb_expand_head(struct sk_buff *skb,
					int nhead, int ntail,
					gfp_t gfp_mask);
// 重新分配 skb，确保有足够的头部空间
extern struct sk_buff *skb_realloc_headroom(struct sk_buff *skb,
					    unsigned int headroom);
// 复制并扩展 skb，调整头部和尾部空间
extern struct sk_buff *skb_copy_expand(const struct sk_buff *skb,
				       int newheadroom, int newtailroom,
				       gfp_t priority);
// 将 skb 数据转换为散布/聚集列表，用于 DMA 操作
extern int	       skb_to_sgvec(struct sk_buff *skb,
				    struct scatterlist *sg, int offset,
				    int len);
// 复制 skb 的写时拷贝（COW，Copy-On-Write）数据部分
extern int	       skb_cow_data(struct sk_buff *skb, int tailbits,
				    struct sk_buff **trailer);
// 为 skb 数据添加填充，保证其按照特定对齐要求结束
extern int	       skb_pad(struct sk_buff *skb, int pad);
// 定义宏，将 consume_skb 作为 dev_kfree_skb 的别名
#define dev_kfree_skb(a)	consume_skb(a)
// 定义宏，将 kfree_skb_clean 作为 dev_consume_skb 的别名
#define dev_consume_skb(a)	kfree_skb_clean(a)
// 当 skb 超过预定大小时触发的 panic 函数
extern void	      skb_over_panic(struct sk_buff *skb, int len,
				     void *here);
// 当 skb 未达到预期大小时触发的 panic 函数
extern void	      skb_under_panic(struct sk_buff *skb, int len,
				      void *here);

// 用于处理分段数据包的函数声明以及用于跟踪 sk_buff 分段读取状态的结构定义。
/*
 * 该函数用于将数据附加到 skb 的片段（fragments）上。
 * @sk: 相关联的套接字。
 * @skb: 目标 skb 结构。
 * @getfrag: 回调函数，负责从某个源位置获取数据并将其复制到目标位置。
 *      - void *from: 数据源的起始指针。
 *      - char *to: 目标内存地址。
 *      - int offset: 从源地址开始的偏移量。
 *      - int len: 需要复制的数据长度。
 *      - int odd: 可以用于传递额外的状态或标志。
 *      - struct sk_buff *skb: 目标 skb。
 * @from: 指向源数据的指针。
 * @length: 需要复制的数据总长度。
 */
extern int skb_append_datato_frags(struct sock *sk, struct sk_buff *skb,
			int getfrag(void *from, char *to, int offset,
			int len,int odd, struct sk_buff *skb),
			void *from, int length);

/*
 * skb_seq_state 结构用于跟踪和管理在 skb 序列或分片中的数据读取状态。
 * 这个结构通常用于执行分片的顺序读取和处理。
 */
struct skb_seq_state {
	__u32		lower_offset;   // 当前读取操作的开始偏移量
	__u32		upper_offset;   // 当前读取操作的结束偏移量
	__u32		frag_idx;       // 当前处理的片段索引
	__u32		stepped_offset; // 在整个 skb 序列中已处理的偏移量。
	struct sk_buff	*root_skb;     // 指向整个 skb 链的根 skb。
	struct sk_buff	*cur_skb;      // 指向当前处理的 sk_buff 的指针
	__u8		*frag_data;     // 指向当前片段数据的指针。
};

// 准备从 skb 中进行序列读取
extern void	      skb_prepare_seq_read(struct sk_buff *skb,
					   unsigned int from, unsigned int to,
					   struct skb_seq_state *st);
// 进行序列读取，返回读取的字节数
extern unsigned int   skb_seq_read(unsigned int consumed, const u8 **data,
				   struct skb_seq_state *st);
// 中断序列读取
extern void	      skb_abort_seq_read(struct skb_seq_state *st);

// 在 skb 中查找文本
extern unsigned int   skb_find_text(struct sk_buff *skb, unsigned int from,
				    unsigned int to, struct ts_config *config,
				    struct ts_state *state);

#ifdef NET_SKBUFF_DATA_USES_OFFSET
// 返回 skb 的结束指针（使用偏移量）
static inline unsigned char *skb_end_pointer(const struct sk_buff *skb)
{
	return skb->head + skb->end;	// 返回 skb 的结束指针（使用偏移量）
}
#else
// 返回 skb 的结束指针（不使用偏移量）
static inline unsigned char *skb_end_pointer(const struct sk_buff *skb)
{
	return skb->end;	// 返回 skb 的结束指针（不使用偏移量）
}
#endif

/* Internal */
// 定义宏，获取 skb 的共享信息结构体
#define skb_shinfo(SKB)	((struct skb_shared_info *)(skb_end_pointer(SKB)))

// 获取 skb 的硬件时间戳信息
static inline struct skb_shared_hwtstamps *skb_hwtstamps(struct sk_buff *skb)
{
	return &skb_shinfo(skb)->hwtstamps;	// 返回指向硬件时间戳的指针
}

// 获取 skb 的传输标记信息
static inline union skb_shared_tx *skb_tx(struct sk_buff *skb)
{
	return &skb_shinfo(skb)->tx_flags;	// 返回指向传输标记的指针
}

/**
 *	skb_queue_empty - check if a queue is empty
 *	@list: queue head
 *
 *	Returns true if the queue is empty, false otherwise.
 */
/**
 *	skb_queue_empty - 检查队列是否为空
 *	@list: 队列头
 *
 *	如果队列为空返回 true，否则返回 false。
 */
static inline int skb_queue_empty(const struct sk_buff_head *list)
{
	return list->next == (struct sk_buff *)list;	// 检查队列头的下一个元素是否指向自身
}

/**
 *	skb_queue_is_last - check if skb is the last entry in the queue
 *	@list: queue head
 *	@skb: buffer
 *
 *	Returns true if @skb is the last buffer on the list.
 */
/**
 *	skb_queue_is_last - 检查 skb 是否是队列中的最后一个条目
 *	@list: 队列头
 *	@skb: 缓冲区
 *
 *	如果 @skb 是列表上的最后一个缓冲区，则返回 true。
 */
static inline bool skb_queue_is_last(const struct sk_buff_head *list,
				     const struct sk_buff *skb)
{
	return (skb->next == (struct sk_buff *) list);	// 检查 skb 的下一个元素是否指向队列头
}

/**
 *	skb_queue_is_first - check if skb is the first entry in the queue
 *	@list: queue head
 *	@skb: buffer
 *
 *	Returns true if @skb is the first buffer on the list.
 */
/**
 *	skb_queue_is_first - 检查 skb 是否是队列中的第一个条目
 *	@list: 队列头
 *	@skb: 缓冲区
 *
 *	如果 @skb 是列表上的第一个缓冲区，则返回 true。
 */
static inline bool skb_queue_is_first(const struct sk_buff_head *list,
				      const struct sk_buff *skb)
{
	return (skb->prev == (struct sk_buff *) list);	// 检查 skb 的前一个元素是否是队列头
}

/**
 *	skb_queue_next - return the next packet in the queue
 *	@list: queue head
 *	@skb: current buffer
 *
 *	Return the next packet in @list after @skb.  It is only valid to
 *	call this if skb_queue_is_last() evaluates to false.
 */
/**
 *	skb_queue_next - 返回队列中的下一个数据包
 *	@list: 队列头
 *	@skb: 当前缓冲区
 *
 *	在 @list 中返回 @skb 之后的下一个数据包。只有当 skb_queue_is_last() 返回 false 时调用此函数才有效。
 */
static inline struct sk_buff *skb_queue_next(const struct sk_buff_head *list,
					     const struct sk_buff *skb)
{
	/* This BUG_ON may seem severe, but if we just return then we
	 * are going to dereference garbage.
	 */
	/* 此 BUG_ON 可能看起来很严重，但如果我们只是返回，我们将会引用无效内存。 */
	BUG_ON(skb_queue_is_last(list, skb));	// 断言，确保当前 skb 不是队列中的最后一个，否则将触发内核错误
	return skb->next;	// 返回 skb 的下一个元素
}

/**
 *	skb_queue_prev - return the prev packet in the queue
 *	@list: queue head
 *	@skb: current buffer
 *
 *	Return the prev packet in @list before @skb.  It is only valid to
 *	call this if skb_queue_is_first() evaluates to false.
 */
/**
 *	skb_queue_prev - 返回队列中的前一个数据包
 *	@list: 队列头
 *	@skb: 当前缓冲区
 *
 *	返回 @list 中 @skb 之前的前一个数据包。只有当 skb_queue_is_first() 返回 false 时调用此函数才有效。
 */
static inline struct sk_buff *skb_queue_prev(const struct sk_buff_head *list,
					     const struct sk_buff *skb)
{
	/* This BUG_ON may seem severe, but if we just return then we
	 * are going to dereference garbage.
	 */
	/* 此 BUG_ON 可能看起来很严重，但如果我们只是返回，我们将会引用无效内存。 */
	BUG_ON(skb_queue_is_first(list, skb));  // 断言，确保当前 skb 不是队列中的第一个，否则将触发内核错误
	return skb->prev;  // 返回 skb 的前一个元素
}

/**
 *	skb_get - reference buffer
 *	@skb: buffer to reference
 *
 *	Makes another reference to a socket buffer and returns a pointer
 *	to the buffer.
 */
/**
 *	skb_get - 引用缓冲区
 *	@skb: 要引用的缓冲区
 *
 *	对一个 socket 缓冲区进行额外的引用，并返回指向该缓冲区的指针。
 */
static inline struct sk_buff *skb_get(struct sk_buff *skb)
{
	atomic_inc(&skb->users);  // 原子地增加 skb 的用户计数
	return skb;  // 返回 skb 的指针
}

/*
 * If users == 1, we are the only owner and are can avoid redundant
 * atomic change.
 */
/*
 * 如果 users == 1，我们是唯一的所有者，可以避免冗余的原子操作。
 */

/**
 *	skb_cloned - is the buffer a clone
 *	@skb: buffer to check
 *
 *	Returns true if the buffer was generated with skb_clone() and is
 *	one of multiple shared copies of the buffer. Cloned buffers are
 *	shared data so must not be written to under normal circumstances.
 */
/**
 *	skb_cloned - 检查缓冲区是否为克隆
 *	@skb: 要检查的缓冲区
 *
 *	如果缓冲区是通过 skb_clone() 生成的，并且是缓冲区的多个共享副本之一，则返回 true。
 *	克隆的缓冲区是共享数据，通常情况下不应该被写入。
 */
// 这个函数用于判断一个 skb 是否为通过 skb_clone() 方法产生的克隆。这种克隆的 skb 共享数据部分的内存，
// 所以通常不应直接修改它们，除非确认已经安全地进行了数据复制（通过如 skb_copy() 等方式）。
static inline int skb_cloned(const struct sk_buff *skb)
{
	// 返回 1 的条件为 skb->cloned = 1 并且 dataref 的低
	// 16bit 部分（整个数据区的引用计数）不为 1；
	return skb->cloned &&	// 检查 skb 是否被标记为克隆
	       (atomic_read(&skb_shinfo(skb)->dataref) & SKB_DATAREF_MASK) != 1;	// 检查数据引用计数是否大于1
}

/**
 *	skb_header_cloned - is the header a clone
 *	@skb: buffer to check
 *
 *	Returns true if modifying the header part of the buffer requires
 *	the data to be copied.
 */
/**
 *	skb_header_cloned - 检查头部是否为克隆
 *	@skb: 要检查的缓冲区
 *
 *	如果修改缓冲区的头部部分需要复制数据，则返回 true。
 */
// 这个函数检查 skb 的头部是否被克隆，即是否需要进行写时复制（copy-on-write, COW）。
// 如果头部的引用计数大于1，修改前需要先复制数据，以避免对其他共享此数据的 skb 产生影响。
static inline int skb_header_cloned(const struct sk_buff *skb)
{
	int dataref;

	if (!skb->cloned)  // 如果 skb 没有被克隆，直接返回 0（无需复制）
		return 0;

	dataref = atomic_read(&skb_shinfo(skb)->dataref);  // 读取数据引用计数
	dataref = (dataref & SKB_DATAREF_MASK) - (dataref >> SKB_DATAREF_SHIFT);  // 计算头部部分的引用计数
	// ：判断数据区里的 header 部分是否被 clone，返回
	// 1 的条件为 skb->cloned = 1 并且 dataref 的低 16bits（整个数据区的引
	// 用计数） - 高 16bits（payload 部分引用计数）的差值不为 1；
	return dataref != 1;  // 如果头部引用计数大于1，则需要复制
}

/**
 *	skb_header_release - release reference to header
 *	@skb: buffer to operate on
 *
 *	Drop a reference to the header part of the buffer.  This is done
 *	by acquiring a payload reference.  You must not read from the header
 *	part of skb->data after this.
 */
/**
 *	skb_header_release - 释放对头部的引用
 *	@skb: 要操作的缓冲区
 *
 *	释放对缓冲区头部的一个引用。这通过获取一个负载（payload）引用来完成。
 *	执行此操作后，你不得再从 skb->data 的头部部分读取数据。
 */
static inline void skb_header_release(struct sk_buff *skb)
{
	BUG_ON(skb->nohdr);  // 如果 nohdr 标志已设置，触发错误
	skb->nohdr = 1;  // 设置 nohdr 标志，表示头部不应再被访问
	atomic_add(1 << SKB_DATAREF_SHIFT, &skb_shinfo(skb)->dataref);  // 增加对负载部分的引用计数
}

/**
 *	skb_shared - is the buffer shared
 *	@skb: buffer to check
 *
 *	Returns true if more than one person has a reference to this
 *	buffer.
 */
/**
 *	skb_shared - 检查缓冲区是否被共享
 *	@skb: 要检查的缓冲区
 *
 *	如果有多于一个引用指向这个缓冲区，则返回 true。
 */
static inline int skb_shared(const struct sk_buff *skb)
{
	return atomic_read(&skb->users) != 1;  // 检查引用计数是否不等于1，若不等于1则表示被共享
}

/**
 *	skb_share_check - check if buffer is shared and if so clone it
 *	@skb: buffer to check
 *	@pri: priority for memory allocation
 *
 *	If the buffer is shared the buffer is cloned and the old copy
 *	drops a reference. A new clone with a single reference is returned.
 *	If the buffer is not shared the original buffer is returned. When
 *	being called from interrupt status or with spinlocks held pri must
 *	be GFP_ATOMIC.
 *
 *	NULL is returned on a memory allocation failure.
 */
/**
 *	skb_share_check - 检查缓冲区是否被共享，如果是，则克隆它
 *	@skb: 要检查的缓冲区
 *	@pri: 内存分配的优先级
 *
 *	如果缓冲区被共享，则克隆该缓冲区，并且旧副本减少一个引用。返回一个具有单一引用的新克隆。
 *	如果缓冲区没有被共享，则返回原始缓冲区。当从中断状态调用或持有自旋锁时，pri 必须是 GFP_ATOMIC。
 *
 *	如果内存分配失败，则返回 NULL。
 */
static inline struct sk_buff *skb_share_check(struct sk_buff *skb,
					      gfp_t pri)
{
	might_sleep_if(pri & __GFP_WAIT);  // 如果允许休眠，则检查调用上下文是否适合休眠
	if (skb_shared(skb)) {  // 检查 skb 是否被共享
		struct sk_buff *nskb = skb_clone(skb, pri);  // 克隆 skb
		kfree_skb(skb);  // 释放原始 skb
		skb = nskb;  // 更新指针到新克隆的 skb
	}
	return skb;  // 返回处理后的 skb，可能是原始的或新克隆的
}

/*
 *	Copy shared buffers into a new sk_buff. We effectively do COW on
 *	packets to handle cases where we have a local reader and forward
 *	and a couple of other messy ones. The normal one is tcpdumping
 *	a packet thats being forwarded.
 */
/*
 *	复制共享缓冲区到一个新的 sk_buff。我们实际上在数据包上执行 COW（写时复制）操作，
 *	以处理我们有一个本地读取器和转发以及其他一些复杂情况的场景。常见的情况是对正在被
 *	转发的数据包进行 tcpdump 捕获。
 */

/**
 *	skb_unshare - make a copy of a shared buffer
 *	@skb: buffer to check
 *	@pri: priority for memory allocation
 *
 *	If the socket buffer is a clone then this function creates a new
 *	copy of the data, drops a reference count on the old copy and returns
 *	the new copy with the reference count at 1. If the buffer is not a clone
 *	the original buffer is returned. When called with a spinlock held or
 *	from interrupt state @pri must be %GFP_ATOMIC
 *
 *	%NULL is returned on a memory allocation failure.
 */
/**
 *	skb_unshare - 使共享缓冲区的一个副本
 *	@skb: 要检查的缓冲区
 *	@pri: 内存分配的优先级
 *
 *	如果套接字缓冲区是一个克隆，则此函数创建数据的一个新副本，
 *	减少旧副本的引用计数并返回新副本，新副本的引用计数为1。
 *	如果缓冲区不是克隆，则返回原始缓冲区。当在持有自旋锁或
 *	从中断状态调用时，@pri 必须是 %GFP_ATOMIC。
 *
 *	如果内存分配失败，则返回 %NULL。
 */
static inline struct sk_buff *skb_unshare(struct sk_buff *skb,
					  gfp_t pri)
{
	might_sleep_if(pri & __GFP_WAIT);  // 如果允许休眠，则检查调用上下文是否适合休眠
	if (skb_cloned(skb)) {  // 检查 skb 是否是克隆的
		struct sk_buff *nskb = skb_copy(skb, pri);  // 创建 skb 的一个完整复制
		/* Free our shared copy */
		kfree_skb(skb);	// 释放我们共享的副本
		skb = nskb;
	}
	return skb;  // 返回处理后的 skb，可能是新复制的或原始的
}

/**
 *	skb_peek - peek at the head of an &sk_buff_head
 *	@list_: list to peek at
 *
 *	Peek an &sk_buff. Unlike most other operations you _MUST_
 *	be careful with this one. A peek leaves the buffer on the
 *	list and someone else may run off with it. You must hold
 *	the appropriate locks or have a private queue to do this.
 *
 *	Returns %NULL for an empty list or a pointer to the head element.
 *	The reference count is not incremented and the reference is therefore
 *	volatile. Use with caution.
 */
/**
 *	skb_peek - 查看 sk_buff_head 的头部
 *	@list_: 要查看的列表
 *
 *	查看一个 sk_buff。与大多数其他操作不同，使用此操作时必须非常小心。
 *	查看操作会保留缓冲区在列表上，其他人可能会取走它。你必须持有适当的锁
 *	或拥有一个私有队列才能执行此操作。
 *
 *	如果列表为空，则返回 %NULL，或返回头元素的指针。
 *	引用计数不会增加，因此引用是不稳定的。使用时需谨慎。
 */
static inline struct sk_buff *skb_peek(struct sk_buff_head *list_)
{
	struct sk_buff *list = ((struct sk_buff *)list_)->next;  // 获取列表的第一个元素
	if (list == (struct sk_buff *)list_)  // 如果列表为空（即头元素的下一个元素指向自身）
		list = NULL;  // 设置 list 为 NULL
	return list;  // 返回列表的头元素或 NULL（如果列表为空）
}

/**
 *	skb_peek_tail - peek at the tail of an &sk_buff_head
 *	@list_: list to peek at
 *
 *	Peek an &sk_buff. Unlike most other operations you _MUST_
 *	be careful with this one. A peek leaves the buffer on the
 *	list and someone else may run off with it. You must hold
 *	the appropriate locks or have a private queue to do this.
 *
 *	Returns %NULL for an empty list or a pointer to the tail element.
 *	The reference count is not incremented and the reference is therefore
 *	volatile. Use with caution.
 */
/**
 *	skb_peek_tail - 查看 sk_buff_head 的尾部
 *	@list_: 要查看的列表
 *
 *	查看一个 sk_buff。与大多数其他操作不同，使用此操作时必须非常小心。
 *	查看操作会保留缓冲区在列表上，其他人可能会取走它。你必须持有适当的锁
 *	或拥有一个私有队列才能执行此操作。
 *
 *	如果列表为空，则返回 %NULL，或返回尾部元素的指针。
 *	引用计数不会增加，因此引用是不稳定的。使用时需谨慎。
 */
static inline struct sk_buff *skb_peek_tail(struct sk_buff_head *list_)
{
	struct sk_buff *list = ((struct sk_buff *)list_)->prev;  // 获取列表的最后一个元素
	if (list == (struct sk_buff *)list_)  // 如果列表为空（即尾元素的前一个元素指向自身）
		list = NULL;  // 设置 list 为 NULL
	return list;  // 返回列表的尾部元素或 NULL（如果列表为空）
}

/**
 *	skb_queue_len	- get queue length
 *	@list_: list to measure
 *
 *	Return the length of an &sk_buff queue.
 */
/**
 *	skb_queue_len	- 获取队列长度
 *	@list_: 要测量的列表
 *
 *	返回一个 sk_buff 队列的长度。
 */
static inline __u32 skb_queue_len(const struct sk_buff_head *list_)
{
	return list_->qlen;  // 返回队列的长度
}

/**
 *	__skb_queue_head_init - initialize non-spinlock portions of sk_buff_head
 *	@list: queue to initialize
 *
 *	This initializes only the list and queue length aspects of
 *	an sk_buff_head object.  This allows to initialize the list
 *	aspects of an sk_buff_head without reinitializing things like
 *	the spinlock.  It can also be used for on-stack sk_buff_head
 *	objects where the spinlock is known to not be used.
 */
/**
 *	__skb_queue_head_init - 初始化 sk_buff_head 的非自旋锁部分
 *	@list: 要初始化的队列
 *
 *	此函数仅初始化 sk_buff_head 对象的列表和队列长度方面。
 *	这允许在不重新初始化如自旋锁之类的其他组件的情况下，初始化 sk_buff_head 的列表方面。
 *	它也可用于栈上的 sk_buff_head 对象，其中已知不使用自旋锁。
 */
// 专门用于初始化 sk_buff_head 的链表部分（不包括自旋锁）。这对于需要自定义锁行为的场景或用于临时（栈上）队列对象非常有用。
static inline void __skb_queue_head_init(struct sk_buff_head *list)
{
	list->prev = list->next = (struct sk_buff *)list;  // 将 prev 和 next 指向自身，表示队列为空
	list->qlen = 0;  // 设置队列长度为 0
}

/*
 * This function creates a split out lock class for each invocation;
 * this is needed for now since a whole lot of users of the skb-queue
 * infrastructure in drivers have different locking usage (in hardirq)
 * than the networking core (in softirq only). In the long run either the
 * network layer or drivers should need annotation to consolidate the
 * main types of usage into 3 classes.
 */
/*
 * 此函数为每次调用创建一个分离的锁类；
 * 由于驱动中 skb-queue 的许多用户在使用锁的方式上（在硬中断中）
 * 与网络核心（仅在软中断中）不同，目前这是必需的。
 * 从长远来看，网络层或驱动应需要注释来整合
 * 主要使用类型到3个类中。
 */
// 这个函数在初始化链表的基础上，还会初始化自旋锁。它是更常规的初始化方式，用于正常情况下需要自旋锁保护的 sk_buff_head 对象。
static inline void skb_queue_head_init(struct sk_buff_head *list)
{
	spin_lock_init(&list->lock);  // 初始化自旋锁
	__skb_queue_head_init(list);  // 调用 __skb_queue_head_init 初始化队列的其他部分
}

// 该函数除了执行 skb_queue_head_init 的所有操作外，还通过 lockdep_set_class 设置锁的类别。这主要用于内核的锁依赖检测系统，帮助开发者识别和避免潜在的死锁问题。
static inline void skb_queue_head_init_class(struct sk_buff_head *list,
		struct lock_class_key *class)
{
	skb_queue_head_init(list);  // 初始化队列头
	lockdep_set_class(&list->lock, class);  // 设置锁的依赖类别，用于锁依赖检测
}

/*
 *	Insert an sk_buff on a list.
 *
 *	The "__skb_xxxx()" functions are the non-atomic ones that
 *	can only be called with interrupts disabled.
 */
/*
 *	在列表中插入一个 sk_buff。
 *
 *	"__skb_xxxx()" 函数是非原子的，只能在中断禁用的情况下调用。
 */
// 向外部暴露的函数，用于将 newsk 插入到 old 所在的 list 中。
extern void        skb_insert(struct sk_buff *old, struct sk_buff *newsk, struct sk_buff_head *list);

// 直接操作 sk_buff 节点，将 newsk 插入到 prev 和 next 之间。它直接修改了节点的 prev 和 next 指针来完成插入，并更新了链表的长度计数。
static inline void __skb_insert(struct sk_buff *newsk,
				struct sk_buff *prev, struct sk_buff *next,
				struct sk_buff_head *list)
{
	newsk->next = next;  // 设置 newsk 的下一个节点为 next
	newsk->prev = prev;  // 设置 newsk 的前一个节点为 prev
	next->prev  = prev->next = newsk;  // 将 newsk 插入到 prev 和 next 之间
	list->qlen++;  // 链表长度增加
}

// 用于将一个完整的 sk_buff_head 链表切片插入到另一个链表中，位置在 prev 和 next 之间。
static inline void __skb_queue_splice(const struct sk_buff_head *list,
				      struct sk_buff *prev,
				      struct sk_buff *next)
{
	struct sk_buff *first = list->next;  // 获取要插入的链表的第一个节点
	struct sk_buff *last = list->prev;   // 获取要插入的链表的最后一个节点

	first->prev = prev;  // 将要插入的链表的第一个节点与 prev 连接
	prev->next = first;  // 将 prev 与要插入的链表的第一个节点连接

	last->next = next;   // 将要插入的链表的最后一个节点与 next 连接
	next->prev = last;   // 将 next 与要插入的链表的最后一个节点连接
}

/**
 *	skb_queue_splice - join two skb lists, this is designed for stacks
 *	@list: the new list to add
 *	@head: the place to add it in the first list
 */
/**
 *	skb_queue_splice - 将两个 skb 列表连接起来，这主要用于栈
 *	@list: 要添加的新列表
 *	@head: 第一个列表中要添加的位置
 */
static inline void skb_queue_splice(const struct sk_buff_head *list,
				    struct sk_buff_head *head)
{
	if (!skb_queue_empty(list)) {  // 如果要添加的列表不为空
		__skb_queue_splice(list, (struct sk_buff *) head, head->next);  // 调用 __skb_queue_splice 将 list 插入到 head 之后
		head->qlen += list->qlen;  // 更新队列长度
	}
}

/**
 *	skb_queue_splice - join two skb lists and reinitialise the emptied list
 *	@list: the new list to add
 *	@head: the place to add it in the first list
 *
 *	The list at @list is reinitialised
 */
/**
 *	skb_queue_splice - 将两个 skb 列表连接起来并重新初始化被清空的列表
 *	@list: 要添加的新列表
 *	@head: 第一个列表中要添加的位置
 *
 *	@list 处的列表被重新初始化
 */
static inline void skb_queue_splice_init(struct sk_buff_head *list,
					 struct sk_buff_head *head)
{
	if (!skb_queue_empty(list)) {  // 如果要添加的列表不为空
		__skb_queue_splice(list, (struct sk_buff *) head, head->next);  // 将 list 插入到 head 的 next 之前
		head->qlen += list->qlen;  // 更新队列长度
		__skb_queue_head_init(list);  // 重新初始化 list，清空其中的元素
	}
}

/**
 *	skb_queue_splice_tail - join two skb lists, each list being a queue
 *	@list: the new list to add
 *	@head: the place to add it in the first list
 */
/**
 *	skb_queue_splice_tail - 将两个 skb 列表连接起来，每个列表都是一个队列
 *	@list: 要添加的新列表
 *	@head: 第一个列表中要添加的位置
 */
static inline void skb_queue_splice_tail(const struct sk_buff_head *list,
					 struct sk_buff_head *head)
{
	if (!skb_queue_empty(list)) {  // 如果要添加的列表不为空
		__skb_queue_splice(list, head->prev, (struct sk_buff *) head);  // 将 list 插入到 head 的尾部
		head->qlen += list->qlen;  // 更新 head 队列的长度
	}
}

/**
 *	skb_queue_splice_tail - join two skb lists and reinitialise the emptied list
 *	@list: the new list to add
 *	@head: the place to add it in the first list
 *
 *	Each of the lists is a queue.
 *	The list at @list is reinitialised
 */
/**
 *	skb_queue_splice_tail - 将两个 skb 列表连接起来并重新初始化被清空的列表
 *	@list: 要添加的新列表
 *	@head: 第一个列表中要添加的位置
 *
 *	每个列表都是一个队列。
 *	在 @list 处的列表被重新初始化。
 */
static inline void skb_queue_splice_tail_init(struct sk_buff_head *list,
					      struct sk_buff_head *head)
{
	if (!skb_queue_empty(list)) {  // 如果要添加的列表不为空
		__skb_queue_splice(list, head->prev, (struct sk_buff *) head);  // 将 list 插入到 head 的尾部
		head->qlen += list->qlen;  // 更新 head 队列的长度
		__skb_queue_head_init(list);  // 重新初始化 list，使其成为空队列
	}
}

/**
 *	__skb_queue_after - queue a buffer at the list head
 *	@list: list to use
 *	@prev: place after this buffer
 *	@newsk: buffer to queue
 *
 *	Queue a buffer int the middle of a list. This function takes no locks
 *	and you must therefore hold required locks before calling it.
 *
 *	A buffer cannot be placed on two lists at the same time.
 */
/**
 *	__skb_queue_after - 在列表头部之后队列一个缓冲区
 *	@list: 要使用的列表
 *	@prev: 在此缓冲区之后放置
 *	@newsk: 要队列的缓冲区
 *
 *	在列表的中间队列一个缓冲区。此函数不获取锁，
 *	因此在调用它之前，你必须持有所需的锁。
 *
 *	一个缓冲区不能同时被放置在两个列表上。
 */
static inline void __skb_queue_after(struct sk_buff_head *list,
				     struct sk_buff *prev,
				     struct sk_buff *newsk)
{
	__skb_insert(newsk, prev, prev->next, list);  // 调用 __skb_insert 将 newsk 插入到 prev 和 prev->next 之间
}

// 此函数声明用于将 newsk 添加到 old 所在的 list 末尾，具体实现未在此段代码中显示。
extern void skb_append(struct sk_buff *old, struct sk_buff *newsk,
		       struct sk_buff_head *list);

/**
 *	__skb_queue_before - 在指定的下一个缓冲区之前队列一个缓冲区
 *	@list: 要使用的列表
 *	@next: 在此缓冲区之前放置
 *	@newsk: 要队列的缓冲区
 *
 *	在列表的中间队列一个缓冲区。此函数不获取锁，
 *	因此在调用它之前，你必须持有所需的锁。
 */
static inline void __skb_queue_before(struct sk_buff_head *list,
				      struct sk_buff *next,
				      struct sk_buff *newsk)
{
	__skb_insert(newsk, next->prev, next, list);  // 调用 __skb_insert 将 newsk 插入到 next->prev 和 next 之间
}

/**
 *	__skb_queue_head - queue a buffer at the list head
 *	@list: list to use
 *	@newsk: buffer to queue
 *
 *	Queue a buffer at the start of a list. This function takes no locks
 *	and you must therefore hold required locks before calling it.
 *
 *	A buffer cannot be placed on two lists at the same time.
 */
/**
 *	__skb_queue_head - 在列表头部插入一个缓冲区
 *	@list: 要使用的列表
 *	@newsk: 要插入的缓冲区
 *
 *	在列表的开始处插入一个缓冲区。此函数不获取锁，
 *	因此在调用它之前，你必须持有所需的锁。
 *
 *	一个缓冲区不能同时被放置在两个列表上。
 */
extern void skb_queue_head(struct sk_buff_head *list, struct sk_buff *newsk);
static inline void __skb_queue_head(struct sk_buff_head *list,
				    struct sk_buff *newsk)
{
	__skb_queue_after(list, (struct sk_buff *)list, newsk);  // 利用 __skb_queue_after 在 list 的头部插入 newsk
}

/**
 *	__skb_queue_tail - queue a buffer at the list tail
 *	@list: list to use
 *	@newsk: buffer to queue
 *
 *	Queue a buffer at the end of a list. This function takes no locks
 *	and you must therefore hold required locks before calling it.
 *
 *	A buffer cannot be placed on two lists at the same time.
 */
/**
 *	__skb_queue_tail - 在列表尾部插入一个缓冲区
 *	@list: 要使用的列表
 *	@newsk: 要插入的缓冲区
 *
 *	在列表的末尾插入一个缓冲区。此函数不获取锁，
 *	因此在调用它之前，你必须持有所需的锁。
 *
 *	一个缓冲区不能同时被放置在两个列表上。
 */
extern void skb_queue_tail(struct sk_buff_head *list, struct sk_buff *newsk);
static inline void __skb_queue_tail(struct sk_buff_head *list,
				   struct sk_buff *newsk)
{
	__skb_queue_before(list, (struct sk_buff *)list, newsk);  // 利用 __skb_queue_before 在 list 的尾部插入 newsk
}

/*
 * remove sk_buff from list. _Must_ be called atomically, and with
 * the list known..
 */
/*
 * 从列表中移除 sk_buff。必须在原子环境中调用，并且确知列表。
 */
extern void	   skb_unlink(struct sk_buff *skb, struct sk_buff_head *list);
// 从给定的 sk_buff_head 类型的链表中移除指定的 sk_buff。
static inline void __skb_unlink(struct sk_buff *skb, struct sk_buff_head *list)
{
	struct sk_buff *next, *prev;

	list->qlen--;  // 减少列表长度
	next	   = skb->next;  // 指向要移除的 skb 的下一个元素
	prev	   = skb->prev;  // 指向要移除的 skb 的前一个元素
	skb->next  = skb->prev = NULL;  // 将要移除的 skb 的前后指针置空
	next->prev = prev;  // 将下一个元素的前指针指向前一个元素
	prev->next = next;  // 将前一个元素的后指针指向下一个元素
}

/**
 *	__skb_dequeue - remove from the head of the queue
 *	@list: list to dequeue from
 *
 *	Remove the head of the list. This function does not take any locks
 *	so must be used with appropriate locks held only. The head item is
 *	returned or %NULL if the list is empty.
 */
/**
 *	__skb_dequeue - 从队列头部移除
 *	@list: 要出队的列表
 *
 *	移除列表的头部。此函数不获取锁，
 *	因此只能在持有适当锁的情况下使用。如果列表为空则返回 %NULL，
 *	否则返回头部元素。
 */
extern struct sk_buff *skb_dequeue(struct sk_buff_head *list);
// 用于从链表的头部移除 sk_buff。首先，它通过 skb_peek 检查头部元素但不移除，如果存在，则使用 __skb_unlink 从链表中移除。
static inline struct sk_buff *__skb_dequeue(struct sk_buff_head *list)
{
	struct sk_buff *skb = skb_peek(list);  // 查看列表头部的元素但不移除
	if (skb)
		__skb_unlink(skb, list);  // 如果头部元素存在，则使用 __skb_unlink 将其从列表中移除
	return skb;  // 返回移除的头部元素或 NULL（如果列表为空）
}

/**
 *	__skb_dequeue_tail - remove from the tail of the queue
 *	@list: list to dequeue from
 *
 *	Remove the tail of the list. This function does not take any locks
 *	so must be used with appropriate locks held only. The tail item is
 *	returned or %NULL if the list is empty.
 */
/**
 *	__skb_dequeue_tail - 从队列尾部移除
 *	@list: 要出队的列表
 *
 *	移除列表的尾部。此函数不获取锁，
 *	因此只能在持有适当锁的情况下使用。如果列表为空则返回 %NULL，
 *	否则返回尾部元素。
 */
extern struct sk_buff *skb_dequeue_tail(struct sk_buff_head *list);
static inline struct sk_buff *__skb_dequeue_tail(struct sk_buff_head *list)
{
	struct sk_buff *skb = skb_peek_tail(list);  // 查看列表尾部的元素但不移除
	if (skb)
		__skb_unlink(skb, list);  // 如果尾部元素存在，则使用 __skb_unlink 将其从列表中移除
	return skb;  // 返回移除的尾部元素或 NULL（如果列表为空）
}

/**
 *	skb_is_nonlinear - 检查 skb 是否为非线性
 *	@skb: 要检查的 skb
 *
 *	返回 skb 是否为非线性的结果。非线性 skb 是指数据跨越多个内存区域的 skb。
 */
// 非线性 sk_buff 指的是其数据不仅仅存储在头部缓冲区 (skb->data) 中，还可能跨越多个数据片（fragments）。如果 skb->data_len 大于0，意味着 skb 包含额外的数据片，因此是非线性的。
static inline int skb_is_nonlinear(const struct sk_buff *skb)
{
	return skb->data_len;  // 如果 data_len 大于 0，则 skb 为非线性
}

// 即大哥长度，线性长度。和skb_headroom()不一样，这个是只头部空间剩余长度。
// 计算 sk_buff 的头部长度，即存储在直接数据区（非分散/聚集段）的数据长度。
static inline unsigned int skb_headlen(const struct sk_buff *skb)
{
	/*
	 * skb->len 是data长度，包含所带分片长度
	 * skb->data_len 是paged data长度, 即分片数据的长度，也就是skb_shared_info中的长度
	 * skb_headlen skb->len - skb->data_len 是当前片（unpaged data）长度
	 */
	/*
	 * len: 线性区和分片区域的总长度
	 * data_len：分片区域frag page中的数据长度
	 * len-data_len: 当前协议层中的线性区长度
	 */
	// skb->len 是 skb 的总数据长度，而 skb->data_len 是分散/聚集段中的数据长度。头部长度是两者的差值。
	return skb->len - skb->data_len;  // 返回 skb 的头部长度，即总长度减去非头部数据的长度
}

// 大哥和小弟的总和，即线性数据长度和页面碎片的长度，不包括分片skb队列长度。
// 函数首先遍历所有的片段（分页数据），累加其大小。然后，将累加的片段长度与头部长度相加，得到 skb 的整体页面长度。
static inline int skb_pagelen(const struct sk_buff *skb)
{
	int i, len = 0;

	for (i = (int)skb_shinfo(skb)->nr_frags - 1; i >= 0; i--)
		len += skb_shinfo(skb)->frags[i].size;  // 累加所有分页片段的大小
	return len + skb_headlen(skb);  // 将累加的分页片段大小与头部大小相加得到总的页面长度
}

// 该函数用于设置指定索引 i 的片段描述符，包括关联的页面、偏移量和大小。同时更新 sk_buff 中的片段总数。
static inline void skb_fill_page_desc(struct sk_buff *skb, int i,
                                      struct page *page, int off, int size)
{
	skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

	frag->page         = page;        // 设置片段的页面指针
	frag->page_offset  = off;         // 设置片段的页面偏移量
	frag->size         = size;        // 设置片段的大小
	skb_shinfo(skb)->nr_frags = i + 1;  // 更新片段数量
}

extern void skb_add_rx_frag(struct sk_buff *skb, int i, struct page *page,
			    int off, int size);

#define SKB_PAGE_ASSERT(skb)    BUG_ON(skb_shinfo(skb)->nr_frags)    // 断言 skb 没有片段
#define SKB_FRAG_ASSERT(skb)    BUG_ON(skb_has_frags(skb))          // 断言 skb 没有任何片段
#define SKB_LINEAR_ASSERT(skb)  BUG_ON(skb_is_nonlinear(skb))       // 断言 skb 是线性的

#ifdef NET_SKBUFF_DATA_USES_OFFSET
// 来获取 sk_buff 的尾部指针
static inline unsigned char *skb_tail_pointer(const struct sk_buff *skb)
{
	return skb->head + skb->tail;  // 返回尾部指针，当数据使用偏移量时
}

// 重设 sk_buff 的尾部指针位置
static inline void skb_reset_tail_pointer(struct sk_buff *skb)
{
	skb->tail = skb->data - skb->head;  // 重设尾部指针为数据开始处的偏移量
}

// 设置 sk_buff 的尾部指针位置
static inline void skb_set_tail_pointer(struct sk_buff *skb, const int offset)
{
	skb_reset_tail_pointer(skb);  // 重设尾部指针
	skb->tail += offset;  // 将尾部指针向前移动 offset 字节
}
#else /* NET_SKBUFF_DATA_USES_OFFSET */
// 来获取 sk_buff 的尾部指针
static inline unsigned char *skb_tail_pointer(const struct sk_buff *skb)
{
	return skb->tail;  // 直接返回尾部指针，当不使用偏移量时
}

// 重设 sk_buff 的尾部指针位置
static inline void skb_reset_tail_pointer(struct sk_buff *skb)
{
	skb->tail = skb->data;  // 重设尾部指针为数据开始处
}

// 设置 sk_buff 的尾部指针位置
static inline void skb_set_tail_pointer(struct sk_buff *skb, const int offset)
{
	skb->tail = skb->data + offset;  // 设置尾部指针为数据开始处向前 offset 字节的位置
}

#endif /* NET_SKBUFF_DATA_USES_OFFSET */

/**
 * 如果该skb存在分片的frag page，那么不能使用
 * skb_put()： 向后扩大数据区空间。headroom空间不变，tailroom空间降低，skb->data指针不变，skb->tail指针下移；
 * skb_push()： 向前扩大数据区空间。headroom空间降低，tailroom空间不变。skb->tail指针不变，skb->data指针上移；
 * skb_pull()： 缩小数据区空间.headroom空间增大，tailroom空间不变，skb->data指针下移，skb->tail指针不变；
 * skb_reserve()： 数据区空间大小不变，headroom空间增大，tailroom空间降低，skb->data和skb->tail同时下移；
 * 可以使用pskb_pull()：对于带有frag page的分片skb来说，data指针往下移动，可能会导致线性区越界，
 * 因此需要判断是否线性区有足够的空间用来pull操作，如果空间不够，那么需要执行linearize，重构线性区，
 * 把一部分frags中的数据移动到线性区中来操作。
 */

/*
 *	Add data to an sk_buff
 */
/*
 *	向 sk_buff 添加数据
 */
// 向后扩大数据区空间。headroom空间不变，tailroom空间降低，skb->data指针不变，skb->tail指针下移；
extern unsigned char *skb_put(struct sk_buff *skb, unsigned int len);
// 函数首先获取尾部指针，然后确保 skb 是线性的（没有跨页的数据）。接着，扩展尾部指针和更新长度，最后返回新增数据区的起始地址。
static inline unsigned char *__skb_put(struct sk_buff *skb, unsigned int len)
{
	unsigned char *tmp = skb_tail_pointer(skb);  // 获取当前的尾部指针
	SKB_LINEAR_ASSERT(skb);  // 断言 skb 是线性的
	skb->tail += len;  // 移动尾部指针，扩展数据区
	skb->len  += len;  // 增加 skb 的总长度
	return tmp;  // 返回添加数据前的尾部地址
}

// 向前扩大数据区空间。headroom空间降低，tailroom空间不变。skb->tail指针不变，skb->data指针上移；
extern unsigned char *skb_push(struct sk_buff *skb, unsigned int len);
// 函数通过向后移动数据起始指针并更新长度来实现在数据包前部添加数据。
static inline unsigned char *__skb_push(struct sk_buff *skb, unsigned int len)
{
	skb->data -= len;  // 将数据指针向前移动，以便在前面添加数据
	skb->len  += len;  // 增加 skb 的总长度
	return skb->data;  // 返回新的数据起始地址
}

// 缩小数据区空间.headroom空间增大，tailroom空间不变，skb->data指针下移，skb->tail指针不变；
extern unsigned char *skb_pull(struct sk_buff *skb, unsigned int len);
// 通过增加数据起始指针和减少长度来实现。
static inline unsigned char *__skb_pull(struct sk_buff *skb, unsigned int len)
{
	skb->len -= len;  // 减少 skb 的总长度
	BUG_ON(skb->len < skb->data_len);  // 断言新长度不小于非线性部分的长度
	return skb->data += len;  // 移动数据起始指针，跳过前面的数据，并返回新的起始地址
}

extern unsigned char *__pskb_pull_tail(struct sk_buff *skb, int delta);

// 从 sk_buff 的开始处移除数据，如果必要会从尾部数据中拉取。
static inline unsigned char *__pskb_pull(struct sk_buff *skb, unsigned int len)
{
	if (len > skb_headlen(skb) &&  // 如果要移除的长度超过头部数据长度
	    !__pskb_pull_tail(skb, len - skb_headlen(skb)))  // 尝试从尾部拉取数据
		return NULL;  // 拉取失败，返回 NULL
	skb->len -= len;  // 更新 skb 的总长度
	return skb->data += len;  // 更新数据指针，跳过被移除的部分，并返回新的起始地址
}

// 尝试从 skb 的开始处移除指定长度的数据。
// 如果请求的长度超过 skb 的总长度，则返回 NULL，表示操作不可能执行；否则调用 __pskb_pull 来实际执行数据移除。
/*
 * 对于带有frag page的分片skb来说，data指针往下移动，可能会导致线性区越界，
 * 因此需要判断是否线性区有足够的空间用来pull操作，如果空间不够，那么需要执行linearize，
 * 重构线性区，把一部分frags中的数据移动到线性区中来操作。
 */
static inline unsigned char *pskb_pull(struct sk_buff *skb, unsigned int len)
{
	return unlikely(len > skb->len) ? NULL : __pskb_pull(skb, len);
}

// 检查是否可以从 skb 的开始处安全移除指定长度的数据，如果必要，尝试从尾部拉取数据以满足请求。
// 此函数首先检查数据是否完全位于 skb 的线性部分，如果不是，它会尝试从尾部调整数据以满足长度要求。
static inline int pskb_may_pull(struct sk_buff *skb, unsigned int len)
{
	if (likely(len <= skb_headlen(skb)))
		return 1;  // 如果请求的长度小于或等于 skb 的头部长度，立即返回成功
	if (unlikely(len > skb->len))
		return 0;  // 如果请求的长度大于 skb 的总长度，返回失败
	return __pskb_pull_tail(skb, len - skb_headlen(skb)) != NULL;  // 尝试从 skb 的尾部拉取需要的数据
}

/**
 *	skb_headroom - bytes at buffer head
 *	@skb: buffer to check
 *
 *	Return the number of bytes of free space at the head of an &sk_buff.
 */
/**
 *	skb_headroom - 检查缓冲区头部的空闲空间
 *	@skb: 要检查的缓冲区
 *
 *	返回 sk_buff 头部的空闲字节数。
 */
// 返回 skb 的头部剩余空间，即 skb->data 之前可以用于扩展或前置数据的字节数。
static inline unsigned int skb_headroom(const struct sk_buff *skb)
{
	return skb->data - skb->head;
}

/**
 *	skb_tailroom - bytes at buffer end
 *	@skb: buffer to check
 *
 *	Return the number of bytes of free space at the tail of an sk_buff
 */
/**
 *	skb_tailroom - 检查缓冲区尾部的空闲空间
 *	@skb: 要检查的缓冲区
 *
 *	返回 sk_buff 尾部的空闲字节数。
 */
// 返回 skb 的尾部剩余空间，即 skb->tail 之后可以用于追加数据的字节数。
// 如果 skb 是非线性的（即包含多个片段），则认为没有尾部空间可用。
static inline int skb_tailroom(const struct sk_buff *skb)
{
	return skb_is_nonlinear(skb) ? 0 : skb->end - skb->tail;
}

/**
 *	skb_reserve - adjust headroom
 *	@skb: buffer to alter
 *	@len: bytes to move
 *
 *	Increase the headroom of an empty &sk_buff by reducing the tail
 *	room. This is only allowed for an empty buffer.
 */
/**
 *	skb_reserve - 调整头部空间
 *	@skb: 要调整的缓冲区
 *	@len: 移动的字节数
 *
 *	通过减少尾部空间来增加一个空 sk_buff 的头部空间。这仅允许用于空缓冲区。
 */
// 在 sk_buff 的头部预留指定长度的空间，通常在填充数据之前调用。
// 这个操作通过向后移动 skb->data 和 skb->tail 来扩展头部空间，适用于尚未填充数据的空缓冲区。
/*
 * 数据区空间大小不变，headroom空间增大，tailroom空间降低，skb->data和skb->tail同时下移；
 * skb_reserve()只能用于空的SKB，通常会在分配SKB之后就调用该函数，此时data和tail指针还一同指向数据区的起始位置。
 * 例如，某个以太网设备驱动的接收函数，在分配SKB之后，向数据缓存区填充数据之前，
 * 会有这样的一条语句skb_reserve(skb, 2)，这是因为以太网头长度为14B，再加上2B就正好16字节边界对齐，
 * 所以大多数以太网设备都会在数据包之前保留2B。
 */
static inline void skb_reserve(struct sk_buff *skb, int len)
{
	skb->data += len;  // 移动数据指针，增加头部空间
	skb->tail += len;  // 相应移动尾部指针，保持数据区的一致性
}

#ifdef NET_SKBUFF_DATA_USES_OFFSET
// 获取 skb 的运输层头部的起始地址。
// 通过 skb->head 和 skb->transport_header 偏移量计算得到。
static inline unsigned char *skb_transport_header(const struct sk_buff *skb)
{
	return skb->head + skb->transport_header;	// 返回运输头部的实际地址
}

// 将 skb 的运输层头部重设为数据开始的位置。
// 在修改 skb->data 后调用，以确保运输头部的正确标记。
static inline void skb_reset_transport_header(struct sk_buff *skb)
{
	skb->transport_header = skb->data - skb->head;	// 重设运输头部的位置为当前数据开始处
}

// 设置 skb 的运输层头部到指定的偏移位置。
// 常用于精确控制数据包结构，特别是在处理复杂的协议栈时。
static inline void skb_set_transport_header(struct sk_buff *skb,
					    const int offset)
{
	skb_reset_transport_header(skb);	// 首先重设运输头部
	skb->transport_header += offset;	// 然后调整运输头部到指定偏移
}

// 获取 skb 的网络层头部的起始地址。
// 通过 skb->head 和 skb->network_header 偏移量计算得到，用于访问和处理网络层数据。
static inline unsigned char *skb_network_header(const struct sk_buff *skb)
{
	return skb->head + skb->network_header;	// 返回网络头部的实际地址
}

// 重置 skb 的网络头部指针到数据区的开始。
// 这个操作通常在修改 skb->data 指针后使用，以确保网络头部指针正确指向数据的起始位置。
static inline void skb_reset_network_header(struct sk_buff *skb)
{
	skb->network_header = skb->data - skb->head;  // 重设网络头部指针到当前数据开始处
}

// 设置 skb 的网络头部到指定的偏移位置。
// 用于精确控制网络头部的位置，例如在处理封装或解封装网络数据包时。
static inline void skb_set_network_header(struct sk_buff *skb, const int offset)
{
	skb_reset_network_header(skb);  // 先重置网络头部
	skb->network_header += offset;  // 再按照给定偏移量设置网络头部
}

// 获取 skb 的链路层头部的起始地址。
// 通过 skb->head 和 skb->mac_header 偏移量计算得到。
static inline unsigned char *skb_mac_header(const struct sk_buff *skb)
{
	return skb->head + skb->mac_header;	// 返回链路层头部的实际地址
}

// 检查是否已经设置了 skb 的 MAC 头部。
// 如果 skb->mac_header 不等于最大无符号整型值，说明 MAC 头部已被设置。
static inline int skb_mac_header_was_set(const struct sk_buff *skb)
{
	return skb->mac_header != ~0U;  // 检查链路层头部是否被设置
}

// 重置 skb 的 MAC 头部指针到数据区的开始。
// 这个操作通常在修改 skb->data 指针后使用，以确保 MAC 头部指针正确指向数据的起始位置。
static inline void skb_reset_mac_header(struct sk_buff *skb)
{
	skb->mac_header = skb->data - skb->head;	// 重设 MAC 头部指针到当前数据开始处
}

// 设置 skb 的 MAC 头部到指定的偏移位置。
// 用于精确控制 MAC 头部的位置，特别是在处理各种链路层封装时。
static inline void skb_set_mac_header(struct sk_buff *skb, const int offset)
{
	skb_reset_mac_header(skb);  // 先重置 MAC 头部
	skb->mac_header += offset;  // 再按照给定偏移量设置 MAC 头部
}

#else /* NET_SKBUFF_DATA_USES_OFFSET */

// 获取 skb 的传输层头部的起始地址。
// 在不使用偏移量的情况下，直接返回存储在 skb 结构中的传输层头部指针。
static inline unsigned char *skb_transport_header(const struct sk_buff *skb)
{
	return skb->transport_header;	// 返回传输层头部的直接指针
}

// 重置 skb 的传输层头部指针到数据区的开始。
// 这通常在数据包内容被修改后使用，以确保传输层头部指针正确对齐到数据起始位置。
static inline void skb_reset_transport_header(struct sk_buff *skb)
{
	skb->transport_header = skb->data;	// 将传输层头部重设为数据区的开始处
}

// 设置 skb 的传输层头部到数据区开始处的指定偏移。
// 允许精确控制传输层头部的位置，特别是在处理封装或解析复杂协议时。
static inline void skb_set_transport_header(struct sk_buff *skb,
					    const int offset)
{
	skb->transport_header = skb->data + offset;	// 设置传输层头部到指定的偏移位置
}

// 获取 skb 的网络层头部的起始地址。
static inline unsigned char *skb_network_header(const struct sk_buff *skb)
{
	return skb->network_header;	// 返回网络层头部的直接指针
}

// 重置 skb 的网络层头部指针到数据区的开始。
static inline void skb_reset_network_header(struct sk_buff *skb)
{
	skb->network_header = skb->data;	// 将网络层头部重设为数据区的开始处
}

// 设置 skb 的网络层头部到数据区开始处的指定偏移。
static inline void skb_set_network_header(struct sk_buff *skb, const int offset)
{
	skb->network_header = skb->data + offset;	// 设置网络层头部到指定的偏移位置
}

// 获取 skb 的链路层头部的起始地址。
static inline unsigned char *skb_mac_header(const struct sk_buff *skb)
{
	return skb->mac_header;	// 返回链路层头部的直接指针
}

// 检查 skb 的 MAC 头部是否已经被设置。
static inline int skb_mac_header_was_set(const struct sk_buff *skb)
{
	return skb->mac_header != NULL;	// 检查链路层头部是否已经被设置
}

// 重置 skb 的 MAC 头部指针到数据区的开始。
// 这通常在调整数据包内容后使用，确保 MAC 头部指针正确指向数据起始位置。
static inline void skb_reset_mac_header(struct sk_buff *skb)
{
	skb->mac_header = skb->data;	// 将 MAC 头部重设为数据区的开始处
}

// 设置 skb 的 MAC 头部到数据区开始处的指定偏移位置。
// 允许精确控制 MAC 头部的位置，特别是在处理封装或解析复杂协议时。
static inline void skb_set_mac_header(struct sk_buff *skb, const int offset)
{
	skb->mac_header = skb->data + offset;	// 设置 MAC 头部到数据区开始处的指定偏移
}
#endif /* NET_SKBUFF_DATA_USES_OFFSET */

// 计算 skb 的传输层头部相对于数据部分的偏移量。
// 这有助于识别传输层数据的位置，重要于协议解析和数据处理。
static inline int skb_transport_offset(const struct sk_buff *skb)
{
	return skb_transport_header(skb) - skb->data;	// 返回传输层头部相对于数据开始处的偏移
}

// 返回 skb 的网络头部到传输头部之间的长度。
// 这可以用于确定网络层头部的实际长度，有助于处理各种网络协议。
static inline u32 skb_network_header_len(const struct sk_buff *skb)
{
	return skb->transport_header - skb->network_header;	// 计算网络头部到传输头部之间的长度
}

// 计算 skb 的网络层头部相对于数据部分的偏移量。
// 这有助于识别网络层数据的位置，对于协议解析和数据处理至关重要。
static inline int skb_network_offset(const struct sk_buff *skb)
{
	return skb_network_header(skb) - skb->data;	// 返回网络层头部相对于数据开始处的偏移
}

/*
 * CPUs often take a performance hit when accessing unaligned memory
 * locations. The actual performance hit varies, it can be small if the
 * hardware handles it or large if we have to take an exception and fix it
 * in software.
 *
 * Since an ethernet header is 14 bytes network drivers often end up with
 * the IP header at an unaligned offset. The IP header can be aligned by
 * shifting the start of the packet by 2 bytes. Drivers should do this
 * with:
 *
 * skb_reserve(skb, NET_IP_ALIGN);
 *
 * The downside to this alignment of the IP header is that the DMA is now
 * unaligned. On some architectures the cost of an unaligned DMA is high
 * and this cost outweighs the gains made by aligning the IP header.
 *
 * Since this trade off varies between architectures, we allow NET_IP_ALIGN
 * to be overridden.
 */
/*
 * CPU在访问未对齐的内存位置时常常会遭受性能损失。
 * 实际的性能损失程度不同，如果硬件处理这一问题则可能很小，
 * 如果需要通过异常处理并在软件中修正，则损失可能很大。
 *
 * 由于以太网头部是14字节，网络驱动程序通常会使得IP头部处于未对齐的偏移。
 * 通过将数据包起始位置向后移动2字节，可以对齐IP头部。驱动程序应通过以下方式实现：
 *
 * skb_reserve(skb, NET_IP_ALIGN);
 *
 * 尽管这样对齐IP头部的做法有其缺点，即现在DMA（直接内存访问）变得未对齐。
 * 在某些架构上，未对齐的DMA的成本很高，这种成本可能抵消了通过对齐IP头部所带来的收益。
 *
 * 由于这种权衡在不同架构间可能有所不同，我们允许覆盖NET_IP_ALIGN的值。
 */
/*
 * 性能考虑：访问未对齐的内存地址可能会导致性能下降。对于网络数据处理，尤其是IP数据包处理，性能是一个关键考量。
 * 
 * 以太网和IP头部对齐：以太网头部通常为14字节，这可能导致紧随其后的IP头部在内存中未对齐。
 * 通过调整（通常向后偏移2字节），可以实现IP头部的对齐，从而提高处理速度。
 * 
 * 对齐的权衡：对IP头部进行对齐虽然可以提高处理速度，但可能会导致DMA操作的未对齐，而在某些硬件架构中，
 * 未对齐的DMA操作成本较高。
 * 
 * 可配置的对齐值：由于不同硬件架构的特性差异，NET_IP_ALIGN 的值可以根据具体情况进行调整，以找到最佳的性能权衡点。
 */
#ifndef NET_IP_ALIGN
#define NET_IP_ALIGN    2  /* 如果未定义NET_IP_ALIGN，则默认定义为2 */
#endif

/*
 * The networking layer reserves some headroom in skb data (via
 * dev_alloc_skb). This is used to avoid having to reallocate skb data when
 * the header has to grow. In the default case, if the header has to grow
 * 32 bytes or less we avoid the reallocation.
 *
 * Unfortunately this headroom changes the DMA alignment of the resulting
 * network packet. As for NET_IP_ALIGN, this unaligned DMA is expensive
 * on some architectures. An architecture can override this value,
 * perhaps setting it to a cacheline in size (since that will maintain
 * cacheline alignment of the DMA). It must be a power of 2.
 *
 * Various parts of the networking layer expect at least 32 bytes of
 * headroom, you should not reduce this.
 */
/*
 * 网络层在 skb 数据中预留了一些头部空间（通过 dev_alloc_skb 实现）。
 * 这是为了避免在头部需要增长时重新分配 skb 数据。在默认情况下，如果头部
 * 需要增长的字节数为32字节或更少，我们可以避免重新分配。
 *
 * 不幸的是，这种头部空间改变了结果网络包的 DMA 对齐。就像 NET_IP_ALIGN 一样，
 * 在某些架构上，未对齐的 DMA 是昂贵的。架构可以覆盖这个值，
 * 也许将其设置为一个缓存行的大小（因为这将保持 DMA 的缓存行对齐）。
 * 它必须是2的幂。
 *
 * 网络层的各个部分至少期望有32字节的头部空间，你不应该减少这个数值。
 */
/*
 * 头部空间（Headroom）：这是在网络数据包的数据缓冲区的开始部分预留的空间，用于在不重新分配整个数据包的情况下增加额外的头部信息。
 * 
 * 避免重新分配：通过预留头部空间，当需要添加或扩展头部信息时，可以直接使用已预留的空间，
 * 从而避免了成本较高的内存重新分配操作。
 * 
 * DMA 对齐问题：虽然预留头部空间可以提高效率，但它可能会导致数据包的 DMA（直接内存访问）对齐问题，
 * 这在某些硬件架构上可能导致显著的性能下降。
 * 
 * 架构特定的优化：不同的硬件架构可以根据其特性来调整预留的头部空间大小，以优化性能和对齐。
 * 例如，设置为与缓存行大小相同可以保持缓存行对齐，从而提高缓存效率。
 * 
 * 头部空间的最小期望：网络层代码通常期望至少有32字节的头部空间，因此在进行任何更改时应保持至少这么多的预留空间。
 */
#ifndef NET_SKB_PAD
#define NET_SKB_PAD	32  /* 如果未定义NET_SKB_PAD，则默认定义为32 */
#endif

extern int ___pskb_trim(struct sk_buff *skb, unsigned int len);

// 直接调整 skb 的长度到指定值，但仅当 skb 不包含分散数据片段时适用。
// 这个函数首先检查 skb 是否包含数据片段（skb->data_len 不为0），如果是，会发出警告并返回，因为该函数不处理非线性 skb。
static inline void __skb_trim(struct sk_buff *skb, unsigned int len)
{
	if (unlikely(skb->data_len)) {
		WARN_ON(1);  // 如果 skb 包含分散的数据片段，则发出警告
		return;
	}
	skb->len = len;  // 设置 skb 的总长度为新的长度
	skb_set_tail_pointer(skb, len);  // 调整尾部指针以匹配新长度
}

extern void skb_trim(struct sk_buff *skb, unsigned int len);

// 根据 skb 是否包含数据片段来选择适当的函数处理长度调整。
// 如果 skb 是非线性的（包含数据片段），则调用更复杂的 ___pskb_trim 函数来处理；否则使用简单的 __skb_trim。
static inline int __pskb_trim(struct sk_buff *skb, unsigned int len)
{
	if (skb->data_len)
		return ___pskb_trim(skb, len);  // 如果 skb 包含数据片段，则调用底层函数处理
	__skb_trim(skb, len);  // 否则使用 __skb_trim 函数直接调整长度
	return 0;
}

// 调整 skb 的长度到不大于指定的 len。
// 这个函数检查新长度是否小于当前长度，如果是，才调用 __pskb_trim 进行调整，避免不必要的操作。
static inline int pskb_trim(struct sk_buff *skb, unsigned int len)
{
	return (len < skb->len) ? __pskb_trim(skb, len) : 0;	// 只在新长度小于当前长度时调整
}

/**
 *	pskb_trim_unique - remove end from a paged unique (not cloned) buffer
 *	@skb: buffer to alter
 *	@len: new length
 *
 *	This is identical to pskb_trim except that the caller knows that
 *	the skb is not cloned so we should never get an error due to out-
 *	of-memory.
 */
/**
 *	pskb_trim_unique - 从一个页化的唯一（未克隆）缓冲区中移除尾部
 *	@skb: 要修改的缓冲区
 *	@len: 新的长度
 *
 *	这个函数与 pskb_trim 相同，但调用者知道 skb 没有被克隆，
 *	所以我们不会因为内存不足而出现错误。
 */
static inline void pskb_trim_unique(struct sk_buff *skb, unsigned int len)
{
	int err = pskb_trim(skb, len);  // 调用 pskb_trim 来裁剪 skb
	BUG_ON(err);  // 断言没有错误发生，如果有错误则产生内核错误
}

/**
 *	skb_orphan - orphan a buffer
 *	@skb: buffer to orphan
 *
 *	If a buffer currently has an owner then we call the owner's
 *	destructor function and make the @skb unowned. The buffer continues
 *	to exist but is no longer charged to its former owner.
 */
/**
 *	skb_orphan - 孤立一个缓冲区
 *	@skb: 要孤立的缓冲区
 *
 *	如果一个缓冲区当前有拥有者，我们则调用拥有者的析构函数，
 *	并使 @skb 变为无主。缓冲区继续存在但不再计入其前任拥有者。
 */
static inline void skb_orphan(struct sk_buff *skb)
{
	if (skb->destructor)  // 如果有设置析构函数
		skb->destructor(skb);  // 调用析构函数
	skb->destructor = NULL;  // 清除析构函数指针
	skb->sk = NULL;  // 清除与 socket 的关联
}

/**
 *	__skb_queue_purge - empty a list
 *	@list: list to empty
 *
 *	Delete all buffers on an &sk_buff list. Each buffer is removed from
 *	the list and one reference dropped. This function does not take the
 *	list lock and the caller must hold the relevant locks to use it.
 */
/**
 *	__skb_queue_purge - 清空列表
 *	@list: 要清空的列表
 *
 *	删除列表上的所有缓冲区。每个缓冲区从列表中移除并且减少一个引用。
 *	此函数不获取列表锁，调用者必须持有相关锁来使用它。
 */
extern void skb_queue_purge(struct sk_buff_head *list);
static inline void __skb_queue_purge(struct sk_buff_head *list)
{
	struct sk_buff *skb;
	while ((skb = __skb_dequeue(list)) != NULL)  // 循环从列表中取出缓冲区
		kfree_skb(skb);  // 释放每个 sk_buff
}

/**
 *	__dev_alloc_skb - allocate an skbuff for receiving
 *	@length: length to allocate
 *	@gfp_mask: get_free_pages mask, passed to alloc_skb
 *
 *	Allocate a new &sk_buff and assign it a usage count of one. The
 *	buffer has unspecified headroom built in. Users should allocate
 *	the headroom they think they need without accounting for the
 *	built in space. The built in space is used for optimisations.
 *
 *	%NULL is returned if there is no free memory.
 */
/**
 *	__dev_alloc_skb - 分配一个用于接收的 skbuff
 *	@length: 要分配的长度
 *	@gfp_mask: get_free_pages 掩码，传递给 alloc_skb
 *
 *	分配一个新的 &sk_buff 并将其使用计数设置为1。缓冲区内建了未指定的头部空间。
 *	用户应该根据他们认为需要的头部空间来分配，而不用考虑内建的空间。
 *	内建的空间用于优化。
 *
 *	如果没有可用内存，返回 %NULL。
 */
static inline struct sk_buff *__dev_alloc_skb(unsigned int length,
					      gfp_t gfp_mask)
{
	struct sk_buff *skb = alloc_skb(length + NET_SKB_PAD, gfp_mask);  // 分配 skb，并额外增加 NET_SKB_PAD 空间
	if (likely(skb))
		skb_reserve(skb, NET_SKB_PAD);  // 在 skb 中预留 NET_SKB_PAD 头部空间
	return skb;  // 返回分配的 skb
}

extern struct sk_buff *dev_alloc_skb(unsigned int length);

extern struct sk_buff *__netdev_alloc_skb(struct net_device *dev,
		unsigned int length, gfp_t gfp_mask);

/**
 *	netdev_alloc_skb - allocate an skbuff for rx on a specific device
 *	@dev: network device to receive on
 *	@length: length to allocate
 *
 *	Allocate a new &sk_buff and assign it a usage count of one. The
 *	buffer has unspecified headroom built in. Users should allocate
 *	the headroom they think they need without accounting for the
 *	built in space. The built in space is used for optimisations.
 *
 *	%NULL is returned if there is no free memory. Although this function
 *	allocates memory it can be called from an interrupt.
 */
/**
 *	netdev_alloc_skb - 在特定设备上为接收操作分配 skbuff
 *	@dev: 接收数据的网络设备
 *	@length: 要分配的长度
 *
 *	分配一个新的 &sk_buff 并将其使用计数设置为1。缓冲区内建了未指定的头部空间。
 *	用户应该根据他们认为需要的头部空间来分配，而不用考虑内建的空间。
 *	内建的空间用于优化。
 *
 *	如果没有可用内存，返回 %NULL。尽管此函数分配内存，它可以从中断中调用。
 */
static inline struct sk_buff *netdev_alloc_skb(struct net_device *dev,
		unsigned int length)
{
	return __netdev_alloc_skb(dev, length, GFP_ATOMIC);  // 调用 __netdev_alloc_skb 分配 skb，使用 GFP_ATOMIC 标志以允许在中断上下文中调用
}

// 在特定的网络设备上分配一个 sk_buff，同时确保 IP 头部对齐。
// 首先调用 netdev_alloc_skb 分配一个 sk_buff，额外添加 NET_IP_ALIGN 的长度以确保有足够的空间进行 IP 头部对齐。如果分配成功并且 NET_IP_ALIGN 为非零，该函数将在 skb 的头部预留必要的空间以确保 IP 头部对齐。
static inline struct sk_buff *netdev_alloc_skb_ip_align(struct net_device *dev,
		unsigned int length)
{
	// ？？为什么没有预留以太网头部？？？不预留以太网头部为什么要对齐？？？？以太网头部14字节+2字节对齐才合理阿
	struct sk_buff *skb = netdev_alloc_skb(dev, length + NET_IP_ALIGN);	// 分配额外空间以允许 IP 头部对齐

	if (NET_IP_ALIGN && skb)
		skb_reserve(skb, NET_IP_ALIGN);	// 在 skb 的头部预留 NET_IP_ALIGN 空间以确保 IP 头部对齐
	return skb;
}

extern struct page *__netdev_alloc_page(struct net_device *dev, gfp_t gfp_mask);

/**
 *	netdev_alloc_page - allocate a page for ps-rx on a specific device
 *	@dev: network device to receive on
 *
 * 	Allocate a new page node local to the specified device.
 *
 * 	%NULL is returned if there is no free memory.
 */
/**
 *	netdev_alloc_page - 为特定设备上的ps-rx分配一个页面
 *	@dev: 接收数据的网络设备
 *
 * 	为指定设备本地分配一个新页面。
 *
 * 	如果没有可用内存，返回 %NULL。
 */
static inline struct page *netdev_alloc_page(struct net_device *dev)
{
	return __netdev_alloc_page(dev, GFP_ATOMIC);  // 调用 __netdev_alloc_page 分配一个页面，使用 GFP_ATOMIC 标志以允许在中断上下文中调用
}

// 释放之前为网络设备分配的页面。
// 通常在数据已被处理完毕或不再需要时调用，释放资源。
static inline void netdev_free_page(struct net_device *dev, struct page *page)
{
	__free_page(page);	// 释放指定的页面
}

/**
 *	skb_clone_writable - is the header of a clone writable
 *	@skb: buffer to check
 *	@len: length up to which to write
 *
 *	Returns true if modifying the header part of the cloned buffer
 *	does not requires the data to be copied.
 */
/**
 *	skb_clone_writable - 检查克隆的缓冲区头部是否可写
 *	@skb: 要检查的缓冲区
 *	@len: 可写的长度上限
 *
 *	如果克隆的缓冲区的头部修改不需要复制数据，则返回真。
 */
static inline int skb_clone_writable(struct sk_buff *skb, unsigned int len)
{
	return !skb_header_cloned(skb) &&  // 检查头部是否未被克隆
				 // 如果 skb 的头部未被克隆（即原始 sk_buff 和克隆都指向相同的头部数据），
				 // 并且写入的长度不超过头部的实际长度，则可以直接修改头部。
	       skb_headroom(skb) + len <= skb->hdr_len;  // 检查是否有足够的头部空间进行写操作
}

// 确保 skb 有足够的头部空间，并且在数据被克隆时复制数据部分。
// 这个函数检查和调整 skb 的头部空间，以确保有足够的空间来添加数据或进行其他操作。如果 skb 被克隆了，它会尝试重新分配一个新的 skb 以避免对共享数据的修改。
static inline int __skb_cow(struct sk_buff *skb, unsigned int headroom,
			    int cloned)
{
	int delta = 0;

	if (headroom < NET_SKB_PAD)  // 如果给定的头部空间小于最小值
		headroom = NET_SKB_PAD;  // 设置为最小值
	if (headroom > skb_headroom(skb))  // 如果需要的头部空间大于当前头部空间
		delta = headroom - skb_headroom(skb);  // 计算额外需要的头部空间

	if (delta || cloned)  // 如果需要更多头部空间或skb已经被克隆
		return pskb_expand_head(skb, ALIGN(delta, NET_SKB_PAD), 0,
					GFP_ATOMIC);  // 扩展头部空间并进行对齐
	return 0;  // 如果不需要调整，直接返回0
}

/**
 *	skb_cow - copy header of skb when it is required
 *	@skb: buffer to cow
 *	@headroom: needed headroom
 *
 *	If the skb passed lacks sufficient headroom or its data part
 *	is shared, data is reallocated. If reallocation fails, an error
 *	is returned and original skb is not changed.
 *
 *	The result is skb with writable area skb->head...skb->tail
 *	and at least @headroom of space at head.
 */
/**
 *	skb_cow - 在必要时复制 skb 的头部
 *	@skb: 要复制的缓冲区
 *	@headroom: 需要的头部空间
 *
 *	如果传递的 skb 缺乏足够的头部空间或其数据部分被共享，
 *	数据会被重新分配。如果重新分配失败，返回错误并且原始 skb 不会改变。
 *
 *	结果是一个具有可写区域 skb->head...skb->tail 
 *	并且在头部至少有 @headroom 空间的 skb。
 */
// 处理 sk_buff 的写时复制逻辑，确保 skb 在需要时具有足够的修改空间，并且数据不是共享的。
// 这个函数首先检查 skb 是否已被克隆。如果是，或者如果需要更多的头部空间，它会调用 __skb_cow 来尝试扩展 skb 或分配新的缓冲区。
static inline int skb_cow(struct sk_buff *skb, unsigned int headroom)
{
	return __skb_cow(skb, headroom, skb_cloned(skb));  // 调用 __skb_cow 处理写时复制逻辑
}

/**
 *	skb_cow_head - skb_cow but only making the head writable
 *	@skb: buffer to cow
 *	@headroom: needed headroom
 *
 *	This function is identical to skb_cow except that we replace the
 *	skb_cloned check by skb_header_cloned.  It should be used when
 *	you only need to push on some header and do not need to modify
 *	the data.
 */
/**
 *	skb_cow_head - skb_cow 的变种，只使头部可写
 *	@skb: 需要处理的缓冲区
 *	@headroom: 需要的头部空间
 *
 *	此函数与 skb_cow 相同，除了它通过 skb_header_cloned 检查替换了 skb_cloned 检查。
 *	应当在只需要推送一些头部而不需要修改数据时使用。
 */
// 确保 skb 的头部有足够的空间并且可修改，而不关心数据部分是否被克隆。
// 这个函数特别用于当你只需要修改 skb 的协议头部时。它使用 skb_header_cloned 来检查头部是否克隆，而不是整个 skb。
static inline int skb_cow_head(struct sk_buff *skb, unsigned int headroom)
{
	return __skb_cow(skb, headroom, skb_header_cloned(skb));  // 处理写时复制逻辑，检查头部是否被克隆
}

/**
 *	skb_padto	- pad an skbuff up to a minimal size
 *	@skb: buffer to pad
 *	@len: minimal length
 *
 *	Pads up a buffer to ensure the trailing bytes exist and are
 *	blanked. If the buffer already contains sufficient data it
 *	is untouched. Otherwise it is extended. Returns zero on
 *	success. The skb is freed on error.
 */
/**
 *	skb_padto - 填充 skbuff 以达到最小长度
 *	@skb: 要填充的缓冲区
 *	@len: 最小长度
 *
 *	填充缓冲区以确保尾部字节存在并且被清空。如果缓冲区已经包含足够的数据，则不做处理。
 *	否则，它将被扩展。成功时返回零。在出错时释放 skb。
 */
// 确保 sk_buff 至少达到指定的长度 len，如果不够则进行填充。
// 如果 skb 的当前长度小于 len，则会调用 skb_pad 来增加长度。如果填充失败，则会释放 skb 并返回错误。
static inline int skb_padto(struct sk_buff *skb, unsigned int len)
{
	unsigned int size = skb->len;  // 当前 skb 的长度
	if (likely(size >= len))
		return 0;  // 如果当前长度已经满足需求，则直接返回 0
	return skb_pad(skb, len - size);  // 调用 skb_pad 来增加 skb 长度，以满足最小长度要求
}

// 向 sk_buff 添加用户空间的数据，并可选地计算校验和。
// 此函数首先检查是否需要计算校验和，如果需要，则同时复制数据并计算校验和；如果不需要校验和，只复制数据。如果复制失败，会恢复 skb 到原始长度。
static inline int skb_add_data(struct sk_buff *skb,
			       char __user *from, int copy)
{
	const int off = skb->len;  // 在添加数据前记录当前skb的长度

	if (skb->ip_summed == CHECKSUM_NONE) {
		int err = 0;
		// 从用户空间复制数据到skb，并计算数据的校验和
		__wsum csum = csum_and_copy_from_user(from, skb_put(skb, copy),
							    copy, 0, &err);
		if (!err) {  // 如果复制没有错误
			// 添加新数据的校验和到现有的校验和中
			skb->csum = csum_block_add(skb->csum, csum, off);
			return 0;  // 返回成功
		}
	} else if (!copy_from_user(skb_put(skb, copy), from, copy))  // 如果不需要计算校验和，只需要复制数据
		return 0;  // 如果复制成功，返回0

	__skb_trim(skb, off);  // 如果复制失败，将skb裁剪回原始长度
	return -EFAULT;  // 返回错误
}

// 检查是否可以将新的数据片段合并到 sk_buff 的最后一个数据片段。
// 检查最后一个数据片段的页面和偏移量，以决定新的数据片段是否可以直接追加到现有的数据片段上，而不需要新的分配。
static inline int skb_can_coalesce(struct sk_buff *skb, int i,
				   struct page *page, int off)
{
	if (i) {  // 如果 i 不为0，表示存在至少一个前置片段
		struct skb_frag_struct *frag = &skb_shinfo(skb)->frags[i - 1];  // 获取最后一个数据片段

		return page == frag->page &&  // 检查当前页是否与最后一个片段在同一页面
		       off == frag->page_offset + frag->size;  // 检查偏移是否正确接在最后一个片段之后
	}
	return 0;  // 如果 i 为0，表示没有前置片段可合并，返回0
}

// 将 sk_buff 的数据部分线性化，即如果数据是分散的，则尝试将其合并成一个连续的内存区域。
// 这个函数用于处理那些非线性的 skb，即数据分散存储在多个内存片段中。函数尝试将所有分散的数据拉到一个连续的内存区域中。如果操作成功，返回 0；如果因为内存不足等原因失败，返回 -ENOMEM。
static inline int __skb_linearize(struct sk_buff *skb)
{
	return __pskb_pull_tail(skb, skb->data_len) ? 0 : -ENOMEM;  // 尝试将 skb 的所有数据片段合并到单一连续的内存区域
}

/**
 *	skb_linearize - convert paged skb to linear one
 *	@skb: buffer to linarize
 *
 *	If there is no free memory -ENOMEM is returned, otherwise zero
 *	is returned and the old skb data released.
 */
/**
 *	skb_linearize - 将分页的 skb 转换为线性的
 *	@skb: 要线性化的缓冲区
 *
 *	如果没有可用的内存，则返回 -ENOMEM；否则返回零，并释放旧的 skb 数据。
 */
// 将 sk_buff 转换为一个线性的内存布局，如果它当前是非线性的（即包含多个内存片段）。
static inline int skb_linearize(struct sk_buff *skb)
{
	return skb_is_nonlinear(skb) ? __skb_linearize(skb) : 0;  // 如果 skb 是非线性的，尝试线性化
}

/**
 *	skb_linearize_cow - make sure skb is linear and writable
 *	@skb: buffer to process
 *
 *	If there is no free memory -ENOMEM is returned, otherwise zero
 *	is returned and the old skb data released.
 */
/**
 *	skb_linearize_cow - 确保 skb 是线性的且可写
 *	@skb: 要处理的缓冲区
 *
 *	如果没有可用的内存，则返回 -ENOMEM；否则返回零，并释放旧的 skb 数据。
 */
// 确保 sk_buff 是线性的，并且缓冲区是可写的，适用于需要修改数据内容的场景。
static inline int skb_linearize_cow(struct sk_buff *skb)
{
	return skb_is_nonlinear(skb) || skb_cloned(skb) ?  // 如果 skb 是非线性的或已被克隆
	       __skb_linearize(skb) : 0;  // 尝试线性化
}

/**
 *	skb_postpull_rcsum - update checksum for received skb after pull
 *	@skb: buffer to update
 *	@start: start of data before pull
 *	@len: length of data pulled
 *
 *	After doing a pull on a received packet, you need to call this to
 *	update the CHECKSUM_COMPLETE checksum, or set ip_summed to
 *	CHECKSUM_NONE so that it can be recomputed from scratch.
 */
/**
 *	skb_postpull_rcsum - 在拉取后更新接收到的 skb 的校验和
 *	@skb: 要更新的缓冲区
 *	@start: 拉取前数据的开始位置
 *	@len: 拉取的数据长度
 *
 *	在对接收到的数据包执行拉取操作后，你需要调用这个函数来更新 CHECKSUM_COMPLETE 校验和，
 *	或将 ip_summed 设置为 CHECKSUM_NONE 以便从头开始重新计算校验和。
 */
// 在 skb 上执行 pull 操作（即移除一些数据头部）后，正确更新或重置校验和。
static inline void skb_postpull_rcsum(struct sk_buff *skb,
				      const void *start, unsigned int len)
{
	if (skb->ip_summed == CHECKSUM_COMPLETE)  // 如果 skb 使用的是完整的校验和模式
		skb->csum = csum_sub(skb->csum, csum_partial(start, len, 0));  // 从校验和中减去被拉取的数据部分的校验和
}

unsigned char *skb_pull_rcsum(struct sk_buff *skb, unsigned int len);

/**
 *	pskb_trim_rcsum - trim received skb and update checksum
 *	@skb: buffer to trim
 *	@len: new length
 *
 *	This is exactly the same as pskb_trim except that it ensures the
 *	checksum of received packets are still valid after the operation.
 */

/**
 *	pskb_trim_rcsum - 裁剪接收到的 skb 并更新校验和
 *	@skb: 要裁剪的缓冲区
 *	@len: 新长度
 *
 *	这个函数与 pskb_trim 完全相同，除了它确保在操作后接收包的校验和仍然有效。
 */
static inline int pskb_trim_rcsum(struct sk_buff *skb, unsigned int len)
{
	if (likely(len >= skb->len))
		return 0;  // 如果新长度大于等于当前长度，不需要裁剪，直接返回0
	if (skb->ip_summed == CHECKSUM_COMPLETE)
		skb->ip_summed = CHECKSUM_NONE;  // 如果校验和状态是完整的，设置为 NONE 以便重新计算
	return __pskb_trim(skb, len);  // 调用 __pskb_trim 进行实际的裁剪
}

// 遍历 sk_buff_head 链表中的所有 sk_buff。
// 使用预取优化来提高链表遍历的性能。
#define skb_queue_walk(queue, skb) \
		for (skb = (queue)->next;					\
		     prefetch(skb->next), (skb != (struct sk_buff *)(queue));	\
		     skb = skb->next)

// 安全地遍历 sk_buff_head 链表，允许在遍历过程中修改链表。
// 在迭代中使用临时变量 tmp 保存下一个节点，确保即使当前节点被修改或删除，遍历仍然安全。
#define skb_queue_walk_safe(queue, skb, tmp)					\
		for (skb = (queue)->next, tmp = skb->next;			\
		     skb != (struct sk_buff *)(queue);				\
		     skb = tmp, tmp = skb->next)

// 从给定的 skb 开始遍历 sk_buff_head 链表。
// 用于从链表中某个特定点开始遍历，不从头开始。
#define skb_queue_walk_from(queue, skb)						\
		for (; prefetch(skb->next), (skb != (struct sk_buff *)(queue));	\
		     skb = skb->next)

// 安全地从给定的 skb 开始遍历 sk_buff_head 链表。
// 与 skb_queue_walk_safe 类似，但是从指定的 skb 开始，而不是从头部开始。
#define skb_queue_walk_from_safe(queue, skb, tmp)				\
		for (tmp = skb->next;						\
		     skb != (struct sk_buff *)(queue);				\
		     skb = tmp, tmp = skb->next)

// 反向遍历 sk_buff_head 链表。
// 从链表的尾部向前遍历，适用于需要反向处理 sk_buff 的场景。
#define skb_queue_reverse_walk(queue, skb) \
		for (skb = (queue)->prev;					\
		     prefetch(skb->prev), (skb != (struct sk_buff *)(queue));	\
		     skb = skb->prev)

// 检查一个 sk_buff 是否包含数据片段。
// 这个函数通过检查 skb 的片段列表（frag_list）是否为空来确定 skb 是否包含片段。如果列表不为空，表示 skb 包含一个或多个数据片段。
static inline bool skb_has_frags(const struct sk_buff *skb)
{
	return skb_shinfo(skb)->frag_list != NULL;	// 检查 skb 是否包含片段列表
}

// 初始化 sk_buff 的片段列表。
// 这个函数将 skb 的片段列表设置为空，这通常用于在 skb 创建时或在重置 skb 状态时调用。
static inline void skb_frag_list_init(struct sk_buff *skb)
{
	skb_shinfo(skb)->frag_list = NULL;
}

// 在 sk_buff 的片段列表头部添加一个新片段。
// 这个函数将一个新的 skb 片段添加到现有片段列表的头部。它首先将新片段的 next 指针指向当前的列表头部，然后更新列表头部为这个新片段。这是一个典型的链表头插法操作。
static inline void skb_frag_add_head(struct sk_buff *skb, struct sk_buff *frag)
{
	frag->next = skb_shinfo(skb)->frag_list;	// 将新片段的 next 指针指向当前的片段列表头
	skb_shinfo(skb)->frag_list = frag;	// 更新片段列表头为新片段
}

// 遍历 skb 的所有片段。
// 这个宏初始化 iter 为 skb 的第一个片段，并迭代访问每个片段直到片段列表结束。
#define skb_walk_frags(skb, iter)	\
	for (iter = skb_shinfo(skb)->frag_list; iter; iter = iter->next)

// 从套接字缓冲区中接收一个数据报。
// sk - 套接字结构，flags - 操作标志，peeked - 指向是否为偷看操作的标记，err - 错误码输出。
extern struct sk_buff *__skb_recv_datagram(struct sock *sk, unsigned flags,
					   int *peeked, int *err);
// ：从套接字缓冲区中接收一个数据报，可指定非阻塞操作。
// sk - 套接字结构，flags - 操作标志，noblock - 是否为非阻塞操作，err - 错误码输出。
extern struct sk_buff *skb_recv_datagram(struct sock *sk, unsigned flags,
					 int noblock, int *err);
// 轮询数据报套接字的状态。
// file - 文件结构指针，sock - 套接字结构，wait - 轮询表结构。
extern unsigned int    datagram_poll(struct file *file, struct socket *sock,
				     struct poll_table_struct *wait);
// 这些函数用于从 skb 到用户空间 (iovec) 的数据复制，支持校验和计算和数据从 iovec 到 skb 的反向复制。
// from, skb - 源 sk_buff 结构，offset - 数据开始偏移，to/iov - 目标 iovec 结构，hlen - 头部长度（用于校验和计算），size, len - 复制的数据大小。
extern int	       skb_copy_datagram_iovec(const struct sk_buff *from,
					       int offset, struct iovec *to,
					       int size);
extern int	       skb_copy_and_csum_datagram_iovec(struct sk_buff *skb,
							int hlen,
							struct iovec *iov);
extern int	       skb_copy_datagram_from_iovec(struct sk_buff *skb,
						    int offset,
						    const struct iovec *from,
						    int from_offset,
						    int len);
extern int	       skb_copy_datagram_const_iovec(const struct sk_buff *from,
						     int offset,
						     const struct iovec *to,
						     int to_offset,
						     int size);
// 用于通常情况下释放数据报。
extern void	       skb_free_datagram(struct sock *sk, struct sk_buff *skb);
// 用于已经锁定套接字的情况。
extern void	       skb_free_datagram_locked(struct sock *sk,
						struct sk_buff *skb);
// 彻底销毁一个数据报，通常用于错误处理或数据报不再需要时。
extern int	       skb_kill_datagram(struct sock *sk, struct sk_buff *skb,
					 unsigned int flags);
// 计算 skb 中指定部分的校验和。
// 从指定的 offset 开始，计算长度为 len 的数据的校验和，初始校验和值为 csum。
extern __wsum	       skb_checksum(const struct sk_buff *skb, int offset,
				    int len, __wsum csum);
// 从 skb 复制数据到指定缓冲区。
extern int	       skb_copy_bits(const struct sk_buff *skb, int offset,
				     void *to, int len);
// 将数据从指定缓冲区复制到 skb。
extern int	       skb_store_bits(struct sk_buff *skb, int offset,
				      const void *from, int len);
//  在复制数据的同时计算校验和。
extern __wsum	       skb_copy_and_csum_bits(const struct sk_buff *skb,
					      int offset, u8 *to, int len,
					      __wsum csum);
// 将 skb 中的数据“拼接”到一个管道中。
extern int             skb_splice_bits(struct sk_buff *skb,
						unsigned int offset,
						struct pipe_inode_info *pipe,
						unsigned int len,
						unsigned int flags);
// 用于将 skb 的数据复制到设备（通常是网络设备）并计算校验和。
extern void	       skb_copy_and_csum_dev(const struct sk_buff *skb, u8 *to);
// 将一个 skb 在指定长度处分割成两个部分。
extern void	       skb_split(struct sk_buff *skb,
				 struct sk_buff *skb1, const u32 len);
// 将数据从一个 skb 转移到另一个 skb。
extern int	       skb_shift(struct sk_buff *tgt, struct sk_buff *skb,
				 int shiftlen);

// 根据特定的传输特性将 skb 分段，常用于处理 TCP 分段。
extern struct sk_buff *skb_segment(struct sk_buff *skb, int features);

// 从 sk_buff 获取指向数据头的指针，如果数据头不在线性区域内，则将数据复制到提供的缓冲区。
// 这个函数用于安全地访问 skb 中的数据，特别是当数据可能不完全在线性区域时。它首先检查数据是否可以直接访问，如果不可以，则尝试复制到外部缓冲区。
static inline void *skb_header_pointer(const struct sk_buff *skb, int offset,
				       int len, void *buffer)
{
	int hlen = skb_headlen(skb);  // 获取 skb 中线性部分的长度

	if (hlen - offset >= len)  // 检查偏移后的长度是否足够
		return skb->data + offset;  // 如果足够，直接返回数据指针

	if (skb_copy_bits(skb, offset, buffer, len) < 0)  // 尝试将数据从 skb 复制到 buffer
		return NULL;  // 如果复制失败，返回 NULL

	return buffer;  // 如果复制成功，返回 buffer 的指针
}

// 从 sk_buff 的线性数据区复制数据到指定的内存地址。
// 这个函数用于直接从 skb 的数据区复制指定长度的数据到另一个内存位置。此函数假设数据已经是线性的，不涉及任何非线性检查或处理。
static inline void skb_copy_from_linear_data(const struct sk_buff *skb,
					     void *to,
					     const unsigned int len)
{
	memcpy(to, skb->data, len);	// 将 skb 的数据复制到指定的目标内存位置
}

// 从 sk_buff 的线性数据区的指定偏移处开始复制指定长度的数据到外部内存。
// 这个函数在复制数据时考虑了偏移量，适用于从数据包中的特定位置开始提取数据。
static inline void skb_copy_from_linear_data_offset(const struct sk_buff *skb,
						    const int offset, void *to,
						    const unsigned int len)
{
	memcpy(to, skb->data + offset, len);	// 从 skb 的线性数据区指定偏移处开始复制数据到指定目标
}

// 将外部数据复制到 sk_buff 的线性数据区。
// 这个函数用于向 skb 的开始处复制数据，通常用于填充或修改数据包的内容。
static inline void skb_copy_to_linear_data(struct sk_buff *skb,
					   const void *from,
					   const unsigned int len)
{
	memcpy(skb->data, from, len);	// 将外部数据复制到 skb 的线性数据区
}

// 将外部数据复制到 sk_buff 的线性数据区的指定偏移处。
// 这个函数在复制数据时考虑了偏移量，适用于在数据包的特定位置插入或修改数据。
static inline void skb_copy_to_linear_data_offset(struct sk_buff *skb,
						  const int offset,
						  const void *from,
						  const unsigned int len)
{
	memcpy(skb->data + offset, from, len);	// 将外部数据复制到 skb 的线性数据区指定偏移处
}

// 初始化 sk_buff 相关的资源或结构（skb_buff 的高速缓存）。
extern void skb_init(void);

// 获取 sk_buff 的时间戳。
// 这个函数用于获取数据包的接收或创建时间，通常用于性能分析、日志记录或时间敏感的处理。
static inline ktime_t skb_get_ktime(const struct sk_buff *skb)
{
	return skb->tstamp;	// 返回 skb 的时间戳
}

/**
 *	skb_get_timestamp - get timestamp from a skb
 *	@skb: skb to get stamp from
 *	@stamp: pointer to struct timeval to store stamp in
 *
 *	Timestamps are stored in the skb as offsets to a base timestamp.
 *	This function converts the offset back to a struct timeval and stores
 *	it in stamp.
 */
/**
 *	skb_get_timestamp - 从 skb 获取时间戳
 *	@skb: 要获取时间戳的 skb
 *	@stamp: 指向 struct timeval 的指针，用于存储时间戳
 *
 *	时间戳在 skb 中以基准时间戳的偏移量形式存储。
 *	此函数将偏移量转换回 struct timeval 并将其存储在 stamp 中。
 */
// 从 sk_buff 中获取时间戳并转换为 struct timeval 格式。
// 这个函数使用 ktime_to_timeval 将 skb 中的 ktime_t 类型时间戳转换为更常用的 timeval 结构。
static inline void skb_get_timestamp(const struct sk_buff *skb,
				     struct timeval *stamp)
{
	*stamp = ktime_to_timeval(skb->tstamp);	// 将 skb 的时间戳转换为 timeval 结构并存储
}

// 从 sk_buff 中获取时间戳并转换为 struct timespec 格式。
// 类似于 skb_get_timestamp，但这里转换的目标格式是 timespec，提供了纳秒级的时间精度。
static inline void skb_get_timestampns(const struct sk_buff *skb,
				       struct timespec *stamp)
{
	*stamp = ktime_to_timespec(skb->tstamp);	// 将 skb 的时间戳转换为 timespec 结构并存储
}

// 为 sk_buff 设置当前的实时时间戳。
// 此函数获取当前时间（以 ktime_t 格式），并将其设置为 skb 的时间戳，通常用于数据包接收或发送时标记时间。
static inline void __net_timestamp(struct sk_buff *skb)
{
	skb->tstamp = ktime_get_real();	// 获取当前实时时间并设置为 skb 的时间戳
}

// 计算从给定的 ktime_t 时间点到当前时间的差值。
// 这个函数用于测量从某一时间点到现在所经过的时间，常用于性能测量和超时判断。
static inline ktime_t net_timedelta(ktime_t t)
{
	return ktime_sub(ktime_get_real(), t);	// 计算从时间 t 到当前时间的时间差
}

// 生成一个无效的（值为零）时间戳。
// 这个函数返回一个表示无效或未设置状态的时间戳，常用于初始化或错误处理。
static inline ktime_t net_invalid_timestamp(void)
{
	return ktime_set(0, 0);	// 返回一个无效的（即时间为0）的时间戳
}

/**
 * skb_tstamp_tx - queue clone of skb with send time stamps
 * @orig_skb:	the original outgoing packet
 * @hwtstamps:	hardware time stamps, may be NULL if not available
 *
 * If the skb has a socket associated, then this function clones the
 * skb (thus sharing the actual data and optional structures), stores
 * the optional hardware time stamping information (if non NULL) or
 * generates a software time stamp (otherwise), then queues the clone
 * to the error queue of the socket.  Errors are silently ignored.
 */
/**
 * skb_tstamp_tx - 将带有发送时间戳的 skb 克隆到队列中
 * @orig_skb: 原始的传出数据包
 * @hwtstamps: 硬件时间戳，如果不可用则可能为 NULL
 *
 * 如果 skb 有关联的套接字，此函数克隆 skb（从而共享实际数据和可选结构），存储硬件时间戳信息（如果非 NULL）
 * 或生成软件时间戳（否则），然后将克隆的 skb 加入到套接字的错误队列中。错误被默默地忽略。
 */
extern void skb_tstamp_tx(struct sk_buff *orig_skb,
			struct skb_shared_hwtstamps *hwtstamps);

// 用于计算 skb 的头部直到给定长度的校验和。
extern __sum16 __skb_checksum_complete_head(struct sk_buff *skb, int len);
// 计算整个 skb 的校验和。
extern __sum16 __skb_checksum_complete(struct sk_buff *skb);

// 检查 sk_buff 是否不需要校验和计算。
// 此函数检查 skb 的 ip_summed 字段是否包含 CHECKSUM_UNNECESSARY 标志，该标志表明已经验证了校验和或校验和不需要计算。
static inline int skb_csum_unnecessary(const struct sk_buff *skb)
{
	return skb->ip_summed & CHECKSUM_UNNECESSARY;	// 检查 skb 是否标记为不需要校验和计算
}

/**
 *	skb_checksum_complete - Calculate checksum of an entire packet
 *	@skb: packet to process
 *
 *	This function calculates the checksum over the entire packet plus
 *	the value of skb->csum.  The latter can be used to supply the
 *	checksum of a pseudo header as used by TCP/UDP.  It returns the
 *	checksum.
 *
 *	For protocols that contain complete checksums such as ICMP/TCP/UDP,
 *	this function can be used to verify that checksum on received
 *	packets.  In that case the function should return zero if the
 *	checksum is correct.  In particular, this function will return zero
 *	if skb->ip_summed is CHECKSUM_UNNECESSARY which indicates that the
 *	hardware has already verified the correctness of the checksum.
 */
/**
 * skb_checksum_complete - 计算整个数据包的校验和
 * @skb: 要处理的数据包
 *
 * 该函数计算整个数据包加上 skb->csum 的校验和。后者可以用来提供 TCP/UDP 伪头部的校验和。
 * 它返回计算出的校验和。
 *
 * 对于包含完整校验和的协议（如 ICMP、TCP、UDP），此函数可以用来验证接收数据包的校验和。
 * 在这种情况下，如果校验和正确，函数应返回零。特别是，如果 skb->ip_summed 是 CHECKSUM_UNNECESSARY，
 * 表示硬件已经验证了校验和的正确性，此函数将返回零。
 */
static inline __sum16 skb_checksum_complete(struct sk_buff *skb)
{
	// 如果不需要计算校验和或校验和已验证，则返回 0，否则调用 __skb_checksum_complete 计算校验和
	return skb_csum_unnecessary(skb) ?
	       0 : __skb_checksum_complete(skb);
}

// 这些函数仅在启用了 NF_CONNTRACK 或 NF_CONNTRACK_MODULE 时编译。
#if defined(CONFIG_NF_CONNTRACK) || defined(CONFIG_NF_CONNTRACK_MODULE)
// 销毁一个网络连接追踪对象。
// 参数：nfct - 指向要销毁的连接追踪对象的指针。
extern void nf_conntrack_destroy(struct nf_conntrack *nfct);
// 减少连接追踪对象的引用计数，并在无引用时销毁该对象。
// 此函数检查 nfct 是否存在，然后原子地减少其引用计数 (use)。如果引用计数为0，调用 nf_conntrack_destroy 销毁对象。
static inline void nf_conntrack_put(struct nf_conntrack *nfct)
{
	if (nfct && atomic_dec_and_test(&nfct->use))
		nf_conntrack_destroy(nfct);	// // 如果引用计数减到0，则销毁连接追踪对象
}
// 增加连接追踪对象的引用计数。
// 如果 nfct 非空，此函数将原子地增加其引用计数。
static inline void nf_conntrack_get(struct nf_conntrack *nfct)
{
	if (nfct)
		atomic_inc(&nfct->use);	// 增加连接追踪对象的引用计数
}
// 增加 sk_buff 的引用计数。
// 用于网络数据包 skb 的重新组装处理时保持引用计数，确保数据包在处理过程中不被释放。
static inline void nf_conntrack_get_reasm(struct sk_buff *skb)
{
	if (skb)
		atomic_inc(&skb->users);	// 增加 skb 的引用计数
}
// 释放 sk_buff。
// 当 skb 不再需要时，调用此函数来释放它。适用于处理完重新组装的数据包后。
static inline void nf_conntrack_put_reasm(struct sk_buff *skb)
{
	if (skb)
		kfree_skb(skb);	// 释放 skb
}
#endif
#ifdef CONFIG_BRIDGE_NETFILTER
// 减少桥接信息结构的引用计数，如果引用计数为0，则释放它。
static inline void nf_bridge_put(struct nf_bridge_info *nf_bridge)
{
	if (nf_bridge && atomic_dec_and_test(&nf_bridge->use))
		kfree(nf_bridge);	// 如果引用计数减至0，则释放桥接信息结构
}
// 增加桥接信息结构的引用计数。
static inline void nf_bridge_get(struct nf_bridge_info *nf_bridge)
{
	if (nf_bridge)
		atomic_inc(&nf_bridge->use);	// 增加桥接信息结构的引用计数
}
#endif /* CONFIG_BRIDGE_NETFILTER */
/**
 * 重置 sk_buff 中与网络过滤相关的信息，释放与之关联的各种结构。
 * @skb: 指向网络数据包的指针
 *
 * 此函数用于清除与 sk_buff 相关联的网络过滤器信息，释放相关资源，并将相关指针置为 NULL，
 * 防止野指针错误。此操作确保 skb 在重新使用前不会携带过时的网络状态信息。
 */
// 当 sk_buff 不再需要其当前网络状态信息，或在将 sk_buff 重新投入使用前，调用此函数可以确保所有的网络过滤器状态
// 都被清除，这对于防止资源泄漏和确保数据包处理逻辑的正确性至关重要。
// 特别在高性能网络路径中，及时释放无用的资源和重置状态是保持系统性能的关键。
static inline void nf_reset(struct sk_buff *skb)
{
// 在网络连接跟踪配置 (CONFIG_NF_CONNTRACK 或 CONFIG_NF_CONNTRACK_MODULE) 启用的情况下，释放与数据包关联的连接跟踪对象 (nfct) 和重新组装的连接跟踪对象 (nfct_reasm)。
#if defined(CONFIG_NF_CONNTRACK) || defined(CONFIG_NF_CONNTRACK_MODULE)
	nf_conntrack_put(skb->nfct);	// 释放网络连接跟踪对象
	skb->nfct = NULL;
	nf_conntrack_put_reasm(skb->nfct_reasm);	// 释放用于数据包重组的连接跟踪对象
	skb->nfct_reasm = NULL;
#endif
// 在桥接网络过滤配置 (CONFIG_BRIDGE_NETFILTER) 启用的情况下，释放与数据包关联的桥接信息 (nf_bridge)。
#ifdef CONFIG_BRIDGE_NETFILTER
	nf_bridge_put(skb->nf_bridge);	// 释放桥接过滤器信息对象
	skb->nf_bridge = NULL;
#endif
}

/* Note: This doesn't put any conntrack and bridge info in dst. */
/**
 * __nf_copy - 复制网络过滤器和连接跟踪信息
 * @dst: 目标数据包
 * @src: 源数据包
 *
 * 注意：此函数不会将任何连接跟踪和桥接信息复制到目的地数据包的目的地字段。
 */
// 当需要在处理网络数据包时创建一个新的 sk_buff 副本，同时需要保留与原始数据包相关的网络状态信息时，使用此函数可以确保网络状态的一致性和完整性。
// 这种复制特别在网络数据包复制、修改和再发送的情况下非常有用，例如在网络路由、负载均衡或防火墙处理中。
static inline void __nf_copy(struct sk_buff *dst, const struct sk_buff *src)
{
// 如果启用了网络连接跟踪功能（CONFIG_NF_CONNTRACK 或 CONFIG_NF_CONNTRACK_MODULE）
#if defined(CONFIG_NF_CONNTRACK) || defined(CONFIG_NF_CONNTRACK_MODULE)
	dst->nfct = src->nfct;                 // 复制源 skb 的连接跟踪对象引用
	nf_conntrack_get(src->nfct);           // 增加源连接跟踪对象的引用计数
	dst->nfctinfo = src->nfctinfo;         // 复制连接跟踪信息
	dst->nfct_reasm = src->nfct_reasm;     // 复制用于重组的连接跟踪对象
	nf_conntrack_get_reasm(src->nfct_reasm);  // 增加重组对象的引用计数
#endif
// 果启用了桥接网络过滤功能（CONFIG_BRIDGE_NETFILTER），函数还会从源 sk_buff 复制桥接信息对象到目标 sk_buff，并增加该对象的引用计数。
#ifdef CONFIG_BRIDGE_NETFILTER
	dst->nf_bridge = src->nf_bridge;       // 复制桥接信息对象
	nf_bridge_get(src->nf_bridge);         // 增加桥接信息对象的引用计数
#endif
}

/**
 * nf_copy - 复制并更新网络过滤器和连接跟踪信息
 * @dst: 目标数据包
 * @src: 源数据包
 *
 * 此函数首先释放目标 skb 中已存在的网络连接跟踪和桥接信息资源，然后从源 skb 复制这些信息。
 * 这确保了在数据包处理过程中资源的正确管理，防止内存泄漏。
 */
// 在需要克隆或复制网络数据包并保留其网络状态信息的网络处理任务中，如在网络路由、负载均衡、防火墙或其他网络监控和管理系统中使用。
// 特别适用于处理需要维护数据包状态一致性和完整性的高级网络功能。
static inline void nf_copy(struct sk_buff *dst, const struct sk_buff *src)
{
#if defined(CONFIG_NF_CONNTRACK) || defined(CONFIG_NF_CONNTRACK_MODULE)
	nf_conntrack_put(dst->nfct);              // 释放目标 skb 的连接跟踪对象
	nf_conntrack_put_reasm(dst->nfct_reasm);  // 释放目标 skb 的用于重组的连接跟踪对象
#endif
#ifdef CONFIG_BRIDGE_NETFILTER
	nf_bridge_put(dst->nf_bridge);	// 释放目标 skb 的桥接信息对象
#endif
	__nf_copy(dst, src);						// 调用 __nf_copy 函数来从源 skb 复制信息到目标 skb
}

#ifdef CONFIG_NETWORK_SECMARK
/**
 * skb_copy_secmark - 复制网络数据包的安全标记
 * @to: 目标数据包
 * @from: 源数据包
 *
 * 此函数用于将一个 skb（网络数据包）的 secmark（安全标记）从一个数据包复制到另一个数据包。
 * 这确保在处理或传递数据包时，安全标记的一致性得以保持。
 */
static inline void skb_copy_secmark(struct sk_buff *to, const struct sk_buff *from)
{
	to->secmark = from->secmark;	// 将源数据包的 secmark 值复制到目标数据包
}

/**
 * skb_init_secmark - 初始化网络数据包的安全标记
 * @skb: 要初始化的数据包
 *
 * 将数据包的 secmark 设置为 0，用于新创建或需要重置 secmark 的数据包。
 */
static inline void skb_init_secmark(struct sk_buff *skb)
{
	skb->secmark = 0;	// 初始化 secmark 为 0
}
#else
/**
 * skb_copy_secmark - 复制网络数据包的安全标记（空操作）
 * @to: 目标数据包
 * @from: 源数据包
 *
 * 如果未启用 CONFIG_NETWORK_SECMARK 配置，则此函数不执行任何操作。
 */
static inline void skb_copy_secmark(struct sk_buff *to, const struct sk_buff *from)
{ }

/**
 * skb_init_secmark - 初始化网络数据包的安全标记（空操作）
 * @skb: 要初始化的数据包
 *
 * 如果未启用 CONFIG_NETWORK_SECMARK 配置，则此函数不执行任何操作。
 */
static inline void skb_init_secmark(struct sk_buff *skb)
{ }
#endif
/**
 * skb_set_queue_mapping - 设置 sk_buff 的队列映射
 * @skb: 指向网络数据包的指针
 * @queue_mapping: 队列映射值
 *
 * 此函数设置 skb 的 queue_mapping 字段，用于网络调度和处理。
 */
static inline void skb_set_queue_mapping(struct sk_buff *skb, u16 queue_mapping)
{
	skb->queue_mapping = queue_mapping;	// 设置队列映射值
}

/**
 * skb_get_queue_mapping - 获取 sk_buff 的队列映射值
 * @skb: 指向网络数据包的指针
 *
 * 返回 skb 的 queue_mapping 字段的值。
 */
static inline u16 skb_get_queue_mapping(const struct sk_buff *skb)
{
	return skb->queue_mapping;	// 返回队列映射值
}

/**
 * skb_copy_queue_mapping - 复制队列映射值从一个 sk_buff 到另一个
 * @to: 目标 skb
 * @from: 源 skb
 *
 * 将源 skb 的 queue_mapping 值复制到目标 skb。
 */
static inline void skb_copy_queue_mapping(struct sk_buff *to, const struct sk_buff *from)
{
	to->queue_mapping = from->queue_mapping;	// 复制队列映射值
}

/**
 * skb_record_rx_queue - 记录接收队列到 sk_buff
 * @skb: 指向网络数据包的指针
 * @rx_queue: 接收队列编号
 *
 * 将接收队列编号加1后记录到 skb 的 queue_mapping 字段。
 */
static inline void skb_record_rx_queue(struct sk_buff *skb, u16 rx_queue)
{
	skb->queue_mapping = rx_queue + 1;	// 记录接收队列编号（加1以避免0值）
}

/**
 * skb_get_rx_queue - 获取 sk_buff 的接收队列编号
 * @skb: 指向网络数据包的指针
 *
 * 返回 skb 中记录的接收队列编号，减1后得到原始值。
 */
static inline u16 skb_get_rx_queue(const struct sk_buff *skb)
{
	return skb->queue_mapping - 1;	// 获取接收队列编号（减1以还原）
}

/**
 * skb_rx_queue_recorded - 检查是否记录了接收队列
 * @skb: 指向网络数据包的指针
 *
 * 检查 skb 的 queue_mapping 是否非0，非0表示已记录接收队列。
 */
static inline bool skb_rx_queue_recorded(const struct sk_buff *skb)
{
	return (skb->queue_mapping != 0);	// 检查是否记录了接收队列
}

/**
 * skb_tx_hash - 计算 sk_buff 的传输散列
 * @dev: 网络设备
 * @skb: 指向网络数据包的指针
 *
 * 根据网络设备和 skb 计算用于传输选择的散列值。
 */
extern u16 skb_tx_hash(const struct net_device *dev,
		       const struct sk_buff *skb);

#ifdef CONFIG_XFRM
/**
 * skb_sec_path - 获取 sk_buff 的安全路径
 * @skb: 指向网络数据包的指针
 *
 * 返回 skb 中的安全路径。当 XFRM（IPsec变换框架）配置启用时，这个函数返回指向 skb 的安全路径结构的指针。
 */
static inline struct sec_path *skb_sec_path(struct sk_buff *skb)
{
	return skb->sp;
}
#else
/**
 * skb_sec_path - 获取 sk_buff 的安全路径（当 XFRM 未配置时）
 * @skb: 指向网络数据包的指针
 *
 * 当 CONFIG_XFRM 未定义时，此函数总是返回 NULL，表示没有安全路径可用。
 */
static inline struct sec_path *skb_sec_path(struct sk_buff *skb)
{
	return NULL;
}
#endif

/**
 * skb_is_gso - 检查 sk_buff 是否是用于 GSO（Generic Segmentation Offload）的
 * @skb: 指向网络数据包的指针
 *
 * 返回 skb 的 GSO 分段大小，如果 GSO 分段大小大于0，表示 skb 是用于 GSO 的。
 */
static inline int skb_is_gso(const struct sk_buff *skb)
{
	return skb_shinfo(skb)->gso_size;
}

/**
 * skb_is_gso_v6 - 检查 sk_buff 是否是用于 IPv6 GSO 的
 * @skb: 指向网络数据包的指针
 *
 * 检查 skb 的 GSO 类型是否包括 SKB_GSO_TCPV6。如果是，表示 skb 是用于 IPv6 的 TCP 分段卸载。
 */
static inline int skb_is_gso_v6(const struct sk_buff *skb)
{
	return skb_shinfo(skb)->gso_type & SKB_GSO_TCPV6;
}

/**
 * __skb_warn_lro_forwarding - 记录关于 LRO（Large Receive Offload）转发的警告
 * @skb: 指向网络数据包的指针
 *
 * 用于在处理大数据包接收卸载时，如果存在转发问题，记录警告。
 */
extern void __skb_warn_lro_forwarding(const struct sk_buff *skb);

/**
 * skb_warn_if_lro - 检查 skb 是否是用于 LRO 的，并发出警告
 * @skb: 指向网络数据包的指针
 *
 * LRO 会设置 gso_size 但不设置 gso_type，而如果真正需要 GSO，则会设置 gso_type。
 * 如果 gso_size 非零且 gso_type 为零，则调用 __skb_warn_lro_forwarding 记录警告，并返回 true。
 * 否则返回 false。
 */
static inline bool skb_warn_if_lro(const struct sk_buff *skb)
{
	/* LRO sets gso_size but not gso_type, whereas if GSO is really
	 * wanted then gso_type will be set. */
	/* LRO会设置gso_size但不设置gso_type，
	 * 而如果真正需要GSO则会设置gso_type。
	 */
	struct skb_shared_info *shinfo = skb_shinfo(skb);	// 获取skb的共享信息结构
	if (shinfo->gso_size != 0 && unlikely(shinfo->gso_type == 0)) {	// 如果gso_size非零且gso_type为零
		__skb_warn_lro_forwarding(skb);	// 发出LRO警告
		return true;	// 返回true表示发现错误配置
	}
	return false;		// 正常情况返回false
}

/**
 * skb_forward_csum - 处理 skb 的校验和
 * @skb: 指向网络数据包的指针
 *
 * 如果 skb 的 ip_summed 字段是 CHECKSUM_COMPLETE，则将其改为 CHECKSUM_NONE。
 * 此函数表示目前不支持将校验和完全处理转为其他状态，需要更多的开发。
 */
static inline void skb_forward_csum(struct sk_buff *skb)
{
	/* Unfortunately we don't support this one.  Any brave souls? */
	/* 不幸的是我们不支持这个功能。有勇气的人可以尝试支持吗？ */
	if (skb->ip_summed == CHECKSUM_COMPLETE)	// 如果校验和状态是完整的
		skb->ip_summed = CHECKSUM_NONE;	// 更改为无校验和状态
}

/**
 * skb_partial_csum_set - 设置部分校验和
 * @skb: 指向网络数据包的指针
 * @start: 校验和计算开始的位置
 * @off: 校验和放置的位置
 *
 * 此函数用于在 skb 中设置部分校验和的相关字段，以支持校验和的部分卸载。
 * 返回值为布尔类型，表示设置是否成功。
 */
bool skb_partial_csum_set(struct sk_buff *skb, u16 start, u16 off);
#endif	/* __KERNEL__ */
#endif	/* _LINUX_SKBUFF_H */
