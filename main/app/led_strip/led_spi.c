#include "led_spi.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "LED_SPI";

// SPI 配置：每个原始 bit 用 1 个 SPI 字节表示，码值 0x80 (10000000b) 和 0xE0 (11100000b)
// SPI 时钟 3.2MHz -> 每个 SPI 时钟 312.5ns，0x80 高电平 312.5ns，0xE0 高电平 937.5ns，符合 WS2812 时序
#define SPI_CLOCK_SPEED_HZ   (3200 * 1000)   // 3.2 MHz

#define BYTES_PER_LED        24              // 每个 LED 需要 24 个 SPI 字节（24 bit * 1 字节/bit）

static spi_device_handle_t spi_handle = NULL;
static uint8_t *color_buffer = NULL;         // 原始颜色缓冲区 (GRB 顺序，3 * LED_NUMBERS 字节)
static uint8_t *spi_buffer = NULL;           // 位扩展后的 SPI 缓冲区 (24 * LED_NUMBERS 字节)

// 构建 SPI 发送数据：将 GRB 每个 bit 转换为 0x80 或 0xE0
static void build_spi_buffer(void) {
    uint8_t *out = spi_buffer;
    for (uint16_t i = 0; i < LED_NUMBERS; i++) {
        uint8_t g = color_buffer[i * 3 + 0];
        uint8_t r = color_buffer[i * 3 + 1];
        uint8_t b = color_buffer[i * 3 + 2];
        uint8_t channels[3] = {g, r, b};
        for (int ch = 0; ch < 3; ch++) {
            uint8_t byte = channels[ch];
            for (int bit = 7; bit >= 0; bit--) {
                *out++ = (byte >> bit) & 0x01 ? 0xE0 : 0x80;
            }
        }
    }
}

void led_spi_init(void) {
    // 分配内存（必须使用 DMA 内存）
    color_buffer = heap_caps_malloc(LED_NUMBERS * 3, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    spi_buffer   = heap_caps_malloc(LED_NUMBERS * BYTES_PER_LED, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!color_buffer || !spi_buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return;
    }
    memset(color_buffer, 0, LED_NUMBERS * 3);
    memset(spi_buffer, 0, LED_NUMBERS * BYTES_PER_LED);

    // 初始化 SPI 总线
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = LED_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LED_NUMBERS * BYTES_PER_LED,
    };
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = SPI_CLOCK_SPEED_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &spi_handle));
    ESP_LOGI(TAG, "SPI LED driver initialized, %d LEDs", LED_NUMBERS);
}

void led_spi_set_pixel(uint16_t index, uint8_t g, uint8_t r, uint8_t b) {
    if (index >= LED_NUMBERS || !color_buffer) return;
    color_buffer[index * 3 + 0] = g;
    color_buffer[index * 3 + 1] = r;
    color_buffer[index * 3 + 2] = b;
}

void led_spi_set_all(const uint8_t *grb_data) {
    if (!color_buffer || !grb_data) return;
    memcpy(color_buffer, grb_data, LED_NUMBERS * 3);
}

void led_spi_show(void) {
    if (!spi_handle || !color_buffer || !spi_buffer) return;
    build_spi_buffer();
    spi_transaction_t trans = {
        .length = LED_NUMBERS * BYTES_PER_LED * 8,
        .tx_buffer = spi_buffer,
        .rx_buffer = NULL,
    };
    esp_err_t ret = spi_device_transmit(spi_handle, &trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI transmit failed: %s", esp_err_to_name(ret));
    }
}

void led_spi_clear(void) {
    if (color_buffer) {
        memset(color_buffer, 0, LED_NUMBERS * 3);
        led_spi_show();
    }
}

uint8_t* led_spi_get_buffer(void) {
    return color_buffer;
}