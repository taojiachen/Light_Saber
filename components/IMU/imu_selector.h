#ifndef IMU_SELECTOR_H
#define IMU_SELECTOR_H

#include "imu_port.h"

/**
 * @brief 获取当前激活的 IMU 驱动实例指针
 *
 * 该函数由被选中的驱动文件实现，CMake 根据 Kconfig 决定编译哪个实现。
 * 返回的指针在整个生命周期内有效，通常指向一个静态只读结构体。
 */
const imu_driver_t* imu_get_driver(void);

#endif