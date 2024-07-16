#ifndef _LINUX_CDEV_H
#define _LINUX_CDEV_H

#include <linux/kobject.h>
#include <linux/kdev_t.h>
#include <linux/list.h>

struct file_operations;
struct inode;
struct module;

// 该对象代表一个字符设备
struct cdev {
	// 嵌入的 kobject，用于在 sysfs 中表示此字符设备
	struct kobject kobj;
	// 指向拥有这个设备的模块的指针，主要用于模块计数
	struct module *owner;
	// 指向文件操作结构体的指针，定义了设备的行为
	const struct file_operations *ops;
	// 链表头，用于将多个 cdev 结构链接起来。将系统中的字符设备形成链表
	// 自己理解，在fs/char_dev.c中，每打开一次cdev设备，都会将一个inode的i_devices添加到该链表中，用来把所有inode串起来，可看cdev_open，cd_forget等函数
	struct list_head list;
	// 设备号（包括主设备号和次设备号）
	dev_t dev;
	// 此设备号范围内包含的设备数。
	// 隶属于同一主设备号的次设备号的数量，表示当前设备驱动程序控制的设备的数量
	unsigned int count;
};

// 初始化一个 cdev 结构
void cdev_init(struct cdev *, const struct file_operations *);

// 分配一个 cdev 结构
struct cdev *cdev_alloc(void);

// 减少一个 cdev 结构的引用计数
void cdev_put(struct cdev *p);

// 将一个 cdev 结构添加到系统中
int cdev_add(struct cdev *, dev_t, unsigned);

// 从系统中删除一个 cdev 结构
void cdev_del(struct cdev *);

// 从 inode 检索 cdev 结构的索引
int cdev_index(struct inode *inode);

// 帮助函数，用于在 inode 中忘记 cdev 关联
void cd_forget(struct inode *);

// 直接可映射字符设备的 backing_dev_info 结构的外部声明
extern struct backing_dev_info directly_mappable_cdev_bdi;

#endif
