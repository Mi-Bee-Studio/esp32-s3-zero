/* SSD1306 128×64 OLED 极简驱动（I2C，水平寻址，帧缓冲全量刷）。
 * 字库：5×7，列主序，bit0=顶行。只收录本项目用到的字符，
 * 未收录的字符渲染为空白。 */
#include "ssd1306.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "ssd1306";
static i2c_master_dev_handle_t s_dev;
static uint8_t s_fb[1024];

/* 5×7 字库：'0'-'9' 取经典 glcdfont 数值，字母为手工设计（可辨识即可） */

static const uint8_t glyph_digit[10][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
};

static const uint8_t glyph_letter[26][5] = {
    /* A */ {0x20, 0x54, 0x54, 0x54, 0x78},
    /* B */ {0x7F, 0x48, 0x44, 0x44, 0x38},
    /* C */ {0x38, 0x44, 0x40, 0x44, 0x38},
    /* D */ {0x7F, 0x41, 0x41, 0x22, 0x1C},
    /* E */ {0x7F, 0x49, 0x49, 0x49, 0x41},
    /* F */ {0x7F, 0x09, 0x09, 0x09, 0x01},
    /* G */ {0x3E, 0x41, 0x49, 0x49, 0x7A},
    /* H */ {0x7F, 0x08, 0x08, 0x08, 0x7F},
    /* I */ {0x00, 0x41, 0x7F, 0x41, 0x00},
    /* J */ {0x20, 0x40, 0x41, 0x3F, 0x01},
    /* K */ {0x7F, 0x08, 0x14, 0x22, 0x41},
    /* L */ {0x7F, 0x40, 0x40, 0x40, 0x40},
    /* M */ {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    /* N */ {0x7F, 0x04, 0x08, 0x10, 0x7F},
    /* O */ {0x3E, 0x41, 0x41, 0x41, 0x3E},
    /* P */ {0x7F, 0x05, 0x05, 0x05, 0x02},
    /* Q */ {0x3E, 0x41, 0x51, 0x21, 0x5E},
    /* R */ {0x7F, 0x09, 0x19, 0x29, 0x46},
    /* S */ {0x46, 0x49, 0x49, 0x49, 0x31},
    /* T */ {0x01, 0x01, 0x7F, 0x01, 0x01},
    /* U */ {0x3F, 0x40, 0x40, 0x40, 0x3F},
    /* V */ {0x1F, 0x20, 0x40, 0x20, 0x1F},
    /* W */ {0x3F, 0x40, 0x38, 0x40, 0x3F},
    /* X */ {0x63, 0x14, 0x08, 0x14, 0x63},
    /* Y */ {0x07, 0x08, 0x70, 0x08, 0x07},
    /* Z */ {0x61, 0x51, 0x49, 0x45, 0x43},
};

static const uint8_t GLYPH_COLON[5] = {0x00, 0x12, 0x12, 0x00, 0x00};
static const uint8_t GLYPH_DOT[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
static const uint8_t GLYPH_MINUS[5] = {0x00, 0x08, 0x08, 0x08, 0x00};
/* v3.1 UI 新增：+（校准偏移）%（湿度）/（比例）°（温度，源码用 \x80 单字节占位） */
static const uint8_t GLYPH_PLUS[5] = {0x08, 0x08, 0x3E, 0x08, 0x08};
static const uint8_t GLYPH_PERCENT[5] = {0x43, 0x30, 0x08, 0x06, 0x63};
static const uint8_t GLYPH_DEGREE[5] = {0x02, 0x05, 0x05, 0x02, 0x00};
static const uint8_t GLYPH_SLASH[5] = {0x60, 0x30, 0x08, 0x06, 0x03};

static const uint8_t *glyph_of(char c)
{
    if (c >= '0' && c <= '9') {
        return glyph_digit[c - '0'];
    }
    if (c >= 'A' && c <= 'Z') {
        return glyph_letter[c - 'A'];
    }
    if (c >= 'a' && c <= 'z') { /* 小写映射为大写 */
        return glyph_letter[c - 'a'];
    }
    if (c == ':') {
        return GLYPH_COLON;
    }
    if (c == '.') {
        return GLYPH_DOT;
    }
    if (c == '-') {
        return GLYPH_MINUS;
    }
    if (c == '+') {
        return GLYPH_PLUS;
    }
    if (c == '%') {
        return GLYPH_PERCENT;
    }
    if (c == '/') {
        return GLYPH_SLASH;
    }
    if (c == '\x80') { /* 度符号占位（"20.3\x80" "C"） */
        return GLYPH_DEGREE;
    }
    return NULL; /* 空格等未收录字符 → 空白 */
}

static esp_err_t cmd(uint8_t c)
{
    uint8_t buf[2] = {0x00, c};
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

static esp_err_t cmd2(uint8_t c, uint8_t v)
{
    uint8_t buf[3] = {0x00, c, v};
    return i2c_master_transmit(s_dev, buf, 3, 100);
}

esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, uint8_t addr)
{
    ESP_ERROR_CHECK(i2c_master_bus_add_device(
        bus, &(i2c_device_config_t){ .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                     .device_address = addr,
                                     .scl_speed_hz = 400000 },
        &s_dev));
    /* 128×64 标准初始化序列 */
    ESP_ERROR_CHECK(cmd(0xAE));            /* display off */
    ESP_ERROR_CHECK(cmd2(0xD5, 0x80));     /* clock div */
    ESP_ERROR_CHECK(cmd2(0xA8, 0x3F));     /* mux 63 */
    ESP_ERROR_CHECK(cmd2(0xD3, 0x00));     /* display offset */
    ESP_ERROR_CHECK(cmd2(0x40, 0x00));     /* start line 0 */
    ESP_ERROR_CHECK(cmd2(0x8D, 0x14));     /* charge pump on（必须） */
    ESP_ERROR_CHECK(cmd2(0x20, 0x00));     /* 水平寻址 */
    ESP_ERROR_CHECK(cmd(0xA1));            /* segment remap */
    ESP_ERROR_CHECK(cmd(0xC8));            /* COM 扫描方向 */
    ESP_ERROR_CHECK(cmd2(0xDA, 0x12));     /* COM pins */
    ESP_ERROR_CHECK(cmd2(0x81, 0xCF));     /* 对比度 */
    ESP_ERROR_CHECK(cmd2(0xD9, 0xF1));     /* precharge */
    ESP_ERROR_CHECK(cmd2(0xDB, 0x40));     /* vcomh */
    ESP_ERROR_CHECK(cmd(0xA4));            /* 显示 RAM 内容 */
    ESP_ERROR_CHECK(cmd(0xA6));            /* 正常极性 */
    ssd1306_clear();
    ssd1306_flush();
    ESP_ERROR_CHECK(cmd(0xAF));            /* display on */
    ESP_LOGI(TAG, "SSD1306 就绪：0x%02X", addr);
    return ESP_OK;
}

void ssd1306_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

static void set_px(int x, int y)
{
    if (x < 0 || x >= 128 || y < 0 || y >= 64) {
        return;
    }
    s_fb[(y / 8) * 128 + x] |= 1 << (y % 8);
}

void ssd1306_text(int col, int row, const char *s, int scale)
{
    int x = col;
    for (const char *p = s; *p; p++) {
        const uint8_t *g = glyph_of(*p);
        if (g) {
            for (int c = 0; c < 5; c++) {
                for (int b = 0; b < 7; b++) {
                    if (g[c] & (1 << b)) {
                        for (int dx = 0; dx < scale; dx++) {
                            for (int dy = 0; dy < scale; dy++) {
                                set_px(x + c * scale + dx,
                                       row + b * scale + dy);
                            }
                        }
                    }
                }
            }
        }
        x += 6 * scale; /* 5 列字宽 + 1 列间距 */
    }
}

void ssd1306_hline(int x0, int x1, int y)
{
    for (int x = x0; x <= x1; x++) {
        set_px(x, y);
    }
}

void ssd1306_vline(int x, int y0, int y1)
{
    for (int y = y0; y <= y1; y++) {
        set_px(x, y);
    }
}

void ssd1306_fill_rect(int x, int y, int w, int h)
{
    for (int dy = 0; dy < h; dy++) {
        ssd1306_hline(x, x + w - 1, y + dy);
    }
}

void ssd1306_display_on(bool on)
{
    cmd(on ? 0xAF : 0xAE);
}

void ssd1306_flush(void)
{
    /* 全屏重定位（0x21/0x22 均为双参数命令）+ 分页流式写入 */
    uint8_t col_win[4] = {0x00, 0x21, 0x00, 0x7F}; /* 列 0..127 */
    uint8_t page_win[4] = {0x00, 0x22, 0x00, 0x07}; /* 页 0..7 */
    ESP_ERROR_CHECK(i2c_master_transmit(s_dev, col_win, 4, 100));
    ESP_ERROR_CHECK(i2c_master_transmit(s_dev, page_win, 4, 100));
    uint8_t chunk[129];
    chunk[0] = 0x40;
    for (int page = 0; page < 8; page++) {
        memcpy(chunk + 1, s_fb + page * 128, 128);
        ESP_ERROR_CHECK(i2c_master_transmit(s_dev, chunk, sizeof(chunk), 200));
    }
}
