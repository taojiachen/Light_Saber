#ifndef IMU_H
#define IMU_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void imu_init(void);
void imu_start_task(void);   // 创建并启动 IMU 读取解算任务

esp_err_t imu_get_angles(float *pitch, float *roll);

#ifdef __cplusplus
}
#endif

#endif