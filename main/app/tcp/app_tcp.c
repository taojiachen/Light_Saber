#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#include "audio.h"
#include "led_effect.h"

#define TCP_PORT 8080
#define MAX_CONN_QUEUE 5
#define BUF_SIZE 1024
#define JSON_BUF_SIZE 4096
#define AUDIO_BUF_SIZE 65536
#define SPIFFS_PART_LABEL "storage"
#define SETTING_FILE_PATH "/spiffs/setting.json"
#define LOG_MAX_BYTES 256
#define PREFIX_MAX_LEN 32
#define RECV_TIMEOUT_SEC 10

static const char *TAG = "TCP_SERVER";

typedef enum {
    DATA_TYPE_UNKNOWN,
    DATA_TYPE_JSON,
    DATA_TYPE_MUSIC
} data_type_t;

static int current_client_sock = -1;
static TaskHandle_t key_task_handle = NULL;
static SemaphoreHandle_t client_sock_mutex = NULL;

// 函数声明
static void print_raw_data(const char *data, int len);
static esp_err_t spiffs_init(void);
static esp_err_t save_setting_json(cJSON *json);
static esp_err_t load_last_music(char *music_buf, size_t buf_len);
static data_type_t parse_prefix(const char *prefix_buf, int prefix_len, int *data_length);
static void handle_json_data(const char *json_str, char *music_name, size_t music_name_len);
static esp_err_t save_audio_data(const char *music_name, const char *audio_data, int audio_len);
static void client_handler_task(void *pvParameters);
static void tcp_server_task(void *pvParameters);

void tcp_register_key_task(TaskHandle_t handle)
{
    key_task_handle = handle;
    ESP_LOGI(TAG, "Key task registered, handle=%p", handle);
}

int tcp_send_data(const char *data, int len)
{
    int sock = -1;
    if (client_sock_mutex) {
        xSemaphoreTake(client_sock_mutex, portMAX_DELAY);
        sock = current_client_sock;
        xSemaphoreGive(client_sock_mutex);
    } else {
        sock = current_client_sock;
    }
    if (sock < 0) {
        ESP_LOGE(TAG, "No active client");
        return -1;
    }
    int ret = send(sock, data, len, 0);
    if (ret < 0) {
        ESP_LOGE(TAG, "Send failed: errno %d", errno);
    } else {
        ESP_LOGI(TAG, "Sent %d bytes", ret);
    }
    return ret;
}

static void print_raw_data(const char *data, int len)
{
    if (len <= 0 || !data) return;
    ESP_LOGI(TAG, "Received raw data: len=%d bytes", len);
    char hex_buf[LOG_MAX_BYTES * 3 + 1] = {0};
    int print_len = MIN(len, LOG_MAX_BYTES);
    for (int i = 0; i < print_len; i++) {
        snprintf(hex_buf + i * 3, 4, "%02X ", (unsigned char)data[i]);
    }
    if (len > LOG_MAX_BYTES) strcat(hex_buf, "...");
    ESP_LOGI(TAG, "Hex: %s", hex_buf);
    char str_buf[LOG_MAX_BYTES + 1] = {0};
    for (int i = 0; i < print_len; i++) {
        str_buf[i] = (data[i] >= 0x20 && data[i] <= 0x7E) ? data[i] : '.';
    }
    if (len > LOG_MAX_BYTES) strcat(str_buf, "...");
    ESP_LOGI(TAG, "String: %s", str_buf);
}

static esp_err_t spiffs_init(void)
{
    // 避免重复挂载
    FILE *test = fopen("/spiffs/test", "r");
    if (test) {
        fclose(test);
        ESP_LOGI(TAG, "SPIFFS already mounted");
        return ESP_OK;
    }
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = SPIFFS_PART_LABEL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(SPIFFS_PART_LABEL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS Total: %d KB, Used: %d KB", total / 1024, used / 1024);
    }
    return ESP_OK;
}

static esp_err_t save_setting_json(cJSON *json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(SETTING_FILE_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open setting file for write");
        return ESP_FAIL;
    }
    char *json_str = cJSON_Print(json);
    if (!json_str) {
        fclose(f);
        return ESP_FAIL;
    }
    fprintf(f, "%s", json_str);
    fclose(f);
    cJSON_free(json_str);
    ESP_LOGI(TAG, "Setting saved");
    return ESP_OK;
}

static esp_err_t load_last_music(char *music_buf, size_t buf_len)
{
    FILE *f = fopen(SETTING_FILE_PATH, "r");
    if (!f) return ESP_FAIL;
    static char file_buf[BUF_SIZE] = {0};
    fread(file_buf, 1, sizeof(file_buf) - 1, f);
    fclose(f);
    cJSON *json = cJSON_Parse(file_buf);
    if (!json) return ESP_FAIL;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (data && cJSON_IsObject(data)) {
        cJSON *music = cJSON_GetObjectItemCaseSensitive(data, "music");
        if (cJSON_IsString(music) && music->valuestring) {
            strlcpy(music_buf, music->valuestring, buf_len);
            cJSON_Delete(json);
            return ESP_OK;
        }
    }
    cJSON_Delete(json);
    return ESP_FAIL;
}

static data_type_t parse_prefix(const char *prefix_buf, int prefix_len, int *data_length)
{
    if (!prefix_buf || prefix_len < 8 || !data_length) return DATA_TYPE_UNKNOWN;
    char *colon1 = strchr(prefix_buf, ':');
    if (!colon1) return DATA_TYPE_UNKNOWN;
    int type_len = colon1 - prefix_buf;
    char type_str[8] = {0};
    strncpy(type_str, prefix_buf, type_len);
    char *colon2 = strchr(colon1 + 1, ':');
    if (!colon2) return DATA_TYPE_UNKNOWN;
    int len_str_len = colon2 - (colon1 + 1);
    if (len_str_len < 1 || len_str_len > 8) return DATA_TYPE_UNKNOWN;
    char len_str[16] = {0};
    strncpy(len_str, colon1 + 1, len_str_len);
    *data_length = atoi(len_str);
    if (strcmp(type_str, "JSON") == 0) return DATA_TYPE_JSON;
    if (strcmp(type_str, "MUSIC") == 0) return DATA_TYPE_MUSIC;
    return DATA_TYPE_UNKNOWN;
}

static void handle_json_data(const char *json_str, char *music_name, size_t music_name_len)
{
    if (!json_str || !music_name) return;
    ESP_LOGI(TAG, "Parsing JSON: %s", json_str);
    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        ESP_LOGE(TAG, "JSON parse error");
        return;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (!data || !cJSON_IsObject(data)) {
        cJSON_Delete(json);
        return;
    }
    cJSON *music = cJSON_GetObjectItemCaseSensitive(data, "music");
    if (music && cJSON_IsString(music) && music->valuestring && strlen(music->valuestring) > 0) {
        strlcpy(music_name, music->valuestring, music_name_len);
    } else {
        load_last_music(music_name, music_name_len);
        cJSON_ReplaceItemInObjectCaseSensitive(data, "music", cJSON_CreateString(music_name));
    }
    save_setting_json(json);
    led_update_config_from_json(json_str);
    cJSON_Delete(json);
    ESP_LOGI(TAG, "JSON handled, music=%s", music_name);
}

static esp_err_t save_audio_data(const char *music_name, const char *audio_data, int audio_len)
{
    if (!music_name || strlen(music_name) == 0 || !audio_data || audio_len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char filepath[64] = {0};
    snprintf(filepath, sizeof(filepath), "/spiffs/%s.mp3", music_name);
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open audio file: %s", filepath);
        return ESP_FAIL;
    }
    size_t written = fwrite(audio_data, 1, audio_len, f);
    fclose(f);
    if (written != audio_len) {
        ESP_LOGE(TAG, "Write incomplete (%d/%d)", written, audio_len);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Audio saved: %s (%d bytes)", filepath, audio_len);
    return ESP_OK;
}

// 简化版 client_handler_task
static void client_handler_task(void *pvParameters)
{
    int client_sock = *(int *)pvParameters;
    free(pvParameters);
    current_client_sock = client_sock;  // 直接赋值，不加锁
    printf("=== client task started, sock=%d ===\n", client_sock);

    char music_name[32] = {0};
    char recv_buf[2048];
    int total = 0;

    while (1) {
        int len = recv(client_sock, recv_buf + total, sizeof(recv_buf) - total - 1, 0);
        if (len <= 0) {
            printf("recv error or closed\n");
            break;
        }
        total += len;
        recv_buf[total] = '\0';
        // 检查是否收到完整数据（包含两个冒号）
        char *colon1 = strchr(recv_buf, ':');
        char *colon2 = colon1 ? strchr(colon1+1, ':') : NULL;
        if (colon2) {
            int len_prefix = colon2 - recv_buf + 1;
            int data_len = atoi(colon1+1);
            if (total >= len_prefix + data_len) {
                // 处理数据
                char *json_start = colon2 + 1;
                printf("Received JSON: %s\n", json_start);
                handle_json_data(json_start, music_name, sizeof(music_name));
                if (key_task_handle) xTaskNotifyGive(key_task_handle);
                // 清空缓冲区，准备下一个
                total = 0;
            }
        }
        vTaskDelay(10);
    }
    close(client_sock);
    current_client_sock = -1;
    vTaskDelete(NULL);
}

// 替换 tcp_server_task 为如下实现
static void tcp_server_task(void *pvParameters)
{
    int server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock < 0) { ESP_LOGE(TAG, "socket failed"); return; }
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(TCP_PORT)
    };
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "bind failed"); close(server_sock); return;
    }
    if (listen(server_sock, MAX_CONN_QUEUE) != 0) {
        ESP_LOGE(TAG, "listen failed"); close(server_sock); return;
    }
    ESP_LOGI(TAG, "TCP Server started on port %d", TCP_PORT);
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &len);
        if (client_sock < 0) continue;
        ESP_LOGI(TAG, "New client: %s:%d", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        current_client_sock = client_sock;  // 无需锁，因为只有一个任务处理

        // 直接在循环中处理该客户端（不创建新任务）
        char recv_buf[4096];
        int total = 0;
        char music_name[32] = {0};
        while (1) {
            int r = recv(client_sock, recv_buf + total, sizeof(recv_buf)-total-1, 0);
            if (r <= 0) break;
            total += r;
            recv_buf[total] = '\0';
            // 查找前缀 JSON:xxxxxx: 或 MUSIC:xxxxxx:
            char *colon1 = strchr(recv_buf, ':');
            if (!colon1) continue;
            char *colon2 = strchr(colon1+1, ':');
            if (!colon2) continue;
            int data_len = atoi(colon1+1);
            int prefix_len = colon2 - recv_buf + 1;
            if (total >= prefix_len + data_len) {
                char *data_start = colon2+1;
                if (strncmp(recv_buf, "JSON", 4) == 0) {
                    handle_json_data(data_start, music_name, sizeof(music_name));
                    if (key_task_handle) xTaskNotifyGive(key_task_handle);
                } else if (strncmp(recv_buf, "MUSIC", 5) == 0) {
                    save_audio_data(music_name, data_start, data_len);
                    if (key_task_handle) xTaskNotifyGive(key_task_handle);
                }
                // 移除已处理数据
                int remain = total - (prefix_len + data_len);
                if (remain > 0) memmove(recv_buf, recv_buf+prefix_len+data_len, remain);
                total = remain;
            }
            if (total >= (int)sizeof(recv_buf)-1) total = 0;
        }
        close(client_sock);
        current_client_sock = -1;
        ESP_LOGI(TAG, "Client disconnected");
    }
}

void tcp_server_init(void)
{
    client_sock_mutex = xSemaphoreCreateMutex();
    spiffs_init();   // 确保 SPIFFS 已挂载
    xTaskCreate(tcp_server_task, "tcp_server", 6 * 1024, NULL, 4, NULL);
}