#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
/* --------------------------------------------- */
#include "esp_board_init.h"
#include <app_tcp.h>
#include <app_wifi_config.h>
#include "audio.h"
#include "led_effect.h"
#include "app_key.h"
#include "imu.h"
#include "websocket.h"
#include "state_machine.h"
#include "app_sr.h"
#include "angle_config_manager.h"
#include "app_aliyun_mqtt.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(gpio_install_isr_service(3));
    esp_board_init();
    imu_init();
    imu_start_task();
    button_init();
    led_init();
    state_machine_init();
    wifi_init();
    app_aliyun_mqtt_init();
    ESP_ERROR_CHECK(app_sr_start());
    angle_config_manager_init();
    audio_init();
    tcp_server_init();
    // ws_start("wss://10.234.89.44:8765");
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    while (1)
    {
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "Free memory after start: %d bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        ESP_LOGI(TAG, "Free PSRAM heap: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
