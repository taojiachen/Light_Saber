// components/imu_solver/drivers/driver_mpu6050.c
#include "imu_port.h"
#include "mpu6050.h"               // 原有驱动头文件
#include "esp_log.h"

static const char *TAG_DRV = "mpu6050_drv";

static imu_driver_context_t ctx;  // 每个驱动仅一个静态上下文

/* ---------- 中断服务函数 ---------- */
static void IRAM_ATTR mpu6050_data_ready_isr(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(ctx.data_ready_sem, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* ---------- 初始化适配器 ---------- */
static int mpu6050_adapter_init(void) {
    // // 1. 创建信号量（如果未创建）
    // if (ctx.data_ready_sem == NULL) {
    //     ctx.data_ready_sem = xSemaphoreCreateBinary();
    //     if (ctx.data_ready_sem == NULL) return -1;
    // }

    // 2. 创建 MPU6050 句柄
    mpu6050_handle_t mpu = mpu6050_create(imu_hw_config.i2c_port, imu_hw_config.dev_addr);
    if (mpu == NULL) return -1;
    ctx.device_handle = mpu;

    // 唤醒并配置量程
    if (mpu6050_wake_up(mpu) != ESP_OK) return -1;
    if (mpu6050_config(mpu, ACCE_FS_2G, GYRO_FS_250DPS) != ESP_OK) return -1;

    // 4. 设置数字低通滤波器 (DLPF) dlpf_cfg = 3 对应加速度带宽 44Hz，陀螺仪带宽 42Hz
    if (mpu6050_set_dlpf(mpu, 3) != ESP_OK) {
        ESP_LOGE(TAG_DRV, "Failed to set DLPF");
        // 可忽略错误继续运行，但记录日志
    }

    // 5. 配置采样率分频（陀螺仪内部 1kHz，分频 19 得 50 Hz）
    mpu6050_set_sample_rate_divider(mpu, 19);

    // // 6. 中断配置（若引脚有效）
    // if (imu_hw_config.int_pin >= 0) {
    //     mpu6050_int_config_t int_cfg = {
    //         .interrupt_pin = imu_hw_config.int_pin,
    //         .active_level = INTERRUPT_PIN_ACTIVE_HIGH,
    //         .pin_mode = INTERRUPT_PIN_PUSH_PULL,
    //         .interrupt_latch = INTERRUPT_LATCH_UNTIL_CLEARED,
    //         .interrupt_clear_behavior = INTERRUPT_CLEAR_ON_ANY_READ
    //     };
    //     mpu6050_config_interrupts(mpu, &int_cfg);
    //     mpu6050_enable_interrupts(mpu, MPU6050_DATA_RDY_INT_BIT);
    //     mpu6050_register_isr(mpu, (mpu6050_isr_t)mpu6050_data_ready_isr);
    // }

    ctx.sample_rate = 50;   // 与采样分频一致
    ESP_LOGI(TAG_DRV, "MPU6050 initialized, DLPF=3, int_pin=%d", imu_hw_config.int_pin);
    return 0;
}

/* ---------- 读取数据 ---------- */
static int mpu6050_adapter_read(imu_data_t *data) {
    mpu6050_handle_t mpu = (mpu6050_handle_t)ctx.device_handle;
    if (!mpu) {
        // ESP_LOGE(TAG_DRV, "MPU6050 handle is NULL");
        return -1;
    }

    mpu6050_acce_value_t acce;
    mpu6050_gyro_value_t gyro;
    if (mpu6050_get_acce(mpu, &acce) != ESP_OK) {
        // ESP_LOGE(TAG_DRV, "Failed to get acce");
        return -1;
    }
    if (mpu6050_get_gyro(mpu, &gyro) != ESP_OK) {
        // ESP_LOGE(TAG_DRV, "Failed to get gyro");
        return -1;
    }

    data->accel_x = acce.acce_x;
    data->accel_y = acce.acce_y;
    data->accel_z = acce.acce_z;
    data->gyro_x  = gyro.gyro_x;
    data->gyro_y  = gyro.gyro_y;
    data->gyro_z  = gyro.gyro_z;

    data->mag_x = data->mag_y = data->mag_z = 0.0f;
    data->data_flags = IMU_DATA_ACCEL_VALID | IMU_DATA_GYRO_VALID;

    return 0;
}

/* ---------- 反初始化 ---------- */
static void mpu6050_adapter_deinit(void) {
    if (ctx.device_handle) {
        mpu6050_delete((mpu6050_handle_t)ctx.device_handle);
        ctx.device_handle = NULL;
    }
    if (ctx.data_ready_sem) {
        vSemaphoreDelete(ctx.data_ready_sem);
        ctx.data_ready_sem = NULL;
    }
}

/* ---------- 等待数据就绪 ---------- */
bool imu_wait_for_data(uint32_t timeout_ms) {
    if (ctx.data_ready_sem == NULL) return false;
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(ctx.data_ready_sem, ticks) == pdTRUE;
}

/* ---------- 驱动实例 ---------- */
static const imu_driver_t mpu6050_driver = {
    .init   = mpu6050_adapter_init,
    .read   = mpu6050_adapter_read,
    .deinit = mpu6050_adapter_deinit,
};

const imu_driver_t* imu_get_driver(void) {
    return &mpu6050_driver;
}