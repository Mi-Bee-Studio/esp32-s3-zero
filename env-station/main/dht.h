#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool ok;          /* true = 本次数值有效 */
    float t_c;        /* 温度 ℃ */
    float rh;         /* 相对湿度 % */
    int model;        /* 11 或 22（自动分辨） */
    uint8_t raw[5];   /* 原始 5 字节（排障用） */
    int n_highs;      /* 捕获到的高电平数（排障用） */
    int n_sym;        /* RX 捕获的符号总数（排障用；0=总线无任何边沿） */
} dht_reading_t;

/* err 码：0=ok 1=超时 2=校验和 3=位数不足 */
esp_err_t dht_init(int gpio_num);
esp_err_t dht_retarget(int gpio_num); /* 换脚重建通道（自动扫描用） */
bool dht_read(dht_reading_t *out, int *err);
const char *dht_err_str(int err);
