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
	// 链表头，用于将多个 cdev 结构链接起来
	struct list_head list;
	// 设备号（包括主设备号和次设备号）
	dev_t dev;
	// 此设备号范围内包含的设备数
	unsigned int count;
};

void cdev_init(struct cdev *, const struct file_operations *);

struct cdev *cdev_alloc(void);

void cdev_put(struct cdev *p);

int cdev_add(struct cdev *, dev_t, unsigned);

void cdev_del(struct cdev *);

int cdev_index(struct inode *inode);

void cd_forget(struct inode *);

extern struct backing_dev_info directly_mappable_cdev_bdi;

#endif
