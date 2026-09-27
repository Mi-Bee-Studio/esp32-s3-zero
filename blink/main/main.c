/*
 * blink — esp32-s3-zero 基线工程。
 *
 * 每秒步进一种颜色的 WS2812（GPIO21），BOOT 键（GPIO0）手动换色；
 * 每 10 秒一条心跳日志（uptime/heap/PSRAM），供 serialtap 持续采集验证。
 * 颜色沿用本工作区状态语义：绿=正常，琥珀=注意，蓝=跟踪中，暗=空闲。
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "app_web.h"

static const char *TAG = "BLINK";

#define WS2812_GPIO  GPIO_NUM_21
#define BOOT_GPIO    GPIO_NUM_0

typedef struct { uint8_t r, g, b; const char *name; } color_t;
static const color_t PALETTE[] = {
    {   0, 255,   0, "green"   },
    { 255, 140,   0, "amber"   },
    {   0, 120, 255, "blue"    },
    {  40,  40,  40, "dim"     },
};
#define PALETTE_N (sizeof(PALETTE) / sizeof(PALETTE[0]))

static led_strip_handle_t s_led;
static int s_color_idx;

static void led_set(int idx)
{
    const color_t *c = &PALETTE[idx % PALETTE_N];
    if (led_strip_set_pixel(s_led, 0, c->r, c->g, c->b) != ESP_OK ||
        led_strip_refresh(s_led) != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 刷新失败");
        return;
    }
    ESP_LOGI(TAG, "LED %s", c->name);
}

static bool boot_pressed(void)
{
    return gpio_get_level(BOOT_GPIO) == 0; // 按下接地
}

void app_main(void)
{
    // BOOT 键：输入 + 上拉（按下为 0）
    const gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    // WS2812 via RMT
    led_strip_config_t strip = {
        .strip_gpio_num = WS2812_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz → 100ns 分辨率，够 WS2812 时序
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip, &rmt, &s_led));
    led_strip_clear(s_led);

    ESP_LOGI(TAG, "blink ready: ws2812=GPIO%d boot=GPIO%d heap=%uK psram=%uK",
             WS2812_GPIO, BOOT_GPIO,
             (unsigned)(esp_get_free_heap_size() / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* 板端维护页 :80（WiFi 配网 / OTA 刷机 / 状态），自带 APSTA 热点兜底 */
    app_web_init();

    /* 主循环看门狗：1s 一拍喂狗，卡死 >5s 触发 panic 重启自恢复 */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 5000,
        .idle_core_mask = 0, /* IDLE 由默认配置照看，这里只挂主任务 */
        .trigger_panic = true,
    };
    esp_err_t werr = esp_task_wdt_init(&wdt_cfg);
    if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(werr);
    }
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    int beat = 0;
    while (true) {
        esp_task_wdt_reset();
        if (boot_pressed()) {
            s_color_idx++;
            led_set(s_color_idx);
            vTaskDelay(pdMS_TO_TICKS(300)); // 消抖
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        s_color_idx++;
        led_set(s_color_idx);
        if (++beat >= 10) {
            beat = 0;
            ESP_LOGI(TAG, "heartbeat uptime=%llds heap=%uK",
                     (long long)(esp_timer_get_time() / 1000000),
                     (unsigned)(esp_get_free_heap_size() / 1024));
        }
    }
}
