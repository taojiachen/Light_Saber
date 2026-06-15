#include "imu.h"
#include "imu_selector.h"  // 提供 imu_get_driver()
#include "madgwick_ahrs.h" // 姿态融合算法
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_timer.h"

static const char *TAG = "IMU";

#define CONFIG_IMU_I2C_SDA_PIN 20
#define CONFIG_IMU_I2C_SCL_PIN 21
#define CONFIG_IMU_I2C_CLK_SPEED 400000
#define CONFIG_IMU_INT_PIN 17

uint16_t interval_ms = 0;
float roll, pitch, yaw;

imu_hw_config_t imu_hw_config = {
    .i2c_port = I2C_NUM_0,
    .sda_pin = CONFIG_IMU_I2C_SDA_PIN,
    .scl_pin = CONFIG_IMU_I2C_SCL_PIN,
    .clk_speed = CONFIG_IMU_I2C_CLK_SPEED,
    .int_pin = CONFIG_IMU_INT_PIN,
    .dev_addr = 0x68 // MPU6050 默认地址
};

// Madgwick 滤波器实例（静态全局，仅本文件可见）
static madgwick_ahrs_t ahrs = {
    .q0 = 1.0f,
    .q1 = 0.0f,
    .q2 = 0.0f,
    .q3 = 0.0f,
    .beta = 0.2f,         // 可调节的滤波增益，需根据实际情况调整
    .sample_freq = 50.0f // 与驱动中设置的采样率一致
};

// TaskHandle_t imu_task_handle = NULL;

void imu_init(void)
{
    // 1. 初始化 I2C 总线
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = imu_hw_config.sda_pin,
        .scl_io_num = imu_hw_config.scl_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = imu_hw_config.clk_speed,
    };

    esp_err_t ret = i2c_param_config(imu_hw_config.i2c_port, &i2c_conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return;
    }
    ret = i2c_driver_install(imu_hw_config.i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return;
    }

    // 2. 获取选中的 IMU 驱动并初始化传感器
    const imu_driver_t *imu = imu_get_driver();
    if (imu->init() != 0)
    {
        ESP_LOGE(TAG, "IMU init failed");
        return;
    }
    ESP_LOGI(TAG, "IMU initialized");
}

esp_err_t imu_get_angles(float *out_pitch, float *out_roll) {
    if (out_pitch) *out_pitch = pitch;
    if (out_roll) *out_roll = roll;
    return ESP_OK;
}

static void imu_task(void *pvParameters)
{
    const imu_driver_t *imu = imu_get_driver();
    uint32_t loop_cnt = 0;

    while (1) {
        imu_data_t data;
        int retry = 3;               // 最多重试3次
        int ret = -1;
        while (retry-- > 0) {
            ret = imu->read(&data);
            if (ret == 0) break;
            vTaskDelay(pdMS_TO_TICKS(1));  // 重试前短暂等待
        }
        if (ret == 0) {
            madgwick_update(&ahrs, &data);
            madgwick_get_euler(&ahrs, &roll, &pitch, &yaw);
            if (++loop_cnt >= 20) {        // 20 * 20ms = 400ms 打印一次
                loop_cnt = 0;
                ESP_LOGI(TAG, "R:%.2f P:%.2f Y:%.2f Active: %ld",
                         roll, pitch, yaw, data.active_level);
            }
        } else {
            ESP_LOGW(TAG, "IMU read failed after retries");
        }
        vTaskDelay(pdMS_TO_TICKS(20));     // 100Hz 轮询
    }
}

void imu_start_task(void)
{
    xTaskCreate(imu_task, "imu_task", 4096, NULL, 5, NULL);
}