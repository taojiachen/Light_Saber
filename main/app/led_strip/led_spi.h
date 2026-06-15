#ifndef __LED_SPI_H__
#define __LED_SPI_H__

#include <stdint.h>
#include <stdbool.h>

#define LED_NUMBERS     30      // 灯珠数量（与原来一致）
#define LED_GPIO        19      // MOSI 引脚（原 RMT 引脚）

// 初始化 SPI 总线
void led_spi_init(void);

// 设置单个 LED 颜色（索引从0开始，颜色顺序 GRB）
void led_spi_set_pixel(uint16_t index, uint8_t g, uint8_t r, uint8_t b);

// 批量设置所有 LED（数据格式：GRB GRB ...，长度 = LED_NUMBERS * 3）
void led_spi_set_all(const uint8_t *grb_data);

// 将当前缓冲区中的颜色刷新到灯带
void led_spi_show(void);

// 清空所有 LED（全灭）
void led_spi_clear(void);

// 获取内部颜色缓冲区指针（用于直接操作）
uint8_t* led_spi_get_buffer(void);

#endif