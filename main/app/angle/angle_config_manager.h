#ifndef ANGLE_CONFIG_MANAGER_H
#define ANGLE_CONFIG_MANAGER_H

#include "esp_err.h"

/**
 * @brief 初始化角度配置管理器（加载 setting.json，创建任务）
 */
esp_err_t angle_config_manager_init(void);

/**
 * @brief 保存配置到指定角度区间
 * @param pitch 俯仰角（度）
 * @param roll  横滚角（度）
 * @param config_json 灯光配置 JSON 字符串（不含 type 包裹，直接是 {"type":...,"data":...}）
 */
void angle_config_save_for_angle(float pitch, float roll, const char *config_json);

#endif