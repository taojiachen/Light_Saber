#include "state_machine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "websocket.h"
#include "app_sr.h"

static const char *TAG = "STATE_MACHINE";

static QueueHandle_t event_queue = NULL;
static TaskHandle_t sm_task_handle = NULL;
static state_t current_state = STATE_IDLE;

// 录音时长（毫秒），设为足够大（10分钟），实际由松开按键提前停止
#define RECORDING_MAX_DURATION_MS (10 * 60 * 1000)

// 前向声明
static void state_machine_task(void *pvParams);
static void process_event(state_event_t event);
static void send_recording_start(void);
static void send_recording_stop(void);

esp_err_t state_machine_init(void)
{
    event_queue = xQueueCreate(10, sizeof(state_event_t));
    if (!event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_FAIL;
    }

    xTaskCreate(state_machine_task, "state_machine", 2 * 1024, NULL, 5, &sm_task_handle);
    ESP_LOGI(TAG, "State machine initialized, initial state: IDLE");
    return ESP_OK;
}

void state_machine_send_event(state_event_t event)
{
    if (event_queue) {
        xQueueSend(event_queue, &event, 0);
    }
}

state_t state_machine_get_current_state(void)
{
    return current_state;
}

static void state_machine_task(void *pvParams)
{
    state_event_t event;
    while (1) {
        if (xQueueReceive(event_queue, &event, portMAX_DELAY) == pdTRUE) {
            process_event(event);
        }
    }
}

// 发送录音开始 JSON
static void send_recording_start(void)
{
    const char *msg = "{\"type\":\"recording_start\"}";
    if (ws_is_connected()) {
        ws_send_text(msg, strlen(msg));
        ESP_LOGI(TAG, "Sent: %s", msg);
    } else {
        ESP_LOGW(TAG, "WebSocket not connected, cannot send recording_start");
    }
}

// 发送录音结束 JSON
static void send_recording_stop(void)
{
    const char *msg = "{\"type\":\"recording_end\"}";
    if (ws_is_connected()) {
        ws_send_text(msg, strlen(msg));
        ESP_LOGI(TAG, "Sent: %s", msg);
    } else {
        ESP_LOGW(TAG, "WebSocket not connected, cannot send recording_end");
    }
}

static void process_event(state_event_t event)
{
    ESP_LOGI(TAG, "Event: %d, Current state: %d", event, current_state);

    switch (current_state) {
    case STATE_IDLE:
        if (event == EVENT_KEY_PRESS_DOWN) {
            // 1. 发送开始消息
            send_recording_start();
            // 2. 启动录音（API 录音模式，时长足够长）
            app_sr_start_api_recording(RECORDING_MAX_DURATION_MS);
            // 3. 切换到录音状态
            current_state = STATE_RECORDING;
            ESP_LOGI(TAG, "State -> RECORDING");
        }
        break;

    case STATE_RECORDING:
        if (event == EVENT_KEY_UP) {
            // 1. 停止录音
            app_sr_stop_api_recording();
            // 2. 发送结束消息
            send_recording_stop();
            // 3. 回到空闲状态
            current_state = STATE_IDLE;
            ESP_LOGI(TAG, "State -> IDLE");
        }
        break;

    default:
        break;
    }
}