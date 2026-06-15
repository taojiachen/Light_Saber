#ifndef IMU_PORT_H
#define IMU_PORT_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"


/* ========== 数据有效性标志 ========== */
#define IMU_DATA_ACCEL_VALID  (1 << 0)
#define IMU_DATA_GYRO_VALID   (1 << 1)
#define IMU_DATA_MAG_VALID    (1 << 2)

/* ========== 统一传感器数据结构 ========== */
typedef struct {
    float accel_x, accel_y, accel_z;   // g
    float gyro_x,  gyro_y,  gyro_z;    // dps
    float mag_x,   mag_y,   mag_z;     // uT（六轴时为0）
    int32_t active_level;              //运动强度
    uint8_t data_flags;
} imu_data_t;

/* ========== 抽象驱动接口 ========== */
typedef struct {
    int  (*init)(void);
    int  (*read)(imu_data_t *data);
    void (*deinit)(void);
} imu_driver_t;

/* ========== 硬件配置结构体（应用层填充） ========== */
typedef struct {
    uint8_t i2c_port;        // I2C 端口号，如 I2C_NUM_0
    int     sda_pin;         // SDA 引脚号
    int     scl_pin;         // SCL 引脚号
    uint32_t clk_speed;      // I2C 时钟频率 (Hz)
    int     int_pin;         // 中断引脚，-1 表示不使用中断
    uint16_t dev_addr;       // I2C 设备地址（如 0x68）
} imu_hw_config_t;

/** 全局硬件配置实例，由应用层在 imu_init() 前初始化 */
extern imu_hw_config_t imu_hw_config;

/* ========== 上下文结构体（驱动内部使用） ========== */
typedef struct imu_driver_context {
    void *device_handle;               // 指向具体设备句柄（如 mpu6050_handle_t）
    SemaphoreHandle_t data_ready_sem;  // 数据就绪信号量
    uint32_t sample_rate;              // 当前采样频率（Hz）
    // 未来可扩展：校准参数、时间戳等
} imu_driver_context_t;

/** 阻塞等待数据就绪，由驱动实现，应用层调用 */
bool imu_wait_for_data(uint32_t timeout_ms);

#endif // IMU_PORT_H