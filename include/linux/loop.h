#ifndef _LINUX_LOOP_H
#define _LINUX_LOOP_H

/*
 * include/linux/loop.h
 *
 * Written by Theodore Ts'o, 3/29/93.
 *
 * Copyright 1993 by Theodore Ts'o.  Redistribution of this file is
 * permitted under the GNU General Public License.
 */

// 定义循环设备文件名和密钥名称的最大长度
#define LO_NAME_SIZE	64
#define LO_KEY_SIZE	32

#ifdef __KERNEL__
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

/* Possible states of device */
/* 可能的设备状态 */
enum {
	Lo_unbound,	// 未绑定状态
	Lo_bound,		// 已绑定状态
	Lo_rundown,	// 正在注销状态
};

struct loop_func_table;

// 循环设备(loop device)的内核头文件，主要定义了循环设备的数据结构和相关操作。
// 循环设备允许文件被当作块设备，通常用于实现文件系统镜像的挂载等操作。
struct loop_device {
	int		lo_number;	// 循环设备的编号
	int		lo_refcnt;	// 引用计数
	loff_t		lo_offset;	// 数据偏移量
	loff_t		lo_sizelimit;	// 大小限制
	int		lo_flags;	// 标志位
	// 转换函数指针，用于加密解密等操作
	int		(*transfer)(struct loop_device *, int cmd,
				    struct page *raw_page, unsigned raw_off,
				    struct page *loop_page, unsigned loop_off,
				    int size, sector_t real_block);
	// 文件名
	char		lo_file_name[LO_NAME_SIZE];
	// 加密名
	char		lo_crypt_name[LO_NAME_SIZE];
	// 加密密钥
	char		lo_encrypt_key[LO_KEY_SIZE];
	// 密钥大小
	int		lo_encrypt_key_size;
	// 加密函数表
	struct loop_func_table *lo_encryption;
	// 初始化数据
	__u32           lo_init[2];
	// 密钥的所有者
	uid_t		lo_key_owner;	/* Who set the key */
	// IOCTL命令处理函数
	int		(*ioctl)(struct loop_device *, int cmd, 
				 unsigned long arg); 

	struct file *	lo_backing_file;	// 对应的文件指针
	struct block_device *lo_device;	// 对应的块设备指针
	unsigned	lo_blocksize;	// 块大小
	void		*key_data; 			// 密钥数据

	gfp_t		old_gfp_mask;	// 旧的GFP掩码

	spinlock_t		lo_lock;	// 保护结构的自旋锁
	struct bio_list		lo_bio_list;	// BIO列表
	int			lo_state;	// 设备的状态
	struct mutex		lo_ctl_mutex;	// 控制互斥锁
	struct task_struct	*lo_thread;	// 对应的线程
	wait_queue_head_t	lo_event;	// 等待队列头

	struct request_queue	*lo_queue;	// 请求队列
	struct gendisk		*lo_disk;	// 对应的gendisk结构
	struct list_head	lo_list;	// 设备列表
};

#endif /* __KERNEL__ */

/*
 * Loop flags
 */
/*
 * 循环设备标志
 */
enum {
	LO_FLAGS_READ_ONLY	= 1,	// 只读标志
	LO_FLAGS_USE_AOPS	= 2,		// 使用地址操作符
	LO_FLAGS_AUTOCLEAR	= 4,	// 自动清除标志
};

/* 包含posix类型定义，用于 __kernel_old_dev_t */
#include <asm/posix_types.h>	/* for __kernel_old_dev_t */
/* 包含Linux数据类型定义，用于 __u64 等 */
#include <linux/types.h>	/* for __u64 */

/* Backwards compatibility version */
/* 向后兼容的版本结构 */
struct loop_info {
	/* ioctl 只读 */
	int		   lo_number;		/* ioctl r/o */
	/* ioctl 只读 */
	__kernel_old_dev_t lo_device; 		/* ioctl r/o */
	/* ioctl 只读 */
	unsigned long	   lo_inode; 		/* ioctl r/o */
	/* ioctl 只读 */
	__kernel_old_dev_t lo_rdevice; 		/* ioctl r/o */
	// 偏移量
	int		   lo_offset;
	// 加密类型
	int		   lo_encrypt_type;
	// 加密键大小, ioctl 写入操作
	int		   lo_encrypt_key_size; 	/* ioctl w/o */
	/* ioctl 只读 */
	int		   lo_flags;			/* ioctl r/o */
	// 设备名
	char		   lo_name[LO_NAME_SIZE];
	/* ioctl 写入操作 */
	unsigned char	   lo_encrypt_key[LO_KEY_SIZE]; /* ioctl w/o */
	// 初始化参数
	unsigned long	   lo_init[2];
	// 保留位
	char		   reserved[4];
};

struct loop_info64 {
	__u64		   lo_device;			/* ioctl r/o */
	__u64		   lo_inode;			/* ioctl r/o */
	__u64		   lo_rdevice;			/* ioctl r/o */
	__u64		   lo_offset;	// 偏移量
	/* 限制大小，0 表示可用的最大值 */
	__u64		   lo_sizelimit;/* bytes, 0 == max available */
	__u32		   lo_number;			/* ioctl r/o */
	// 加密类型
	__u32		   lo_encrypt_type;
	/* ioctl 写入操作 */
	__u32		   lo_encrypt_key_size;		/* ioctl w/o */
	__u32		   lo_flags;			/* ioctl r/o */
	// 文件名
	__u8		   lo_file_name[LO_NAME_SIZE];
	// 加密名
	__u8		   lo_crypt_name[LO_NAME_SIZE];
	/* ioctl 写入操作 */
	__u8		   lo_encrypt_key[LO_KEY_SIZE]; /* ioctl w/o */
	// 初始化参数
	__u64		   lo_init[2];
};

/*
 * Loop filter types
 */
/*
 * 循环设备加密类型定义
 */

// 无加密
#define LO_CRYPT_NONE		0
// XOR加密
#define LO_CRYPT_XOR		1
// DES加密
#define LO_CRYPT_DES		2
// Twofish加密
#define LO_CRYPT_FISH2		3    /* Twofish encryption */
// Blowfish加密
#define LO_CRYPT_BLOW		4
// CAST128加密
#define LO_CRYPT_CAST128	5
// IDEA加密
#define LO_CRYPT_IDEA		6
// 虚拟加密（测试用）
#define LO_CRYPT_DUMMY		9
// Skipjack加密
#define LO_CRYPT_SKIPJACK	10
// 使用CryptoAPI的加密
#define LO_CRYPT_CRYPTOAPI	18
// 最大加密类型数量
#define MAX_LO_CRYPT		20

#ifdef __KERNEL__
/* Support for loadable transfer modules */
/* 支持可加载传输模块 */
struct loop_func_table {
	/* 加密类型 */
	int number;	/* filter type */ 
	int (*transfer)(struct loop_device *lo, int cmd,
			struct page *raw_page, unsigned raw_off,
			struct page *loop_page, unsigned loop_off,
			int size, sector_t real_block);
	int (*init)(struct loop_device *, const struct loop_info64 *); 
	/* release is called from loop_unregister_transfer or clr_fd */
	/* release函数从loop_unregister_transfer或clr_fd调用 */
	int (*release)(struct loop_device *); 
	int (*ioctl)(struct loop_device *, int cmd, unsigned long arg);
	struct module *owner;
}; 

int loop_register_transfer(struct loop_func_table *funcs);
int loop_unregister_transfer(int number); 

#endif
/*
 * IOCTL commands --- we will commandeer 0x4C ('L')
 */
/*
 * IOCTL命令 --- 我们使用0x4C ('L')作为开始
 */

#define LOOP_SET_FD		0x4C00  // 设置文件描述符
#define LOOP_CLR_FD		0x4C01  // 清除文件描述符
#define LOOP_SET_STATUS		0x4C02  // 设置状态
#define LOOP_GET_STATUS		0x4C03  // 获取状态
#define LOOP_SET_STATUS64	0x4C04  // 设置64位状态
#define LOOP_GET_STATUS64	0x4C05  // 获取64位状态
#define LOOP_CHANGE_FD		0x4C06  // 更改文件描述符
#define LOOP_SET_CAPACITY	0x4C07  // 设置容量

#endif
