#pragma once
#include "driver/i2c_master.h"

/* 极简 SSD1306 128×64 I2C 驱动：帧缓冲 + 5×7 字库文本 + 基本图元，够环境小站用 */
esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, uint8_t addr);
void ssd1306_clear(void);
/* 在 (col, row) 处写字符串；scale=1 占 6×8 像素，scale=2 占 12×16 */
void ssd1306_text(int col, int row, const char *s, int scale);
/* 图元（v3.1 UI：状态带图标/运动条/阈值刻度用） */
void ssd1306_hline(int x0, int x1, int y);
void ssd1306_vline(int x, int y0, int y1);
void ssd1306_fill_rect(int x, int y, int w, int h);
void ssd1306_flush(void);
/* 面板级开关（屏保/唤醒；0xAE 关显示保持 GRAM，0xAF 恢复） */
void ssd1306_display_on(bool on);
