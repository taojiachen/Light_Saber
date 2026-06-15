#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "esp_board_init.h"
#include "websocket.h"
#include "app_sr.h"

static const char *TAG = "app_sr";

// 录音状态标志
static bool api_recording = false;          // API 触发的录音（按键按下时启动）
static TaskHandle_t i2s_read_task_handle = NULL;
QueueHandle_t audio_encode_queue = NULL;    // 外部声明，在 audio.c 中用到

// I2S 读取任务：持续读取 PCM 数据，根据 api_recording 决定是否入队
static void i2s_read_task(void *pvParameter)
{
    // 每帧 PCM 数据大小（字节），与编码器要求一致：60ms @16kHz 单声道 = 960 个 int16_t = 1920 字节
    const size_t frame_bytes = BYTES_PER_FRAME;  // 1920
    int16_t *pcm_buffer = heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm_buffer) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer");
        vTaskDelete(NULL);
        return;
    }

    // 注意：旧版 ESP-IDF 的 esp_i2s_read 没有 bytes_read 参数，需要根据实际 API 调整
    // 如果你的 i2s 驱动支持阻塞读取，直接传入长度即可
    while (1) {
        // 阻塞读取指定字节数（实际读取长度应与 frame_bytes 一致）
        esp_err_t ret = esp_i2s_read(pcm_buffer, frame_bytes);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 如果处于录音状态，则将 PCM 帧发送到编码队列
        if (api_recording) {
            if (xQueueSend(audio_encode_queue, pcm_buffer, 0) != pdPASS) {
                ESP_LOGW(TAG, "Audio encode queue full, drop frame");
            }
        }
    }
    free(pcm_buffer);
    vTaskDelete(NULL);
}

esp_err_t app_sr_start(void)
{
    // 创建队列用于传递 PCM 音频数据给编码任务
    audio_encode_queue = xQueueCreate(10, BYTES_PER_FRAME);
    if (!audio_encode_queue) {
        ESP_LOGE(TAG, "Failed to create audio encode queue");
        return ESP_FAIL;
    }

    // 创建 I2S 读取任务（栈大小 4KB，核心 1）
    BaseType_t ret = xTaskCreatePinnedToCore(i2s_read_task, "i2s_read", 4096, NULL, 5, &i2s_read_task_handle, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create I2S read task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "I2S audio capture started (no wakeword/VAD)");
    return ESP_OK;
}

void app_sr_start_api_recording(int duration_ms)
{
    if (api_recording) {
        ESP_LOGW(TAG, "Already recording");
        return;
    }
    api_recording = true;
    ESP_LOGI(TAG, "API recording started, duration=%d ms (ignored, stop by key up)", duration_ms);
}

void app_sr_stop_api_recording(void)
{
    if (!api_recording) {
        ESP_LOGW(TAG, "No active recording");
        return;
    }
    api_recording = false;
    ESP_LOGI(TAG, "API recording stopped manually");
}

bool app_sr_is_api_recording(void)
{
    return api_recording;
}