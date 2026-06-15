#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "cJSON.h"

#include "websocket.h"
#include "audio.h"
#include "led_effect.h"
#include "angle_config_manager.h"

#include "esp_attr.h"

static const char *TAG = "WS_CLIENT";

// 内置默认配置参数
#define WS_DEFAULT_PING_INTERVAL 0
#define WS_DEFAULT_BUFFER_SIZE (8 * 1024)
#define WS_DEFAULT_NETWORK_TIMEOUT 20000
#define WS_DEFAULT_MAX_RECONNECT 15
#define WS_DEFAULT_INIT_RECONNECT_DELAY 2000
#define WS_DEFAULT_MAX_RECONNECT_DELAY 30000
#define WS_DEFAULT_SKIP_CERT_CHECK true
#define MAX_OPUS_FRAME_LEN 200

extern float g_key_press_pitch;
extern float g_key_press_roll;
extern bool g_key_pressed_flag;

// WebSocket配置结构体
typedef struct
{
    const char *uri;
    const char *cert_pem;
    int ping_interval_sec;
    int buffer_size;
    int network_timeout_ms;
    int max_reconnect_attempts;
    int initial_reconnect_delay_ms;
    int max_reconnect_delay_ms;
    bool skip_cert_check;
} ws_config_t;

// WebSocket客户端上下文
typedef struct
{
    esp_websocket_client_handle_t client;
    ws_config_t config;
    ws_state_t state;

    ws_data_handler_t data_handler;
    ws_state_handler_t state_handler;

    // 重连相关
    TaskHandle_t reconnect_task;
    TimerHandle_t reconnect_timer;
    int reconnect_attempts;
    int reconnect_success;
    int current_delay;
    bool manual_disconnect;

    SemaphoreHandle_t mutex;
    bool initialized;
} ws_context_t;

static ws_context_t g_ws_ctx = {0};

TaskHandle_t ws_send_opus_task_handle = NULL;
extern QueueHandle_t ws_send_queue;

EXT_RAM_BSS_ATTR uint8_t opus_data_buf[MAX_OPUS_FRAME_LEN] = {0};

// 前向声明
static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void ws_reconnect_task(void *pvParameters);
static void ws_reconnect_timer_cb(TimerHandle_t timer);
static void ws_set_state(ws_state_t new_state);
static esp_err_t ws_create_client(void);
static void ws_destroy_client(void);
static void ws_start_reconnect_timer(void);
static void ws_stop_reconnect_timer(void);
static void ws_send_device_info(void);
static esp_err_t ws_init_internal(const ws_config_t *config);
static void ws_send_opus_task(void *pvParameters); // 改为 void 类型

// ==================== 状态设置 ====================
static void ws_set_state(ws_state_t new_state)
{
    if (xSemaphoreTake(g_ws_ctx.mutex, portMAX_DELAY) == pdTRUE)
    {
        ws_state_t old_state = g_ws_ctx.state;
        g_ws_ctx.state = new_state;
        xSemaphoreGive(g_ws_ctx.mutex);

        if (old_state != new_state && g_ws_ctx.state_handler)
        {
            g_ws_ctx.state_handler(old_state, new_state);
        }
        ESP_LOGI(TAG, "状态变化: %d -> %d", old_state, new_state);
    }
}

// ==================== 发送设备信息 ====================
static void ws_send_device_info(void)
{
    uint8_t mac_addr[6];
    char mac_str[18];

    if (esp_wifi_get_mac(WIFI_IF_STA, mac_addr) == ESP_OK)
    {
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
    }
    else
    {
        strcpy(mac_str, "unknown");
    }

    time_t now = time(NULL);
    char device_info[128];
    snprintf(device_info, sizeof(device_info),
             "{\"type\":\"device_info\",\"mac\":\"%s\",\"timestamp\":%lld}",
             mac_str, (long long)now);

    ws_send_text(device_info, strlen(device_info));
}

// ==================== WebSocket 事件处理器 ====================
static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket连接成功");
        ws_set_state(WS_STATE_CONNECTED);

        if (xSemaphoreTake(g_ws_ctx.mutex, portMAX_DELAY) == pdTRUE)
        {
            g_ws_ctx.current_delay = g_ws_ctx.config.initial_reconnect_delay_ms;
            if (g_ws_ctx.reconnect_attempts > 0)
                g_ws_ctx.reconnect_success++;
            g_ws_ctx.reconnect_attempts = 0;
            xSemaphoreGive(g_ws_ctx.mutex);
        }

        ws_stop_reconnect_timer();
        ws_send_device_info();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket连接断开");

        bool manual = false;
        if (xSemaphoreTake(g_ws_ctx.mutex, portMAX_DELAY) == pdTRUE)
        {
            manual = g_ws_ctx.manual_disconnect;
            xSemaphoreGive(g_ws_ctx.mutex);
        }

        if (!manual)
        {
            ws_set_state(WS_STATE_RECONNECTING);
            // ws_start_reconnect_timer();
        }
        else
        {
            ws_set_state(WS_STATE_DISCONNECTED);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 2) // 二进制音频数据
        {
            ESP_LOGI(TAG, "收到二进制数据，长度: %d", data->data_len);
            if (g_ws_ctx.data_handler)
                g_ws_ctx.data_handler(data->data_ptr, data->data_len);
            else
                ESP_LOGW(TAG, "未注册数据处理回调");
        }
        else if (data->op_code == 1) // 文本命令
        {
            ESP_LOGI(TAG, "收到文本数据: %.*s", data->data_len, (char *)data->data_ptr);
            cJSON *json = cJSON_Parse((char *)data->data_ptr);
            if (json)
            {
                cJSON *type = cJSON_GetObjectItem(json, "type");
                if (type && cJSON_IsString(type))
                {
                    const char *type_str = type->valuestring;

                    // 音频流控制（服务器下发的）
                    if (strcmp(type_str, "audio_start") == 0)
                        audio_start_event();
                    else if (strcmp(type_str, "audio_end") == 0)
                        audio_end_event();

                    // 可选：服务器端对录音开始/结束的确认（单片机可忽略）
                    else if (strcmp(type_str, "recording_start") == 0 ||
                             strcmp(type_str, "recording_end") == 0)
                    {
                        ESP_LOGI(TAG, "服务器确认录音状态: %s", type_str);
                    }
                }
                else if (type && cJSON_IsNumber(type))
                {
                    // 服务器下发光剑控制 JSON（格式如 {"type":0,"data":{...}}）
                    int cmd_type = type->valueint;
                    ESP_LOGI(TAG, "收到光剑控制命令 type=%d", cmd_type);
                    // 如果是由按键触发的录音返回的控制命令，则保存到角度配置
                    if (g_key_pressed_flag)
                    {
                        // 将整个 JSON 对象转为字符串（不格式化，节省内存）
                        char *config_str = cJSON_PrintUnformatted(json);
                        if (config_str)
                        {
                            angle_config_save_for_angle(g_key_press_pitch, g_key_press_roll, config_str);
                            free(config_str);
                        }
                        g_key_pressed_flag = false; // 清除标志，避免重复保存
                    }
                    else
                    {
                        // 直接更新 LED（不保存）
                        led_update_config_from_json((char *)data->data_ptr);
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "未知JSON格式，忽略");
                }
                cJSON_Delete(json);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket错误");
        if (data && data->error_handle.error_type != WEBSOCKET_ERROR_TYPE_NONE)
        {
            ESP_LOGE(TAG, "错误类型: %d, ESP错误: %s",
                     data->error_handle.error_type,
                     esp_err_to_name(data->error_handle.esp_tls_last_esp_err));
            if (data->error_handle.esp_transport_sock_errno != 0)
            {
                ESP_LOGE(TAG, "Socket错误: %d (%s)",
                         data->error_handle.esp_transport_sock_errno,
                         strerror(data->error_handle.esp_transport_sock_errno));
            }
        }
        ws_set_state(WS_STATE_ERROR);
        // ws_start_reconnect_timer();
        break;

    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "WebSocket连接已关闭");
        break;

    case WEBSOCKET_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "准备建立WebSocket连接");
        ws_set_state(WS_STATE_CONNECTING);
        break;

    default:
        ESP_LOGD(TAG, "未知事件: %d", (int)event_id);
        break;
    }
}

// ==================== 重连定时器回调 ====================
static void ws_reconnect_timer_cb(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "重连定时器触发");
    if (xTaskCreate(ws_reconnect_task, "ws_reconnect", 4096, NULL, 4, &g_ws_ctx.reconnect_task) != pdPASS)
    {
        ESP_LOGE(TAG, "创建重连任务失败");
        ws_start_reconnect_timer();
    }
}

// ==================== 重连任务 ====================
static void ws_reconnect_task(void *pvParameters)
{
    const int check_interval_ms = 2000; // 每2秒检查一次
    while (1)
    {
        if (!ws_is_connected() && !g_ws_ctx.manual_disconnect)
        {
            ESP_LOGI(TAG, "检测到连接断开，尝试重连...");
            ws_destroy_client();
            vTaskDelay(pdMS_TO_TICKS(500));
            if (ws_create_client() == ESP_OK)
            {
                if (esp_websocket_client_start(g_ws_ctx.client) == ESP_OK)
                {
                    ESP_LOGI(TAG, "重连启动成功");
                }
                else
                {
                    ESP_LOGE(TAG, "重连启动失败");
                }
            }
            else
            {
                ESP_LOGE(TAG, "创建客户端失败");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
    }
}
// ==================== 重连定时器控制 ====================
static void ws_start_reconnect_timer(void)
{
    if (g_ws_ctx.reconnect_timer && xTimerIsTimerActive(g_ws_ctx.reconnect_timer) == pdFALSE)
    {
        int delay = g_ws_ctx.current_delay;
        g_ws_ctx.current_delay = (g_ws_ctx.current_delay * 3) / 2;
        if (g_ws_ctx.current_delay > g_ws_ctx.config.max_reconnect_delay_ms)
            g_ws_ctx.current_delay = g_ws_ctx.config.max_reconnect_delay_ms;

        delay += (rand() % 1000) - 500;
        if (delay < 1000)
            delay = 1000;

        ESP_LOGI(TAG, "启动重连定时器，延迟: %d ms", delay);
        xTimerChangePeriod(g_ws_ctx.reconnect_timer, pdMS_TO_TICKS(delay), 0);
        xTimerStart(g_ws_ctx.reconnect_timer, 0);
    }
}

static void ws_stop_reconnect_timer(void)
{
    if (g_ws_ctx.reconnect_timer && xTimerIsTimerActive(g_ws_ctx.reconnect_timer) == pdTRUE)
    {
        xTimerStop(g_ws_ctx.reconnect_timer, 0);
        ESP_LOGI(TAG, "停止重连定时器");
    }
}

// ==================== WebSocket 客户端管理 ====================
static esp_err_t ws_create_client(void)
{
    if (g_ws_ctx.client)
    {
        ESP_LOGW(TAG, "客户端已存在");
        return ESP_OK;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = g_ws_ctx.config.uri,
        .transport = WEBSOCKET_TRANSPORT_OVER_SSL,
        .cert_pem = g_ws_ctx.config.cert_pem,
        .skip_cert_common_name_check = g_ws_ctx.config.skip_cert_check,
        .disable_auto_reconnect = true,
        .task_prio = 3,
        .task_stack = 8192,
        .buffer_size = g_ws_ctx.config.buffer_size,
        .ping_interval_sec = g_ws_ctx.config.ping_interval_sec,
        .network_timeout_ms = g_ws_ctx.config.network_timeout_ms,
    };

    g_ws_ctx.client = esp_websocket_client_init(&ws_cfg);
    if (!g_ws_ctx.client)
    {
        ESP_LOGE(TAG, "创建WebSocket客户端失败");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_websocket_register_events(g_ws_ctx.client, WEBSOCKET_EVENT_ANY,
                                                  ws_event_handler, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "注册事件处理器失败: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(g_ws_ctx.client);
        g_ws_ctx.client = NULL;
        return ret;
    }
    return ESP_OK;
}

static void ws_destroy_client(void)
{
    if (g_ws_ctx.client)
    {
        esp_websocket_client_stop(g_ws_ctx.client);
        esp_websocket_client_destroy(g_ws_ctx.client);
        g_ws_ctx.client = NULL;
        ESP_LOGI(TAG, "WebSocket客户端已销毁");
    }
}

// ==================== 内部初始化 ====================
static esp_err_t ws_init_internal(const ws_config_t *config)
{
    if (g_ws_ctx.initialized)
    {
        ESP_LOGW(TAG, "WebSocket已经初始化");
        return ESP_OK;
    }
    if (!config || !config->uri)
    {
        ESP_LOGE(TAG, "无效的配置参数");
        return ESP_ERR_INVALID_ARG;
    }

    memset(&g_ws_ctx, 0, sizeof(g_ws_ctx));
    g_ws_ctx.config = *config;

    if (g_ws_ctx.config.ping_interval_sec <= 0)
        g_ws_ctx.config.ping_interval_sec = WS_DEFAULT_PING_INTERVAL;
    if (g_ws_ctx.config.buffer_size <= 0)
        g_ws_ctx.config.buffer_size = WS_DEFAULT_BUFFER_SIZE;
    if (g_ws_ctx.config.network_timeout_ms <= 0)
        g_ws_ctx.config.network_timeout_ms = WS_DEFAULT_NETWORK_TIMEOUT;
    if (g_ws_ctx.config.max_reconnect_attempts <= 0)
        g_ws_ctx.config.max_reconnect_attempts = WS_DEFAULT_MAX_RECONNECT;
    if (g_ws_ctx.config.initial_reconnect_delay_ms <= 0)
        g_ws_ctx.config.initial_reconnect_delay_ms = WS_DEFAULT_INIT_RECONNECT_DELAY;
    if (g_ws_ctx.config.max_reconnect_delay_ms <= 0)
        g_ws_ctx.config.max_reconnect_delay_ms = WS_DEFAULT_MAX_RECONNECT_DELAY;
    if (g_ws_ctx.config.cert_pem == NULL)
        g_ws_ctx.config.cert_pem = server_cert_pem;

    g_ws_ctx.current_delay = g_ws_ctx.config.initial_reconnect_delay_ms;
    g_ws_ctx.state = WS_STATE_DISCONNECTED;

    g_ws_ctx.mutex = xSemaphoreCreateMutex();
    if (!g_ws_ctx.mutex)
    {
        ESP_LOGE(TAG, "创建互斥锁失败");
        return ESP_FAIL;
    }

    BaseType_t ret = xTaskCreate(ws_reconnect_task, "ws_reconnect", 4096, NULL, 3, &g_ws_ctx.reconnect_task);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "创建重连任务失败");
        vSemaphoreDelete(g_ws_ctx.mutex);
        return ESP_FAIL;
    }

    g_ws_ctx.initialized = true;
    ESP_LOGI(TAG, "WebSocket客户端初始化成功");
    return ESP_OK;
}

// ==================== OPUS 发送任务（void 类型） ====================
static void ws_send_opus_task(void *pvParameters)
{
    uint16_t req_len = 0;
    while (1)
    {
        if (xQueueReceive(ws_send_queue, &req_len, portMAX_DELAY) == pdTRUE)
        {
            if (req_len > MAX_OPUS_FRAME_LEN)
            {
                ESP_LOGE(TAG, "请求读取长度超过缓冲区上限: req_len=%u, max=%u", req_len, MAX_OPUS_FRAME_LEN);
                req_len = 0;
                continue;
            }
            if (!ws_is_connected())
            {
                ESP_LOGE(TAG, "WebSocket未连接，跳过OPUS数据发送");
                req_len = 0;
                continue;
            }

            esp_err_t read_ret = audio_get_opus_encode_data(opus_data_buf, req_len);
            if (read_ret != ESP_OK)
            {
                ESP_LOGE(TAG, "读取OPUS数据失败: %s", esp_err_to_name(read_ret));
                req_len = 0;
                continue;
            }

            esp_err_t send_ret = ws_send_binary(opus_data_buf, req_len);
            if (send_ret != ESP_OK)
                ESP_LOGE(TAG, "发送OPUS数据失败: %s (长度=%u)", esp_err_to_name(send_ret), req_len);
            else
                ESP_LOGD(TAG, "成功发送OPUS数据，长度=%u字节", req_len);

            req_len = 0;
            memset(opus_data_buf, 0, req_len);
        }
        else
        {
            ESP_LOGW(TAG, "从ws_send_queue接收数据失败");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // 不会到达这里
}

// ==================== 对外接口 ====================
esp_err_t ws_start(const char *uri)
{
    if (uri == NULL || strlen(uri) == 0)
    {
        ESP_LOGE(TAG, "无效的WS地址");
        return ESP_ERR_INVALID_ARG;
    }

    ws_config_t default_config = {
        .uri = uri,
        .cert_pem = server_cert_pem,
        .ping_interval_sec = WS_DEFAULT_PING_INTERVAL,
        .buffer_size = WS_DEFAULT_BUFFER_SIZE,
        .network_timeout_ms = WS_DEFAULT_NETWORK_TIMEOUT,
        .max_reconnect_attempts = WS_DEFAULT_MAX_RECONNECT,
        .initial_reconnect_delay_ms = WS_DEFAULT_INIT_RECONNECT_DELAY,
        .max_reconnect_delay_ms = WS_DEFAULT_MAX_RECONNECT_DELAY,
        .skip_cert_check = WS_DEFAULT_SKIP_CERT_CHECK};

    esp_err_t ret = ws_init_internal(&default_config);
    if (ret != ESP_OK)
        return ret;

    if (xSemaphoreTake(g_ws_ctx.mutex, portMAX_DELAY) == pdTRUE)
    {
        g_ws_ctx.manual_disconnect = false;
        xSemaphoreGive(g_ws_ctx.mutex);
    }

    ret = ws_create_client();
    if (ret != ESP_OK)
        return ret;

    ret = esp_websocket_client_start(g_ws_ctx.client);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "启动WebSocket客户端失败: %s", esp_err_to_name(ret));
        ws_destroy_client();
        ws_start_reconnect_timer(); // 启动重连
        return ESP_OK;              // 返回成功，由重连机制处理
    }

    ws_register_data_handler(ws_recv_data_handler);
    ESP_LOGI(TAG, "WebSocket客户端启动成功，连接地址: %s", uri);

    if (ws_send_opus_task_handle == NULL)
    {
        xTaskCreatePinnedToCore(ws_send_opus_task, "ws_send_opus_task", 4096, NULL, 5, &ws_send_opus_task_handle, 1);
    }
    return ESP_OK;
}

void ws_stop(void)
{
    if (!g_ws_ctx.initialized)
        return;

    if (xSemaphoreTake(g_ws_ctx.mutex, portMAX_DELAY) == pdTRUE)
    {
        g_ws_ctx.manual_disconnect = true;
        xSemaphoreGive(g_ws_ctx.mutex);
    }

    // 删除重连任务
    if (g_ws_ctx.reconnect_task)
    {
        vTaskDelete(g_ws_ctx.reconnect_task);
        g_ws_ctx.reconnect_task = NULL;
    }

    ws_destroy_client();
    ws_set_state(WS_STATE_DISCONNECTED);
    ESP_LOGI(TAG, "WebSocket客户端已停止");
}

esp_err_t ws_send_text(const char *data, size_t len)
{
    if (!g_ws_ctx.initialized || !g_ws_ctx.client)
    {
        ESP_LOGE(TAG, "WebSocket未初始化或客户端不存在");
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || len == 0 || len > INT_MAX)
    {
        ESP_LOGE(TAG, "无效的数据参数");
        return ESP_ERR_INVALID_ARG;
    }
    if (!esp_websocket_client_is_connected(g_ws_ctx.client))
    {
        ESP_LOGE(TAG, "WebSocket未连接");
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_text(g_ws_ctx.client, data, (int)len, portMAX_DELAY);
    if (sent <= 0)
    {
        ESP_LOGE(TAG, "发送文本数据失败: %d", sent);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ws_send_binary(const void *data, size_t len)
{
    if (!g_ws_ctx.initialized || !g_ws_ctx.client)
    {
        ESP_LOGE(TAG, "WebSocket未初始化或客户端不存在");
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || len == 0 || len > INT_MAX)
    {
        ESP_LOGE(TAG, "无效的数据参数");
        return ESP_ERR_INVALID_ARG;
    }
    if (!esp_websocket_client_is_connected(g_ws_ctx.client))
    {
        ESP_LOGE(TAG, "WebSocket未连接");
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_bin(g_ws_ctx.client, (const char *)data, (int)len, portMAX_DELAY);
    if (sent <= 0)
    {
        ESP_LOGE(TAG, "发送二进制数据失败: %d", sent);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ws_send_jpeg_binary(uint8_t *jpg_data, size_t jpg_len)
{
    if (!g_ws_ctx.initialized || !g_ws_ctx.client)
    {
        ESP_LOGE(TAG, "WebSocket未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    if (!jpg_data || jpg_len == 0)
    {
        ESP_LOGE(TAG, "无效JPEG数据");
        return ESP_ERR_INVALID_ARG;
    }
    if (!esp_websocket_client_is_connected(g_ws_ctx.client))
    {
        ESP_LOGE(TAG, "WebSocket未连接");
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_bin(g_ws_ctx.client, (const char *)jpg_data, (int)jpg_len, portMAX_DELAY);
    if (sent <= 0)
    {
        ESP_LOGE(TAG, "发送JPEG二进制失败: %d", sent);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "JPEG二进制发送成功，大小: %zu 字节", jpg_len);
    return ESP_OK;
}

bool ws_is_connected(void)
{
    if (!g_ws_ctx.initialized || !g_ws_ctx.client)
        return false;
    return esp_websocket_client_is_connected(g_ws_ctx.client);
}

void ws_register_data_handler(ws_data_handler_t handler)
{
    g_ws_ctx.data_handler = handler;
    if (handler)
        ESP_LOGI(TAG, "数据接收回调已注册");
    else
        ESP_LOGW(TAG, "未注册数据处理回调");
}

void ws_register_state_handler(ws_state_handler_t handler)
{
    g_ws_ctx.state_handler = handler;
    ESP_LOGI(TAG, "状态变化回调已注册");
}

void ws_deinit(void)
{
    if (!g_ws_ctx.initialized)
        return;

    ws_stop();

    if (g_ws_ctx.reconnect_timer)
    {
        xTimerDelete(g_ws_ctx.reconnect_timer, portMAX_DELAY);
        g_ws_ctx.reconnect_timer = NULL;
    }

    if (g_ws_ctx.mutex)
    {
        vSemaphoreDelete(g_ws_ctx.mutex);
        g_ws_ctx.mutex = NULL;
    }

    g_ws_ctx.initialized = false;
    ESP_LOGI(TAG, "WebSocket客户端已反初始化");
}