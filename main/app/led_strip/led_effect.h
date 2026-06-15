#ifndef __LED_EFFECT_H__
#define __LED_EFFECT_H__

#include <stdbool.h>

void led_init(void);
void led_set_paused(bool paused);
void led_update_config_from_json(const char *json_str);

#endif