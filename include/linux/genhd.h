#ifndef _LINUX_GENHD_H
#define _LINUX_GENHD_H

/*
 * 	genhd.h Copyright (C) 1992 Drew Eckhardt
 *	Generic hard disk header file by  
 * 		Drew Eckhardt
 *
 *		<drew@colorado.edu>
 */

#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/rcupdate.h>

#ifdef CONFIG_BLOCK

// 宏定义，用于从 kobject 获得包含它的 device 结构体
#define kobj_to_dev(k)		container_of((k), struct device, kobj)
// 从 device 结构体获取 gendisk 结构体
#define dev_to_disk(device)	container_of((device), struct gendisk, part0.__dev)
// 从 device 结构体获取分区结构 hd_struct
#define dev_to_part(device)	container_of((device), struct hd_struct, __dev)
// 从 gendisk 结构体获取 device 结构体
#define disk_to_dev(disk)	(&(disk)->part0.__dev)
// 从分区结构 hd_struct 获取 device 结构体
#define part_to_dev(part)	(&((part)->__dev))

// 对外部声明
extern struct device_type part_type;  // 分区的设备类型
extern struct kobject *block_depr;    // 块设备的弃用 kobject
extern struct class block_class;      // 块设备的类

// 分区类型枚举
enum {
/* These three have identical behaviour; use the second one if DOS FDISK gets
   confused about extended/logical partitions starting past cylinder 1023. */
	/* 这三个具有相同行为；如果 DOS FDISK 在柱面 1023 之后对扩展/逻辑分区感到困惑，请使用第二个 */
	DOS_EXTENDED_PARTITION = 5,
	LINUX_EXTENDED_PARTITION = 0x85,
	WIN98_EXTENDED_PARTITION = 0x0f,

	SUN_WHOLE_DISK = DOS_EXTENDED_PARTITION,

	LINUX_SWAP_PARTITION = 0x82,    // Linux 交换分区
	LINUX_DATA_PARTITION = 0x83,    // Linux 数据分区
	LINUX_LVM_PARTITION = 0x8e,     // Linux LVM 分区
	/* autodetect RAID partition */
	LINUX_RAID_PARTITION = 0xfd,    // 自动检测 RAID 分区

	SOLARIS_X86_PARTITION =	LINUX_SWAP_PARTITION, // Solaris 分区（使用 Linux 交换分区 ID）
	NEW_SOLARIS_X86_PARTITION = 0xbf,              // 新 Solaris 分区

	// DM6 辅助分区1，没有动态磁盘覆盖 (DDO): 使用转换的几何参数
	DM6_AUX1PARTITION = 0x51,	/* no DDO:  use xlated geom */
	// DM6 辅助分区3，同上
	DM6_AUX3PARTITION = 0x53,	/* no DDO:  use xlated geom */
	// DM6 分区，有动态磁盘覆盖: 使用转换的几何参数和偏移
	DM6_PARTITION =	0x54,		/* has DDO: use xlated geom & offset */
	// EZ-Drive
	EZD_PARTITION =	0x55,		/* EZ-DRIVE */

	// FreeBSD 分区 ID
	FREEBSD_PARTITION = 0xa5,	/* FreeBSD Partition ID */
	// OpenBSD 分区 ID
	OPENBSD_PARTITION = 0xa6,	/* OpenBSD Partition ID */
	// NetBSD 分区 ID
	NETBSD_PARTITION = 0xa9,	/* NetBSD Partition ID */
	// BSDI 分区 ID
	BSDI_PARTITION = 0xb7,		/* BSDI Partition ID */
	// Minix 分区 ID
	MINIX_PARTITION = 0x81,		/* Minix Partition ID */
	// UnixWare 分区 ID，与 GNU HURD 和 SCO Unix 相同
	UNIXWARE_PARTITION = 0x63,	/* Same as GNU_HURD and SCO Unix */
};

// 最大分区数量
#define DISK_MAX_PARTS			256
// 磁盘名称长度
#define DISK_NAME_LEN			32

#include <linux/major.h>
#include <linux/device.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/workqueue.h>

// 描述了一个硬盘分区的详细信息，包括起始扇区、扇区数量等，同时包含对该分区的 I/O 统计信息，是磁盘驱动中的关键数据结构。
/**
 * struct partition - 描述硬盘分区表项的结构体
 * @boot_ind: 活动标记（0x80 表示活动）
 * @head: 起始磁头
 * @sector: 起始扇区
 * @cyl: 起始柱面
 * @sys_ind: 分区类型
 * @end_head: 结束磁头
 * @end_sector: 结束扇区
 * @end_cyl: 结束柱面
 * @start_sect: 分区开始的扇区号（从0开始计数）
 * @nr_sects: 分区中的扇区数
 */
struct partition {
	/* 0x80 - 活动 */
	unsigned char boot_ind;		/* 0x80 - active */
	/* 起始磁头 */
	unsigned char head;		/* starting head */
	/* 起始扇区 */
	unsigned char sector;		/* starting sector */
	/* 起始柱面 */
	unsigned char cyl;		/* starting cylinder */
	/* 分区类型 */
	unsigned char sys_ind;		/* What partition type */
	/* 结束磁头 */
	unsigned char end_head;		/* end head */
	/* 结束扇区 */
	unsigned char end_sector;	/* end sector */
	/* 结束柱面 */
	unsigned char end_cyl;		/* end cylinder */
	/* 从 0 开始的起始扇区 */
	__le32 start_sect;	/* starting sector counting from 0 */
	/* 分区内的扇区数 */
	__le32 nr_sects;		/* nr of sectors in partition */
} __attribute__((packed));	// 使用 packed 属性确保编译器不会改变这些字段的布局

// 提供了磁盘操作的详细统计信息，可以用于监控和优化磁盘性能。
/**
 * struct disk_stats - 描述磁盘I/O统计信息的结构体
 * @sectors: 读写扇区数
 * @ios: 读写操作数
 * @merges: 合并前的读写操作数
 * @ticks: 读写操作花费的时间
 * @io_ticks: 磁盘活跃的总时间
 * @time_in_queue: 请求在队列中的总时间
 */
struct disk_stats {
		/* READs and WRITEs */
	unsigned long sectors[2];	/* 读和写扇区数 */
	unsigned long ios[2];		/* 读和写操作数 */
	unsigned long merges[2];	/* 读和写合并数 */
	unsigned long ticks[2];		/* 读和写操作时间 */
	unsigned long io_ticks;		/* I/O 操作的总时间 */
	unsigned long time_in_queue;	/* 请求在队列的总时间 */
};

/**
 * struct hd_struct - 描述硬盘分区的结构体
 * @start_sect: 分区的起始扇区
 * @nr_sects: 分区的扇区数
 * @alignment_offset: 对齐偏移
 * @discard_alignment: 废弃对齐
 * @__dev: 设备
 * @holder_dir: 持有者目录
 * @policy: 策略
 * @partno: 分区号
 * @stamp: 时间戳
 * @in_flight: 正在处理的读写请求数
 * @dkstats: 磁盘统计信息
 * @rcu_head: RCU 头，用于无锁编程
 */
struct hd_struct {
	sector_t start_sect;		/* 分区起始扇区 */
	sector_t nr_sects;		/* 分区扇区数 */
	sector_t alignment_offset;	/* 对齐偏移 */
	unsigned int discard_alignment;	/* 废弃对齐 */
	struct device __dev;		/* 设备实体 */
	struct kobject *holder_dir;	/* 持有者目录 */
	int policy, partno;		/* 策略和分区号 */
#ifdef CONFIG_FAIL_MAKE_REQUEST
	int make_it_fail;		/* 测试时用于强制失败 */
#endif
	unsigned long stamp;		/* 时间戳 */
	int in_flight[2];		/* 进行中的读写请求数 */
#ifdef CONFIG_SMP
	struct disk_stats __percpu *dkstats;	/* 每 CPU 的磁盘统计数据 */
#else
	struct disk_stats dkstats;	/* 磁盘统计数据 */
#endif
	struct rcu_head rcu_head;	/* RCU 头部 */
};

// 硬盘设备标志
#define GENHD_FL_REMOVABLE			1	// 可移动介质
/* 2 is unused */
#define GENHD_FL_MEDIA_CHANGE_NOTIFY		4	// 媒体更改通知
#define GENHD_FL_CD				8		// CD设备
#define GENHD_FL_UP				16	// 设备已启动（通电）
#define GENHD_FL_SUPPRESS_PARTITION_INFO	32	// 抑制分区信息
// 允许扩展的设备编号
#define GENHD_FL_EXT_DEVT			64 /* allow extended devt */
// 本地容量处理
#define GENHD_FL_NATIVE_CAPACITY		128

// SCSI命令的最大数量
#define BLK_SCSI_MAX_CMDS	(256)
// 每个长整型可以表示的SCSI命令数
#define BLK_SCSI_CMD_PER_LONG	(BLK_SCSI_MAX_CMDS / (sizeof(long) * 8))

// SCSI命令过滤结构
struct blk_scsi_cmd_filter {
	// 可接受的读命令过滤位图
	unsigned long read_ok[BLK_SCSI_CMD_PER_LONG];
	// 可接受的写命令过滤位图
	unsigned long write_ok[BLK_SCSI_CMD_PER_LONG];
	struct kobject kobj;	// 关联的 kobject
};

// 硬盘分区表结构
struct disk_part_tbl {
	struct rcu_head rcu_head;		// 用于RCU同步
	int len;	// 分区数组长度
	struct hd_struct *last_lookup;	// 上一次查找的分区
	struct hd_struct *part[];	// 动态大小的分区数组
};

// 通用硬盘结构
struct gendisk {
	/* major, first_minor and minors are input parameters only,
	 * don't use directly.  Use disk_devt() and disk_max_parts().
	 */
	// 主设备号
	int major;			/* major number of driver */
	// 第一个次设备号
	int first_minor;
	// 最大次设备数，对于不能分区的硬盘为1
	int minors;                     /* maximum number of minors, =1 for
                                         * disks that can't be partitioned. */
	// 硬盘名称
	char disk_name[DISK_NAME_LEN];	/* name of major driver */
	// 设备节点名生成函数
	char *(*devnode)(struct gendisk *gd, mode_t *mode);
	/* Array of pointers to partitions indexed by partno.
	 * Protected with matching bdev lock but stat and other
	 * non-critical accesses use RCU.  Always access through
	 * helpers.
	 */
	// 分区表
	struct disk_part_tbl *part_tbl;	// 分区表指针
	struct hd_struct part0;					// 嵌入的第0分区（通常代表整个硬盘）
	
	const struct block_device_operations *fops;	// 块设备操作函数
	struct request_queue *queue;		// 请求队列
	void *private_data;							// 私有数据指针

	int flags;	// 设备标志
	// FIXME: remove 已弃用
	struct device *driverfs_dev;  // FIXME: remove
	// 从属目录的 kobject
	struct kobject *slave_dir;

	// 随机数状态，用于部分旧的IO调度
	struct timer_rand_state *random;

	// RAID同步IO计数
	atomic_t sync_io;		/* RAID */
	// 异步通知工作结构
	struct work_struct async_notify;
#ifdef  CONFIG_BLK_DEV_INTEGRITY
	// 块设备完整性数据结构
	struct blk_integrity *integrity;
#endif
	int node_id;	// 设备所在NUMA节点ID
};

static inline struct gendisk *part_to_disk(struct hd_struct *part)
{
	if (likely(part)) {
		if (part->partno)
			return dev_to_disk(part_to_dev(part)->parent);
		else
			return dev_to_disk(part_to_dev(part));
	}
	return NULL;
}

static inline int disk_max_parts(struct gendisk *disk)
{
	if (disk->flags & GENHD_FL_EXT_DEVT)
		return DISK_MAX_PARTS;
	return disk->minors;
}

static inline bool disk_partitionable(struct gendisk *disk)
{
	return disk_max_parts(disk) > 1;
}

static inline dev_t disk_devt(struct gendisk *disk)
{
	return disk_to_dev(disk)->devt;
}

static inline dev_t part_devt(struct hd_struct *part)
{
	return part_to_dev(part)->devt;
}

extern struct hd_struct *disk_get_part(struct gendisk *disk, int partno);

static inline void disk_put_part(struct hd_struct *part)
{
	if (likely(part))
		put_device(part_to_dev(part));
}

/*
 * Smarter partition iterator without context limits.
 */
#define DISK_PITER_REVERSE	(1 << 0) /* iterate in the reverse direction */
#define DISK_PITER_INCL_EMPTY	(1 << 1) /* include 0-sized parts */
#define DISK_PITER_INCL_PART0	(1 << 2) /* include partition 0 */
#define DISK_PITER_INCL_EMPTY_PART0 (1 << 3) /* include empty partition 0 */

struct disk_part_iter {
	struct gendisk		*disk;
	struct hd_struct	*part;
	int			idx;
	unsigned int		flags;
};

extern void disk_part_iter_init(struct disk_part_iter *piter,
				 struct gendisk *disk, unsigned int flags);
extern struct hd_struct *disk_part_iter_next(struct disk_part_iter *piter);
extern void disk_part_iter_exit(struct disk_part_iter *piter);

extern struct hd_struct *disk_map_sector_rcu(struct gendisk *disk,
					     sector_t sector);

/*
 * Macros to operate on percpu disk statistics:
 *
 * {disk|part|all}_stat_{add|sub|inc|dec}() modify the stat counters
 * and should be called between disk_stat_lock() and
 * disk_stat_unlock().
 *
 * part_stat_read() can be called at any time.
 *
 * part_stat_{add|set_all}() and {init|free}_part_stats are for
 * internal use only.
 */
#ifdef	CONFIG_SMP
#define part_stat_lock()	({ rcu_read_lock(); get_cpu(); })
#define part_stat_unlock()	do { put_cpu(); rcu_read_unlock(); } while (0)

#define __part_stat_add(cpu, part, field, addnd)			\
	(per_cpu_ptr((part)->dkstats, (cpu))->field += (addnd))

#define part_stat_read(part, field)					\
({									\
	typeof((part)->dkstats->field) res = 0;				\
	unsigned int _cpu;						\
	for_each_possible_cpu(_cpu)					\
		res += per_cpu_ptr((part)->dkstats, _cpu)->field;	\
	res;								\
})

static inline void part_stat_set_all(struct hd_struct *part, int value)
{
	int i;

	for_each_possible_cpu(i)
		memset(per_cpu_ptr(part->dkstats, i), value,
				sizeof(struct disk_stats));
}

static inline int init_part_stats(struct hd_struct *part)
{
	part->dkstats = alloc_percpu(struct disk_stats);
	if (!part->dkstats)
		return 0;
	return 1;
}

static inline void free_part_stats(struct hd_struct *part)
{
	free_percpu(part->dkstats);
}

#else /* !CONFIG_SMP */
#define part_stat_lock()	({ rcu_read_lock(); 0; })
#define part_stat_unlock()	rcu_read_unlock()

#define __part_stat_add(cpu, part, field, addnd)				\
	((part)->dkstats.field += addnd)

#define part_stat_read(part, field)	((part)->dkstats.field)

static inline void part_stat_set_all(struct hd_struct *part, int value)
{
	memset(&part->dkstats, value, sizeof(struct disk_stats));
}

static inline int init_part_stats(struct hd_struct *part)
{
	return 1;
}

static inline void free_part_stats(struct hd_struct *part)
{
}

#endif /* CONFIG_SMP */

#define part_stat_add(cpu, part, field, addnd)	do {			\
	__part_stat_add((cpu), (part), field, addnd);			\
	if ((part)->partno)						\
		__part_stat_add((cpu), &part_to_disk((part))->part0,	\
				field, addnd);				\
} while (0)

#define part_stat_dec(cpu, gendiskp, field)				\
	part_stat_add(cpu, gendiskp, field, -1)
#define part_stat_inc(cpu, gendiskp, field)				\
	part_stat_add(cpu, gendiskp, field, 1)
#define part_stat_sub(cpu, gendiskp, field, subnd)			\
	part_stat_add(cpu, gendiskp, field, -subnd)

static inline void part_inc_in_flight(struct hd_struct *part, int rw)
{
	part->in_flight[rw]++;
	if (part->partno)
		part_to_disk(part)->part0.in_flight[rw]++;
}

static inline void part_dec_in_flight(struct hd_struct *part, int rw)
{
	part->in_flight[rw]--;
	if (part->partno)
		part_to_disk(part)->part0.in_flight[rw]--;
}

static inline int part_in_flight(struct hd_struct *part)
{
	return part->in_flight[0] + part->in_flight[1];
}

/* block/blk-core.c */
extern void part_round_stats(int cpu, struct hd_struct *part);

/* block/genhd.c */
extern void add_disk(struct gendisk *disk);
extern void del_gendisk(struct gendisk *gp);
extern void unlink_gendisk(struct gendisk *gp);
extern struct gendisk *get_gendisk(dev_t dev, int *partno);
extern struct block_device *bdget_disk(struct gendisk *disk, int partno);

extern void set_device_ro(struct block_device *bdev, int flag);
extern void set_disk_ro(struct gendisk *disk, int flag);

static inline int get_disk_ro(struct gendisk *disk)
{
	return disk->part0.policy;
}

/* drivers/char/random.c */
extern void add_disk_randomness(struct gendisk *disk);
extern void rand_initialize_disk(struct gendisk *disk);

static inline sector_t get_start_sect(struct block_device *bdev)
{
	return bdev->bd_part->start_sect;
}
static inline sector_t get_capacity(struct gendisk *disk)
{
	return disk->part0.nr_sects;
}
static inline void set_capacity(struct gendisk *disk, sector_t size)
{
	disk->part0.nr_sects = size;
}

#ifdef CONFIG_SOLARIS_X86_PARTITION

#define SOLARIS_X86_NUMSLICE	16
#define SOLARIS_X86_VTOC_SANE	(0x600DDEEEUL)

struct solaris_x86_slice {
	__le16 s_tag;		/* ID tag of partition */
	__le16 s_flag;		/* permission flags */
	__le32 s_start;		/* start sector no of partition */
	__le32 s_size;		/* # of blocks in partition */
};

struct solaris_x86_vtoc {
	unsigned int v_bootinfo[3];	/* info needed by mboot (unsupported) */
	__le32 v_sanity;		/* to verify vtoc sanity */
	__le32 v_version;		/* layout version */
	char	v_volume[8];		/* volume name */
	__le16	v_sectorsz;		/* sector size in bytes */
	__le16	v_nparts;		/* number of partitions */
	unsigned int v_reserved[10];	/* free space */
	struct solaris_x86_slice
		v_slice[SOLARIS_X86_NUMSLICE]; /* slice headers */
	unsigned int timestamp[SOLARIS_X86_NUMSLICE]; /* timestamp (unsupported) */
	char	v_asciilabel[128];	/* for compatibility */
};

#endif /* CONFIG_SOLARIS_X86_PARTITION */

#ifdef CONFIG_BSD_DISKLABEL
/*
 * BSD disklabel support by Yossi Gottlieb <yogo@math.tau.ac.il>
 * updated by Marc Espie <Marc.Espie@openbsd.org>
 */

/* check against BSD src/sys/sys/disklabel.h for consistency */

#define BSD_DISKMAGIC	(0x82564557UL)	/* The disk magic number */
#define BSD_MAXPARTITIONS	16
#define OPENBSD_MAXPARTITIONS	16
#define BSD_FS_UNUSED		0	/* disklabel unused partition entry ID */
struct bsd_disklabel {
	__le32	d_magic;		/* the magic number */
	__s16	d_type;			/* drive type */
	__s16	d_subtype;		/* controller/d_type specific */
	char	d_typename[16];		/* type name, e.g. "eagle" */
	char	d_packname[16];			/* pack identifier */ 
	__u32	d_secsize;		/* # of bytes per sector */
	__u32	d_nsectors;		/* # of data sectors per track */
	__u32	d_ntracks;		/* # of tracks per cylinder */
	__u32	d_ncylinders;		/* # of data cylinders per unit */
	__u32	d_secpercyl;		/* # of data sectors per cylinder */
	__u32	d_secperunit;		/* # of data sectors per unit */
	__u16	d_sparespertrack;	/* # of spare sectors per track */
	__u16	d_sparespercyl;		/* # of spare sectors per cylinder */
	__u32	d_acylinders;		/* # of alt. cylinders per unit */
	__u16	d_rpm;			/* rotational speed */
	__u16	d_interleave;		/* hardware sector interleave */
	__u16	d_trackskew;		/* sector 0 skew, per track */
	__u16	d_cylskew;		/* sector 0 skew, per cylinder */
	__u32	d_headswitch;		/* head switch time, usec */
	__u32	d_trkseek;		/* track-to-track seek, usec */
	__u32	d_flags;		/* generic flags */
#define NDDATA 5
	__u32	d_drivedata[NDDATA];	/* drive-type specific information */
#define NSPARE 5
	__u32	d_spare[NSPARE];	/* reserved for future use */
	__le32	d_magic2;		/* the magic number (again) */
	__le16	d_checksum;		/* xor of data incl. partitions */

			/* filesystem and partition information: */
	__le16	d_npartitions;		/* number of partitions in following */
	__le32	d_bbsize;		/* size of boot area at sn0, bytes */
	__le32	d_sbsize;		/* max size of fs superblock, bytes */
	struct	bsd_partition {		/* the partition table */
		__le32	p_size;		/* number of sectors in partition */
		__le32	p_offset;	/* starting sector */
		__le32	p_fsize;	/* filesystem basic fragment size */
		__u8	p_fstype;	/* filesystem type, see below */
		__u8	p_frag;		/* filesystem fragments per block */
		__le16	p_cpg;		/* filesystem cylinders per group */
	} d_partitions[BSD_MAXPARTITIONS];	/* actually may be more */
};

#endif	/* CONFIG_BSD_DISKLABEL */

#ifdef CONFIG_UNIXWARE_DISKLABEL
/*
 * Unixware slices support by Andrzej Krzysztofowicz <ankry@mif.pg.gda.pl>
 * and Krzysztof G. Baranowski <kgb@knm.org.pl>
 */

#define UNIXWARE_DISKMAGIC     (0xCA5E600DUL)	/* The disk magic number */
#define UNIXWARE_DISKMAGIC2    (0x600DDEEEUL)	/* The slice table magic nr */
#define UNIXWARE_NUMSLICE      16
#define UNIXWARE_FS_UNUSED     0		/* Unused slice entry ID */

struct unixware_slice {
	__le16   s_label;	/* label */
	__le16   s_flags;	/* permission flags */
	__le32   start_sect;	/* starting sector */
	__le32   nr_sects;	/* number of sectors in slice */
};

struct unixware_disklabel {
	__le32   d_type;               	/* drive type */
	__le32   d_magic;                /* the magic number */
	__le32   d_version;              /* version number */
	char    d_serial[12];           /* serial number of the device */
	__le32   d_ncylinders;           /* # of data cylinders per device */
	__le32   d_ntracks;              /* # of tracks per cylinder */
	__le32   d_nsectors;             /* # of data sectors per track */
	__le32   d_secsize;              /* # of bytes per sector */
	__le32   d_part_start;           /* # of first sector of this partition */
	__le32   d_unknown1[12];         /* ? */
 	__le32	d_alt_tbl;              /* byte offset of alternate table */
 	__le32	d_alt_len;              /* byte length of alternate table */
 	__le32	d_phys_cyl;             /* # of physical cylinders per device */
 	__le32	d_phys_trk;             /* # of physical tracks per cylinder */
 	__le32	d_phys_sec;             /* # of physical sectors per track */
 	__le32	d_phys_bytes;           /* # of physical bytes per sector */
 	__le32	d_unknown2;             /* ? */
	__le32   d_unknown3;             /* ? */
	__le32	d_pad[8];               /* pad */

	struct unixware_vtoc {
		__le32	v_magic;		/* the magic number */
		__le32	v_version;		/* version number */
		char	v_name[8];		/* volume name */
		__le16	v_nslices;		/* # of slices */
		__le16	v_unknown1;		/* ? */
		__le32	v_reserved[10];		/* reserved */
		struct unixware_slice
			v_slice[UNIXWARE_NUMSLICE];	/* slice headers */
	} vtoc;

};  /* 408 */

#endif /* CONFIG_UNIXWARE_DISKLABEL */

#ifdef CONFIG_MINIX_SUBPARTITION
#   define MINIX_NR_SUBPARTITIONS  4
#endif /* CONFIG_MINIX_SUBPARTITION */

#define ADDPART_FLAG_NONE	0
#define ADDPART_FLAG_RAID	1
#define ADDPART_FLAG_WHOLEDISK	2

extern int blk_alloc_devt(struct hd_struct *part, dev_t *devt);
extern void blk_free_devt(dev_t devt);
extern dev_t blk_lookup_devt(const char *name, int partno);
extern char *disk_name (struct gendisk *hd, int partno, char *buf);

extern int disk_expand_part_tbl(struct gendisk *disk, int target);
extern int rescan_partitions(struct gendisk *disk, struct block_device *bdev);
extern struct hd_struct * __must_check add_partition(struct gendisk *disk,
						     int partno, sector_t start,
						     sector_t len, int flags);
extern void delete_partition(struct gendisk *, int);
extern void printk_all_partitions(void);

extern struct gendisk *alloc_disk_node(int minors, int node_id);
extern struct gendisk *alloc_disk(int minors);
extern struct kobject *get_disk(struct gendisk *disk);
extern void put_disk(struct gendisk *disk);
extern void blk_register_region(dev_t devt, unsigned long range,
			struct module *module,
			struct kobject *(*probe)(dev_t, int *, void *),
			int (*lock)(dev_t, void *),
			void *data);
extern void blk_unregister_region(dev_t devt, unsigned long range);

extern ssize_t part_size_show(struct device *dev,
			      struct device_attribute *attr, char *buf);
extern ssize_t part_stat_show(struct device *dev,
			      struct device_attribute *attr, char *buf);
extern ssize_t part_inflight_show(struct device *dev,
			      struct device_attribute *attr, char *buf);
#ifdef CONFIG_FAIL_MAKE_REQUEST
extern ssize_t part_fail_show(struct device *dev,
			      struct device_attribute *attr, char *buf);
extern ssize_t part_fail_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count);
#endif /* CONFIG_FAIL_MAKE_REQUEST */

#else /* CONFIG_BLOCK */

static inline void printk_all_partitions(void) { }

static inline dev_t blk_lookup_devt(const char *name, int partno)
{
	dev_t devt = MKDEV(0, 0);
	return devt;
}

#endif /* CONFIG_BLOCK */

#endif /* _LINUX_GENHD_H */
