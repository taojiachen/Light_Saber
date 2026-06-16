#include "led_effect.h"
#include "led_spi.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>

static const char *TAG = "LED_EFFECT";

// 辅助宏
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

// 颜色结构体
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} RGB_t;

// JSON 解析结构体
typedef struct {
    int surge_intensity;
    int sum;          // 用于 type=1 的宽度
} LedPram_t;

typedef struct {
    int duration;
    int cycles;
    RGB_t color_st;
    RGB_t color_ed;
    int style;
    int order;        // 用于 type=1 的方向
    LedPram_t pram;
} LedDataConfig_t;

typedef struct {
    int type;
    LedDataConfig_t data;
} LedEffectConfig_t;

// 全局变量
static LedEffectConfig_t g_led_config;
static SemaphoreHandle_t g_config_mutex = NULL;
static bool s_led_paused = false;
static TaskHandle_t led_task_handle = NULL;

// 配置更新中断标志
static volatile bool g_config_changed = false;

// 函数声明
static void led_effect_type0_style0(LedDataConfig_t data);
static void led_effect_type0_style1(LedDataConfig_t data);
static void led_effect_type0_style2(LedDataConfig_t data);
static void led_effect_type0_style3(LedDataConfig_t data);  // 新激光剑脉冲
static void led_effect_type1(LedDataConfig_t data);
static LedEffectConfig_t parse_led_effect_json(const char *json_str);

// 公共接口
void led_set_paused(bool paused) {
    s_led_paused = paused;
    ESP_LOGI(TAG, "LED task %s", paused ? "paused" : "resumed");
}

void led_update_config_from_json(const char *json_str) {
    if (!json_str) return;
    LedEffectConfig_t new_config = parse_led_effect_json(json_str);
    if (g_config_mutex == NULL) {
        g_config_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(g_config_mutex, portMAX_DELAY);
    g_led_config = new_config;
    xSemaphoreGive(g_config_mutex);
    g_config_changed = true;
    ESP_LOGI(TAG, "LED config updated from JSON");
}

// JSON 解析
static LedEffectConfig_t parse_led_effect_json(const char *json_str) {
    LedEffectConfig_t config = {0};
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return config;
    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsNumber(type_item)) config.type = type_item->valueint;
    cJSON *data_item = cJSON_GetObjectItem(root, "data");
    if (data_item) {
        cJSON *duration = cJSON_GetObjectItem(data_item, "duration");
        if (cJSON_IsNumber(duration)) config.data.duration = duration->valueint;
        cJSON *cycles = cJSON_GetObjectItem(data_item, "cycles");
        if (cJSON_IsNumber(cycles)) config.data.cycles = cycles->valueint;
        cJSON *color_st = cJSON_GetObjectItem(data_item, "color_st");
        if (cJSON_IsArray(color_st) && cJSON_GetArraySize(color_st) >= 3) {
            config.data.color_st.r = cJSON_GetArrayItem(color_st, 0)->valueint;
            config.data.color_st.g = cJSON_GetArrayItem(color_st, 1)->valueint;
            config.data.color_st.b = cJSON_GetArrayItem(color_st, 2)->valueint;
        }
        cJSON *color_ed = cJSON_GetObjectItem(data_item, "color_ed");
        if (cJSON_IsArray(color_ed) && cJSON_GetArraySize(color_ed) >= 3) {
            config.data.color_ed.r = cJSON_GetArrayItem(color_ed, 0)->valueint;
            config.data.color_ed.g = cJSON_GetArrayItem(color_ed, 1)->valueint;
            config.data.color_ed.b = cJSON_GetArrayItem(color_ed, 2)->valueint;
        }
        cJSON *style = cJSON_GetObjectItem(data_item, "style");
        if (cJSON_IsNumber(style)) config.data.style = style->valueint;
        cJSON *order = cJSON_GetObjectItem(data_item, "order");
        if (cJSON_IsNumber(order)) config.data.order = order->valueint;
        cJSON *pram = cJSON_GetObjectItem(data_item, "pram");
        if (pram) {
            cJSON *surge = cJSON_GetObjectItem(pram, "surge_intensity");
            if (cJSON_IsNumber(surge)) config.data.pram.surge_intensity = surge->valueint;
            cJSON *sum = cJSON_GetObjectItem(pram, "sum");
            if (cJSON_IsNumber(sum)) config.data.pram.sum = sum->valueint;
        }
    }
    cJSON_Delete(root);
    return config;
}

// ---------- 效果实现 ----------

// style 0: 柔和渐变（呼吸灯）- 在两个颜色之间缓慢正弦变化
static void led_effect_type0_style0(LedDataConfig_t data) {
    int total_steps = data.duration / 20; // 每20ms一步
    if (total_steps < 1) total_steps = 1;
    float r_diff = data.color_ed.r - data.color_st.r;
    float g_diff = data.color_ed.g - data.color_st.g;
    float b_diff = data.color_ed.b - data.color_st.b;

    for (int step = 0; step < total_steps; step++) {
        if (g_config_changed) return;
        if (s_led_paused) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        // 使用正弦波实现平滑过渡 (0~PI)
        float phase = (float)step / total_steps * 3.14159f;
        float sin_val = (sinf(phase) + 1.0f) / 2.0f; // 0~1
        uint8_t r = data.color_st.r + r_diff * sin_val;
        uint8_t g = data.color_st.g + g_diff * sin_val;
        uint8_t b = data.color_st.b + b_diff * sin_val;
        for (int i = 0; i < LED_NUMBERS; i++) {
            led_spi_set_pixel(i, g, r, b); // 注意顺序：实际函数参数是 (index, g, r, b)
        }
        led_spi_show();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // 最终设为结束色
    for (int i = 0; i < LED_NUMBERS; i++) {
        led_spi_set_pixel(i, data.color_ed.g, data.color_ed.r, data.color_ed.b);
    }
    led_spi_show();
    ESP_LOGI(TAG, "Type0 Style0 (breath) completed");
}

// style 1: 活泼跳变（快速切换两色）
static void led_effect_type0_style1(LedDataConfig_t data) {
    if (data.cycles <= 0) data.cycles = 1;
    int cycle_duration = data.duration / data.cycles;
    int half = cycle_duration / 2;
    if (half < 1) half = 1;

    for (int c = 0; c < data.cycles; c++) {
        if (g_config_changed) return;
        // 起始色
        for (int i = 0; i < LED_NUMBERS; i++) {
            led_spi_set_pixel(i, data.color_st.g, data.color_st.r, data.color_st.b);
        }
        led_spi_show();
        vTaskDelay(pdMS_TO_TICKS(half));
        if (g_config_changed) return;
        // 结束色
        for (int i = 0; i < LED_NUMBERS; i++) {
            led_spi_set_pixel(i, data.color_ed.g, data.color_ed.r, data.color_ed.b);
        }
        led_spi_show();
        vTaskDelay(pdMS_TO_TICKS(half));
    }
    ESP_LOGI(TAG, "Type0 Style1 (quick flash) completed");
}

// style 2: 奇幻波浪（流水灯/拖尾）
static void led_effect_type0_style2(LedDataConfig_t data) {
    int tail_len = data.pram.surge_intensity;
    if (tail_len < 1) tail_len = 3;
    if (tail_len > LED_NUMBERS) tail_len = LED_NUMBERS;

    int total_steps = LED_NUMBERS + tail_len - 1;
    int step_duration = (data.duration / data.cycles) / total_steps;
    if (step_duration < 1) step_duration = 1;

    float r_step = (float)(data.color_ed.r - data.color_st.r) / total_steps;
    float g_step = (float)(data.color_ed.g - data.color_st.g) / total_steps;
    float b_step = (float)(data.color_ed.b - data.color_st.b) / total_steps;

    for (int cycle = 0; cycle < data.cycles; cycle++) {
        if (g_config_changed) return;
        RGB_t current_color = data.color_st;
        for (int step = 0; step < total_steps; step++) {
            if (g_config_changed) return;
            if (s_led_paused) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
            current_color.r = CLAMP(current_color.r + r_step, 0, 255);
            current_color.g = CLAMP(current_color.g + g_step, 0, 255);
            current_color.b = CLAMP(current_color.b + b_step, 0, 255);
            // 清空
            for (int i = 0; i < LED_NUMBERS; i++) {
                led_spi_set_pixel(i, 0, 0, 0);
            }
            // 绘制拖尾
            for (int t = 0; t < tail_len; t++) {
                int pos = step - t;
                if (pos < 0 || pos >= LED_NUMBERS) continue;
                float ratio = 1.0f - (float)t / tail_len;
                uint8_t r = current_color.r * ratio;
                uint8_t g = current_color.g * ratio;
                uint8_t b = current_color.b * ratio;
                led_spi_set_pixel(pos, g, r, b);
            }
            led_spi_show();
            vTaskDelay(pdMS_TO_TICKS(step_duration));
        }
    }
    ESP_LOGI(TAG, "Type0 Style2 (wave) completed");
}

// style 3: 激光剑脉冲 - 在给定颜色附近随机波动
static void led_effect_type0_style3(LedDataConfig_t data) {
    // 基础颜色取 color_st 和 color_ed 的平均值（或直接用 color_st）
    RGB_t base_color = {
        .r = (data.color_st.r + data.color_ed.r) / 2,
        .g = (data.color_st.g + data.color_ed.g) / 2,
        .b = (data.color_st.b + data.color_ed.b) / 2
    };
    // 波动幅度由 surge_intensity 控制 (0~100) 映射到 0~30
    int amplitude = data.pram.surge_intensity * 30 / 100;
    if (amplitude < 1) amplitude = 1;
    if (amplitude > 30) amplitude = 30;

    int total_steps = data.duration / 20; // 每20ms更新一次
    if (total_steps < 1) total_steps = 1;

    for (int step = 0; step < total_steps; step++) {
        if (g_config_changed) return;
        if (s_led_paused) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        // 在基色上叠加随机偏移 (-amplitude ~ +amplitude)
        int r_offset = (esp_random() % (2 * amplitude + 1)) - amplitude;
        int g_offset = (esp_random() % (2 * amplitude + 1)) - amplitude;
        int b_offset = (esp_random() % (2 * amplitude + 1)) - amplitude;
        uint8_t r = CLAMP(base_color.r + r_offset, 0, 255);
        uint8_t g = CLAMP(base_color.g + g_offset, 0, 255);
        uint8_t b = CLAMP(base_color.b + b_offset, 0, 255);
        for (int i = 0; i < LED_NUMBERS; i++) {
            led_spi_set_pixel(i, g, r, b);
        }
        led_spi_show();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // 最后回到基色
    for (int i = 0; i < LED_NUMBERS; i++) {
        led_spi_set_pixel(i, base_color.g, base_color.r, base_color.b);
    }
    led_spi_show();
    ESP_LOGI(TAG, "Type0 Style3 (lightsaber pulse) completed");
}

// type 1: 流水跑马灯（保留原实现）
static void led_effect_type1(LedDataConfig_t data) {
    if (data.cycles <= 0) data.cycles = 1;
    if (data.duration <= 0) data.duration = 1000;
    int cycle_duration = data.duration / data.cycles;
    int sum = (data.pram.sum < 1) ? 3 : data.pram.sum;
    sum = (sum > LED_NUMBERS) ? LED_NUMBERS : sum;
    int order = (data.order != 0 && data.order != 1) ? 0 : data.order;
    int style = (data.style != 0 && data.style != 1) ? 0 : data.style;
    RGB_t mid_color = {
        .r = (data.color_st.r + data.color_ed.r) / 2,
        .g = (data.color_st.g + data.color_ed.g) / 2,
        .b = (data.color_st.b + data.color_ed.b) / 2
    };

    // 初始化为起始色
    for (int i = 0; i < LED_NUMBERS; i++) {
        led_spi_set_pixel(i, data.color_st.g, data.color_st.r, data.color_st.b);
    }
    led_spi_show();

    int retain_start = (order == 0) ? LED_NUMBERS : -1;
    int retain_end = (order == 0) ? LED_NUMBERS : -1;

    for (int cycle = 0; cycle < data.cycles; cycle++) {
        if (g_config_changed) return;
        int current_end_pos;
        if (style == 1) {
            if (order == 0) {
                current_end_pos = LED_NUMBERS - 1 - cycle;
                current_end_pos = (current_end_pos < 0) ? 0 : current_end_pos;
            } else {
                current_end_pos = cycle;
                current_end_pos = (current_end_pos >= LED_NUMBERS) ? LED_NUMBERS - 1 : current_end_pos;
            }
        } else {
            current_end_pos = (order == 0) ? LED_NUMBERS - 1 : 0;
        }

        if (style == 1) {
            if (order == 0) {
                retain_start = current_end_pos;
                retain_end = LED_NUMBERS - 1;
            } else {
                retain_start = 0;
                retain_end = current_end_pos;
            }
        }

        int flow_steps = (order == 0) ? (current_end_pos + 1) : (LED_NUMBERS - current_end_pos);
        flow_steps = (flow_steps < 1) ? 1 : flow_steps;
        int step_duration = cycle_duration / flow_steps;
        step_duration = (step_duration < 1) ? 1 : step_duration;

        for (int step = 0; step < flow_steps; step++) {
            if (g_config_changed) return;
            if (s_led_paused) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
            int center_pos;
            if (order == 0) center_pos = step;
            else center_pos = LED_NUMBERS - 1 - step;

            if (style == 0) {
                for (int i = 0; i < LED_NUMBERS; i++) {
                    led_spi_set_pixel(i, data.color_st.g, data.color_st.r, data.color_st.b);
                }
            } else {
                for (int i = retain_start; i <= retain_end; i++) {
                    if (i < 0 || i >= LED_NUMBERS) continue;
                    led_spi_set_pixel(i, data.color_ed.g, data.color_ed.r, data.color_ed.b);
                }
            }

            for (int offset = -(sum/2); offset <= sum/2; offset++) {
                int pixel_idx = center_pos + offset;
                if (pixel_idx < 0 || pixel_idx >= LED_NUMBERS) continue;
                if (style == 1) {
                    if ((order == 0 && pixel_idx >= retain_start) || (order == 1 && pixel_idx <= retain_end))
                        continue;
                }
                RGB_t draw_color = (offset == 0) ? data.color_ed : mid_color;
                led_spi_set_pixel(pixel_idx, draw_color.g, draw_color.r, draw_color.b);
            }
            led_spi_show();
            vTaskDelay(pdMS_TO_TICKS(step_duration));
        }
    }

    if (style == 1) {
        for (int i = 0; i < LED_NUMBERS; i++) {
            led_spi_set_pixel(i, data.color_ed.g, data.color_ed.r, data.color_ed.b);
        }
        led_spi_show();
    }
    ESP_LOGI(TAG, "Type1 completed");
}

// LED 主任务
static void led_task(void *pvParameters) {
    const char *default_json = "{\"type\":1,\"data\":{\"duration\":20000,\"cycles\":60,\"color_st\":[255,255,255],\"color_ed\":[210,0,210],\"style\":1,\"order\":0,\"pram\":{\"sum\":3}}}";
    led_update_config_from_json(default_json);
    led_spi_init();

    while (1) {
        if (s_led_paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (g_config_changed) {
            g_config_changed = false;
            ESP_LOGI(TAG, "Config changed, restart effect");
        }
        LedEffectConfig_t current_config;
        if (g_config_mutex) {
            xSemaphoreTake(g_config_mutex, portMAX_DELAY);
            current_config = g_led_config;
            xSemaphoreGive(g_config_mutex);
        } else {
            current_config = parse_led_effect_json(default_json);
        }
        if (current_config.type == 0) {
            switch (current_config.data.style) {
                case 0: led_effect_type0_style0(current_config.data); break;
                case 1: led_effect_type0_style1(current_config.data); break;
                case 2: led_effect_type0_style2(current_config.data); break;
                case 3: led_effect_type0_style3(current_config.data); break;
                default: led_effect_type0_style0(current_config.data); break;
            }
        } else if (current_config.type == 1) {
            led_effect_type1(current_config.data);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void led_init(void) {
    g_config_mutex = xSemaphoreCreateMutex();
    BaseType_t ret = xTaskCreatePinnedToCore(led_task, "led_task", 6 * 1024, NULL, 4, &led_task_handle, 1);
    if (ret != pdPASS) ESP_LOGE(TAG, "Failed to create led task");
    else ESP_LOGI(TAG, "LED effect task started (SPI+DMA)");
}