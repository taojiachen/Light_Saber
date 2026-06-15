#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "app_key.h"
#include "state_machine.h"
#include "imu.h"          // 获取 IMU 角度

#define BUTTON_GPIO         4
#define DEBOUNCE_DELAY_MS   30      // 消抖延时（毫秒）
#define ESP_INTR_FLAG_DEFAULT 0

static const char *TAG = "button_isr";
static TaskHandle_t button_task_handle = NULL;

// 导出的全局变量（用于外部读取按键时记录的角度）
float g_key_press_pitch = 0;
float g_key_press_roll = 0;
bool g_key_pressed_flag = false;

// 中断服务函数（仅发送通知）
static void IRAM_ATTR button_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(button_task_handle, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// 消抖与事件处理任务
static void button_debounce_task(void *pvParameter)
{
    uint32_t ulNotificationValue;
    int last_stable_level;   // 上一次稳定电平（1=未按下，0=按下）

    // 1. 读取当前电平作为初始稳定状态（消除上电瞬间误判）
    last_stable_level = gpio_get_level(BUTTON_GPIO);
    ESP_LOGI(TAG, "Initial stable level: %d (0=pressed)", last_stable_level);

    // 2. 启动后等待 500ms，避开系统初始化阶段的不稳定期
    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        // 等待中断通知
        ulNotificationValue = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (ulNotificationValue > 0) {
            // 消抖：延时后再次读取电平
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_DELAY_MS));
            int current_level = gpio_get_level(BUTTON_GPIO);

            // 只有电平发生变化时才处理
            if (current_level != last_stable_level) {
                last_stable_level = current_level;
                if (current_level == 0) {
                    // 按键按下：记录当前 IMU 角度
                    imu_get_angles(&g_key_press_pitch, &g_key_press_roll);
                    g_key_pressed_flag = true;
                    ESP_LOGI(TAG, "Button pressed, recorded angle: pitch=%.1f roll=%.1f",
                             g_key_press_pitch, g_key_press_roll);
                    state_machine_send_event(EVENT_KEY_PRESS_DOWN);
                } else {
                    ESP_LOGI(TAG, "Button released");
                    state_machine_send_event(EVENT_KEY_UP);
                }
            }
        }
    }
}

void button_init(void)
{
    // 1. 配置 GPIO（双边沿触发，内部上拉）
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,      // 双边沿触发
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };
    gpio_config(&io_conf);

    // 2. 创建消抖任务（高优先级）
    xTaskCreate(button_debounce_task, "btn_debounce", 2048, NULL, 10, &button_task_handle);

    // 3. 安装 GPIO ISR 服务（如果尚未安装，避免重复安装）
    static bool isr_installed = false;
    if (!isr_installed) {
        gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
        isr_installed = true;
    }

    // 4. 清除可能残留的中断标志：先禁用中断，延时，读取电平，再使能
    gpio_intr_disable(BUTTON_GPIO);
    vTaskDelay(pdMS_TO_TICKS(50));          // 等待电平稳定
    (void)gpio_get_level(BUTTON_GPIO);      // 读取一次，清空状态寄存器
    gpio_intr_enable(BUTTON_GPIO);

    // 5. 注册中断处理函数
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);

    ESP_LOGI(TAG, "Button GPIO interrupt initialized (ANYEDGE, pullup)");
}

// 可选：提供重置标志的函数（供外部使用）
void button_reset_record_flag(void)
{
    g_key_pressed_flag = false;
}

// 可选：获取最后一次记录的角度（供外部调用）
void button_get_last_recorded_angle(float *pitch, float *roll)
{
    if (pitch) *pitch = g_key_press_pitch;
    if (roll) *roll = g_key_press_roll;
}