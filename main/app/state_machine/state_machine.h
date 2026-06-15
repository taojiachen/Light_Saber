#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "esp_err.h"

// 事件类型：仅保留按键按下和松开
typedef enum {
    EVENT_NONE,
    EVENT_KEY_PRESS_DOWN,   // 按键按下
    EVENT_KEY_UP,           // 按键松开
} state_event_t;

// 状态机状态：只有空闲和录音中
typedef enum {
    STATE_IDLE,
    STATE_RECORDING,
} state_t;

/**
 * @brief 初始化状态机
 */
esp_err_t state_machine_init(void);

/**
 * @brief 发送事件给状态机
 */
void state_machine_send_event(state_event_t event);

/**
 * @brief 获取当前状态（可选，供外部调试）
 */
state_t state_machine_get_current_state(void);

#endif