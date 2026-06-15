#include "angle_config_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_effect.h"
#include "imu.h"
#include <string.h>

#include "audio.h"

static const char *TAG = "ANGLE_CFG";

// 角度分箱边界
static const int PITCH_BINS[] = {-90, -60, -30, 0, 30, 60, 90};
static const int ROLL_BINS[]  = {-180, -150, -120, -90, -60, -30, 0, 30, 60, 90, 120, 150, 180};
#define PITCH_BIN_COUNT (sizeof(PITCH_BINS)/sizeof(PITCH_BINS[0]) - 1)
#define ROLL_BIN_COUNT  (sizeof(ROLL_BINS)/sizeof(ROLL_BINS[0]) - 1)

// 全局配置存储
static char *g_config_map[PITCH_BIN_COUNT][ROLL_BIN_COUNT] = {NULL};
static char *g_default_config = NULL;
static char g_current_active_config[1024] = {0};

#define CONFIG_FILE "/spiffs/setting.json"

static int get_pitch_index(float pitch) {
    for (int i = 0; i < PITCH_BIN_COUNT; i++) {
        if (pitch >= PITCH_BINS[i] && pitch <= PITCH_BINS[i+1])
            return i;
    }
    return (pitch < PITCH_BINS[0]) ? 0 : (PITCH_BIN_COUNT - 1);
}

static int get_roll_index(float roll) {
    for (int i = 0; i < ROLL_BIN_COUNT; i++) {
        if (roll >= ROLL_BINS[i] && roll <= ROLL_BINS[i+1])
            return i;
    }
    return (roll < ROLL_BINS[0]) ? 0 : (ROLL_BIN_COUNT - 1);
}

static esp_err_t load_config_from_file(void) {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        ESP_LOGW(TAG, "Config file not exist");
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_FAIL;

    cJSON *def = cJSON_GetObjectItem(root, "default");
    if (def) {
        if (g_default_config) free(g_default_config);
        g_default_config = cJSON_PrintUnformatted(def);
    }

    cJSON *zones = cJSON_GetObjectItem(root, "zones");
    if (zones) {
        cJSON *configs = cJSON_GetObjectItem(zones, "configs");
        if (configs && cJSON_IsArray(configs)) {
            for (int i = 0; i < PITCH_BIN_COUNT && i < cJSON_GetArraySize(configs); i++) {
                cJSON *row = cJSON_GetArrayItem(configs, i);
                if (row && cJSON_IsArray(row)) {
                    for (int j = 0; j < ROLL_BIN_COUNT && j < cJSON_GetArraySize(row); j++) {
                        cJSON *item = cJSON_GetArrayItem(row, j);
                        if (item && !cJSON_IsNull(item)) {
                            char *json_str = cJSON_PrintUnformatted(item);
                            if (json_str) {
                                if (g_config_map[i][j]) free(g_config_map[i][j]);
                                g_config_map[i][j] = json_str;
                            }
                        }
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded config from file");
    return ESP_OK;
}

static esp_err_t save_config_to_file(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    
    if (g_default_config) {
        cJSON *def = cJSON_Parse(g_default_config);
        if (def) cJSON_AddItemToObject(root, "default", def);
    } else {
        cJSON_AddNullToObject(root, "default");
    }
    
    cJSON *zones = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "zones", zones);
    
    cJSON *pitch_bins = cJSON_CreateIntArray(PITCH_BINS, sizeof(PITCH_BINS)/sizeof(int));
    cJSON_AddItemToObject(zones, "pitch_bins", pitch_bins);
    cJSON *roll_bins = cJSON_CreateIntArray(ROLL_BINS, sizeof(ROLL_BINS)/sizeof(int));
    cJSON_AddItemToObject(zones, "roll_bins", roll_bins);
    
    cJSON *configs = cJSON_CreateArray();
    cJSON_AddItemToObject(zones, "configs", configs);
    for (int i = 0; i < PITCH_BIN_COUNT; i++) {
        cJSON *row = cJSON_CreateArray();
        cJSON_AddItemToArray(configs, row);
        for (int j = 0; j < ROLL_BIN_COUNT; j++) {
            if (g_config_map[i][j]) {
                cJSON *cfg = cJSON_Parse(g_config_map[i][j]);
                if (cfg) cJSON_AddItemToArray(row, cfg);
                else cJSON_AddItemToArray(row, cJSON_CreateNull());
            } else {
                cJSON_AddItemToArray(row, cJSON_CreateNull());
            }
        }
    }
    
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_FAIL;
    
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) {
        free(json_str);
        return ESP_FAIL;
    }
    fprintf(f, "%s", json_str);
    fclose(f);
    free(json_str);
    ESP_LOGI(TAG, "Config saved");
    return ESP_OK;
}

static char* get_config_for_angle(float pitch, float roll) {
    int p_idx = get_pitch_index(pitch);
    int r_idx = get_roll_index(roll);
    if (g_config_map[p_idx][r_idx]) {
        return strdup(g_config_map[p_idx][r_idx]);
    }
    return g_default_config ? strdup(g_default_config) : NULL;
}

static void update_config_for_angle(float pitch, float roll, const char *config_json) {
    int p_idx = get_pitch_index(pitch);
    int r_idx = get_roll_index(roll);
    if (g_config_map[p_idx][r_idx]) free(g_config_map[p_idx][r_idx]);
    g_config_map[p_idx][r_idx] = strdup(config_json);
}

void angle_config_save_for_angle(float pitch, float roll, const char *config_json) {
    update_config_for_angle(pitch, roll, config_json);
    save_config_to_file();
    
    float cur_pitch, cur_roll;
    if (imu_get_angles(&cur_pitch, &cur_roll) == ESP_OK) {
        ESP_LOGI(TAG, "Current angles: pitch=%.1f roll=%.1f", cur_pitch, cur_roll);
        if (get_pitch_index(cur_pitch) == get_pitch_index(pitch) &&
            get_roll_index(cur_roll) == get_roll_index(roll)) {
            led_update_config_from_json(config_json);
            strncpy(g_current_active_config, config_json, sizeof(g_current_active_config));
        }
    }
}

static void angle_monitor_task(void *pv) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    float pitch = 0, roll = 0;
    while (1) {
        if (imu_get_angles(&pitch, &roll) == ESP_OK) {
            char *new_cfg = get_config_for_angle(pitch, roll);
            if (new_cfg && strcmp(new_cfg, g_current_active_config) != 0) {
                ESP_LOGI(TAG, "Switch config: pitch=%.1f roll=%.1f", pitch, roll);
                led_update_config_from_json(new_cfg);
                // audio_play_opus_file("dc");
                strncpy(g_current_active_config, new_cfg, sizeof(g_current_active_config));
            }
            free(new_cfg);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t angle_config_manager_init(void) {
    esp_err_t ret = load_config_from_file();
    if (ret != ESP_OK) {
        const char *default_json = "{\"type\":0,\"data\":{\"duration\":3000,\"cycles\":15,\"style\":1,\"color_st\":[255,0,0],\"color_ed\":[0,255,0],\"pram\":{\"surge_intensity\":70}}}";
        g_default_config = strdup(default_json);
        save_config_to_file();
    }
    xTaskCreate(angle_monitor_task, "angle_monitor", 4 * 1024, NULL, 5, NULL);
    return ESP_OK;
}