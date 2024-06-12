#include <linux/mutex.h>

// 定义一个函数指针类型 kobj_probe_t，它接受设备号、一个整数指针和一个void指针，返回一个指向 kobject 的指针。
typedef struct kobject *kobj_probe_t(dev_t, int *, void *);
// 前向声明一个 kobj_map 结构体，这是管理kobject映射的关键数据结构。
struct kobj_map;

// 将一个设备号范围映射到一个kobject。该函数为设备号范围内的每个设备提供了创建kobject的机制。
int kobj_map(struct kobj_map *, dev_t, unsigned long, struct module *,
	     kobj_probe_t *, int (*)(dev_t, void *), void *);
// 从kobj_map中解除一个设备号范围的映射。
void kobj_unmap(struct kobj_map *, dev_t, unsigned long);
// 在kobj_map中查找与给定设备号对应的kobject，如果找到，返回kobject的指针，否则返回NULL。
struct kobject *kobj_lookup(struct kobj_map *, dev_t, int *);
// 初始化一个kobj_map结构，使用提供的探测函数和互斥锁。
struct kobj_map *kobj_map_init(kobj_probe_t *, struct mutex *);
