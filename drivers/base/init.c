/*
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 *
 * This file is released under the GPLv2
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/memory.h>

#include "base.h"

/**
 * driver_init - initialize driver model.
 *
 * Call the driver model init functions to initialize their
 * subsystems. Called early from init/main.c.
 */
/**
 * driver_init - 初始化驱动模型。
 *
 * 调用驱动模型的初始化函数来初始化它们的子系统。在 init/main.c 中的早期调用。
 */
void __init driver_init(void)
{
	/* These are the core pieces */
	/* 这些是核心组件 */
	devtmpfs_init();         // 初始化 devtmpfs，这是一个在内存中运行的文件系统，用于自动创建设备文件
	devices_init();          // 初始化设备模型，设置设备子系统
	buses_init();            // 初始化总线子系统，管理不同类型的总线
	classes_init();          // 初始化类子系统，管理不同类型的设备类
	firmware_init();         // 初始化固件服务，如加载和请求固件
	hypervisor_init();       // 初始化与虚拟化相关的功能

	/* These are also core pieces, but must come after the
	 * core core pieces.
	 */
	/* 这些也是核心组件，但必须在核心组件之后调用 */
	platform_bus_init();     // 初始化平台总线，处理平台设备
	system_bus_init();       // 初始化系统总线，处理内部核心总线
	cpu_dev_init();          // 初始化处理器设备，管理 CPU 设备
	memory_dev_init();       // 初始化内存设备管理，管理系统内存设备
}
