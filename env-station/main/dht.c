/* DHT11/DHT22 单总线读取 —— 起始脉冲用 GPIO 开漏，波形捕获用 RMT RX。
 *
 * 时序：MCU 开漏拉低 ≥18ms 发起 → 切回输入（上拉为高）→ 传感器回
 * 80us 低 + 80us 高 → 40 bit：每 bit 50us 低 + 26us 高(0) / 70us 高(1)。
 *
 * 为什么不用 RMT TX 发起始脉冲（v1 教训）：TX 通道是推挽输出且空闲电平
 * 默认为低、disable 后仍挂在引脚上——数据线被常按在 GND，传感器永远
 * 看不到起始脉冲、RX 也等不到任何边沿（表现为所有引脚都 timeout）。
 * GPIO 开漏：拉低主动、释放靠上拉，无竞争、无空闲电平问题。
 *
 * 解析：收集 15~120us 的高电平——首枚是 80us 响应，其后 40 枚是数据位
 * （>45us 判 1）。引脚要求外部 4.7~10k 上拉（模块自带），内部上拉兜底。
 */
#include "dht.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "dht";

static int s_gpio;
static rmt_channel_handle_t s_rx;
static SemaphoreHandle_t s_done;
static rmt_symbol_word_t s_buf[64];
static volatile size_t s_n = 0;

static bool IRAM_ATTR on_recv(rmt_channel_handle_t ch,
                              const rmt_rx_done_event_data_t *edata,
                              void *user)
{
    BaseType_t woken = pdFALSE;
    memcpy(s_buf, edata->received_symbols,
           edata->num_symbols * sizeof(rmt_symbol_word_t));
    s_n = edata->num_symbols;
    xSemaphoreGiveFromISR(s_done, &woken);
    return woken == pdTRUE;
}

static esp_err_t setup_rx(int gpio_num)
{
    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = gpio_num,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, /* 1us/tick */
        .mem_block_symbols = 64,
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_cfg, &s_rx));
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(
        s_rx, &(rmt_rx_event_callbacks_t){ .on_recv_done = on_recv }, NULL));
    ESP_ERROR_CHECK(rmt_enable(s_rx));
    return ESP_OK;
}

esp_err_t dht_init(int gpio_num)
{
    s_done = xSemaphoreCreateBinary();
    return dht_retarget(gpio_num);
}

/* 换脚重建通道（自动扫描用）：删旧建新 */
esp_err_t dht_retarget(int gpio_num)
{
    s_gpio = gpio_num;
    if (s_rx) {
        rmt_disable(s_rx);
        rmt_del_channel(s_rx);
        s_rx = NULL;
    }

    /* 输入 + 内部上拉兜底（RMT RX 只接输入路径，不动 pad 驱动） */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(setup_rx(gpio_num));
    ESP_LOGI(TAG, "DHT 就绪：IO%d（GPIO 开漏起始 + RMT RX）", gpio_num);
    return ESP_OK;
}

bool dht_read(dht_reading_t *out, int *err)
{
    memset(out, 0, sizeof(*out));
    int e = 0;

    rmt_receive(s_rx, s_buf, sizeof(s_buf),
                &(rmt_receive_config_t){
                    /* 驱动上限：min<3187ns、max<32767000ns（S3 实测报错值） */
                    .signal_range_min_ns = 2000,
                    .signal_range_max_ns = 30000000, /* 30ms < 32.767ms 上限 */
                });

    /* 起始脉冲：开漏拉低 20ms → 切回输入，上拉接管 */
    gpio_set_direction(s_gpio, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(s_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_direction(s_gpio, GPIO_MODE_INPUT);

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(200)) != pdTRUE) {
        e = 1; /* 总线无任何边沿（传感器未接/无供电/线被按死） */
        goto done;
    }

    /* 收集 15~120us 的高电平：首枚为 80us 响应，其后 40 枚为数据位 */
    int highs[48] = {0};
    int nh = 0;
    for (size_t i = 0; i < s_n && nh < 48; i++) {
        if (s_buf[i].duration0 >= 15 && s_buf[i].duration0 <= 120
            && s_buf[i].level0 == 1) {
            highs[nh++] = s_buf[i].duration0;
        }
        if (s_buf[i].duration1 >= 15 && s_buf[i].duration1 <= 120
            && s_buf[i].level1 == 1) {
            highs[nh++] = s_buf[i].duration1;
        }
    }
    out->n_highs = nh;
    out->n_sym = (int)s_n;
    /* nh=42：释放伪影(~30us)+响应(80us)+40 位；nh=41：响应+40 位 */
    const int *bits_src = highs;
    int nbits = nh;
    if (nbits == 42) {
        bits_src = highs + 2;
        nbits = 40;
    } else if (nbits == 41) {
        bits_src = highs + 1;
        nbits = 40;
    }
    if (nbits < 40) {
        e = 3; /* 总线有边沿但位数不足（波形/上拉问题） */
        goto done;
    }

    uint64_t v = 0;
    for (int i = 0; i < 40; i++) {
        v = (v << 1) | (bits_src[i] > 45 ? 1 : 0);
    }
    for (int i = 0; i < 5; i++) {
        out->raw[i] = (v >> (32 - 8 * i)) & 0xFF;
    }
    uint8_t sum = out->raw[0] + out->raw[1] + out->raw[2] + out->raw[3];
    if (sum != out->raw[4]) {
        e = 2; /* 校验和错 */
        goto done;
    }

    /* 型号自动分辨（2026-09-27 修正）：先按 DHT22 解，物理量程外（rh>100 /
     * |t| 越界）回退 DHT11。单看"小数字节非零"会误判——DHT11 的十位小数
     * 字节非零恰好长得像 DHT22 格式（rp2040-zero 实测帧 27 00 24 03 按
     * DHT22 解出 rh=998.4%/t=921.9℃，按 DHT11 = 39%/36.3℃）。
     * 真 DHT22 的 rh 原始值上限 0x03E8(=100.0)，rh>100 必非 DHT22。 */
    float rh22 = ((out->raw[0] << 8) | out->raw[1]) * 0.1f;
    float t22 = (((out->raw[2] & 0x7F) << 8) | out->raw[3]) * 0.1f;
    if (out->raw[2] & 0x80) {
        t22 = -t22;
    }
    bool dht22 = (out->raw[1] != 0 || out->raw[3] != 0)
                 && rh22 <= 100.0f && t22 <= 125.0f && t22 >= -40.0f;
    out->model = dht22 ? 22 : 11;
    if (dht22) {
        out->rh = ((out->raw[0] << 8) | out->raw[1]) * 0.1f;
        float t = ((out->raw[2] & 0x7F) << 8 | out->raw[3]) * 0.1f;
        out->t_c = (out->raw[2] & 0x80) ? -t : t;
    } else {
        out->rh = out->raw[0] + out->raw[1] * 0.1f;
        out->t_c = out->raw[2] + out->raw[3] * 0.1f;
    }
    out->ok = true;

done:
    if (err) {
        *err = e;
    }
    return out->ok;
}

const char *dht_err_str(int err)
{
    switch (err) {
    case 0: return "ok";
    case 1: return "timeout";
    case 2: return "checksum";
    case 3: return "short-frame";
    default: return "?";
    }
}
