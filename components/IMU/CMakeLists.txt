set(imu_sources
    core/madgwick_ahrs.c
)

# 根据 Kconfig 选择对应的驱动源码目录
if(CONFIG_IMU_MPU6050)
    list(APPEND imu_sources
        drivers/mpu6050/mpu6050.c
        drivers/mpu6050/driver_mpu6050.c
    )
elseif(CONFIG_IMU_ICM20948)   # 未来添加
    list(APPEND imu_sources
        drivers/icm20948/icm20948.c
        drivers/icm20948/driver_icm20948.c
    )
# elseif(CONFIG_IMU_BMI160) ...
endif()

idf_component_register(
    SRCS ${imu_sources}
    INCLUDE_DIRS
        core
        .                            # imu_selector.h 可被外部引用
        drivers/mpu6050              # 让适配器能 include "mpu6050.h"
    REQUIRES driver                  # 基础驱动
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format)