/* env-station v3 —— 温湿度 + 双色 OLED UI + WiFi CSI 采集。
 *
 * 实测接线：OLED SDA=IO6、SCL=IO5（0x3C，上 16 行黄下 48 行蓝的双色屏）；
 * 左按钮=IO1、右按钮=IO2；DHT22 DATA=IO3。
 * UI：黄带=状态带（页名/WiFi 信号条/串流呼吸点/PC 在链），蓝区=数据，
 * 底行=按键提示（动作后 1.5s 让位给 toast 回执）。
 * 按键语法（与 luatos 感知终端同一套）：
 *   L 短按      翻页 ENV→CSI→SYS
 *   L 长按 1s   串流开关（全局）
 *   R 短按      页内主操作：ENV=立即读数 CSI=串流开关 SYS=诊断快照
 *   R 长按 1s   重启进 ROM 下载模式（免拔插刷机自救）
 *   L+R 同按 2.5s 软重启（1.5s 时提示倒计时）
 * 屏保（防烧屏规范）：空闲 10min 自动熄屏（GRAM 保留），任意键唤醒（该次
 * 按键只唤醒不触发动作，避免长按误入 DFU）。
 * CSI：WiFi 关联后自动开启，ping 网关激励，#S1 行流经 USB 串口（serialtap 采集）。
 * 串口命令："wifi <ssid> <pass>" 下发凭据；"wifi?" 查状态。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/ip4_addr.h"

#include "app_disc.h"
#include "app_probe.h"
#include "app_web.h"
#include "csi.h"
#include "dht.h"
#include "main.h"
#include "wfp_tcp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "soc/rtc_cntl_reg.h"
#include "ssd1306.h"

static const char *TAG = "env-station";

#define I2C_SDA_IO 6
#define I2C_SCL_IO 5
#define BTN_L_IO 1
#define BTN_R_IO 2
#define DHT_IO 3
#define DHT_PERIOD_MS 2500

/* 最新读数（DHT 任务写，UI/日志读；uint32 序号做轻量同步） */
static dht_reading_t s_last;
static volatile uint32_t s_seq;
static volatile int s_err_last = 1;
static volatile bool s_force_read;
static uint32_t s_reads, s_errs;
static float s_t_off, s_rh_off; /* 校准偏移（NVS "env"/toff,rhoff） */

static void buttons_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BTN_L_IO) | (1ULL << BTN_R_IO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static void dht_task(void *arg)
{
    dht_reading_t r;
    int err = 0;
    int cur_gpio = DHT_IO;
    int consec_timeout = 0;
    bool scanned = false;
    static const int SCAN[] = {3, 4, 7, 8, 9, 10, 11, 12, 13, 17, 18, 38, 39, 40, 41, 42, 45};
    /* 挂看门狗：采集卡死（RMT/单总线时序异常）>5s 整机重启自恢复 */
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        if (dht_read(&r, &err)) {
            /* #ENV 上报原始值（板端本地校准只作用于 OLED/本地显示，
             * 平台侧校准才是权威——见 homepulse/docs/wfp-protocol.md） */
            wfp_out("#ENV {\"seq\":%lu,\"t\":%.1f,\"rh\":%.1f}",
                    (unsigned long)s_seq, r.t_c, r.rh);
            r.t_c += s_t_off;
            r.rh += s_rh_off;
            if (r.rh < 0) r.rh = 0;
            if (r.rh > 100) r.rh = 100;
            s_last = r;
            s_err_last = 0;
            s_seq++;
            s_reads++;
            consec_timeout = 0;
            ESP_LOGI(TAG, "DHT%d@IO%d: t=%.1fC rh=%.1f%% raw=%02X%02X%02X%02X%02X",
                     r.model, cur_gpio, r.t_c, r.rh, r.raw[0], r.raw[1],
                     r.raw[2], r.raw[3], r.raw[4]);
        } else {
            s_err_last = err;
            s_errs++;
            ESP_LOGW(TAG, "DHT@IO%d 读取失败: %s sym=%d highs=%d（第 %lu 次）",
                     cur_gpio, dht_err_str(err), r.n_sym, r.n_highs,
                     (unsigned long)s_errs);
            if (err == 1 && ++consec_timeout >= 4 && !scanned) {
                scanned = true;
                ESP_LOGI(TAG, "IO%d 无应答，开始扫描 DHT 实际接线…", cur_gpio);
                for (size_t i = 0; i < sizeof(SCAN) / sizeof(SCAN[0]); i++) {
                    int g = SCAN[i];
                    if (g == cur_gpio) {
                        continue;
                    }
                    ESP_ERROR_CHECK(dht_retarget(g));
                    vTaskDelay(pdMS_TO_TICKS(50));
                    int e2 = 0;
                    dht_reading_t r2;
                    if (dht_read(&r2, &e2) || e2 != 1) {
                        cur_gpio = g;
                        ESP_LOGI(TAG, "DHT 定位成功：IO%d（err=%s）", g,
                                 dht_err_str(e2));
                        break;
                    }
                }
                if (cur_gpio == DHT_IO) {
                    ESP_LOGW(TAG, "扫描完毕：候选脚均无 DHT 应答，回 IO%d 继续试",
                             DHT_IO);
                    ESP_ERROR_CHECK(dht_retarget(DHT_IO));
                }
                consec_timeout = 0;
            }
        }
        for (int waited = 0; waited < DHT_PERIOD_MS; waited += 100) {
            if (s_force_read) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_force_read = false;
    }
}

static void cal_load(void)
{
    nvs_handle_t h;
    if (nvs_open("env", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t t = 0, r = 0;
    nvs_get_i32(h, "toff", &t);
    nvs_get_i32(h, "rhoff", &r);
    nvs_close(h);
    s_t_off = t / 10.0f;   /* 存 0.1 精度整数 */
    s_rh_off = r / 10.0f;
}

static bool cal_save(float toff, float rhoff)
{
    nvs_handle_t h;
    if (nvs_open("env", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_set_i32(h, "toff", (int32_t)(toff * 10));
    nvs_set_i32(h, "rhoff", (int32_t)(rhoff * 10));
    nvs_commit(h);
    nvs_close(h);
    s_t_off = toff;
    s_rh_off = rhoff;
    return true;
}

/* 串口命令任务（驱动 API 读取，绕过 VFS/stdin——实测 fgets 收不到）：
 * "wifi <ssid> <pass>" / "wifi?" / "cal <toff> <rhoff>" / "cal?"
 * JSON 行（homepulse 协议）转交 csi_on_cmd。 */
static void cmd_task(void *arg)
{
    (void)arg;
    usb_serial_jtag_driver_config_t dcfg = {
        .rx_buffer_size = 2048,
        .tx_buffer_size = 2048,
    };
    esp_err_t ret = usb_serial_jtag_driver_install(&dcfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "USB-JTAG 驱动已装（命令行通道就绪）");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "USB-JTAG 驱动已由 console 装过，复用");
    } else {
        ESP_LOGW(TAG, "USB-JTAG 驱动安装失败：%s（命令行不可用）",
                 esp_err_to_name(ret));
        vTaskDelete(NULL);
    }

    char line[256];
    size_t pos = 0;
    uint8_t buf[64];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf) - 1,
                                           pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c != '\r' && c != '\n') {
                if (pos < sizeof(line) - 1) {
                    line[pos++] = c;
                }
                continue;
            }
            if (pos == 0) {
                continue;
            }
            line[pos] = 0;
            pos = 0;
            cmd_dispatch(line);
        }
    }
}

/* 控制台行分发（main.h）：USB 控制台任务与 WFP TCP 端点（wfp_tcp.c）共用。 */
void cmd_dispatch(const char *line)
{
    ESP_LOGI(TAG, "CMD: %s", line);
    if (line[0] == '{') {
        csi_on_cmd(line);
    } else if (strncmp(line, "wifi ", 5) == 0) {
        char ssid[33] = {0}, pass[65] = {0};
        int m = sscanf(line + 5, "%32s %64s", ssid, pass);
        if (m >= 1) {
            csi_station_set_wifi(ssid, m == 2 ? pass : "");
        } else {
            printf("> 用法: wifi <ssid> <pass>\n");
        }
    } else if (strcmp(line, "wifi?") == 0) {
        csi_status_t st;
        csi_station_get_status(&st);
        printf("> wifi %s%s csi=%s\n", st.ssid[0] ? st.ssid : "(none)",
               st.connected ? " CONNECTED" : "",
               st.csi_on ? "ON" : "off");
    } else if (strncmp(line, "cal ", 4) == 0) {
        float toff = 0, rhoff = 0;
        if (sscanf(line + 4, "%f %f", &toff, &rhoff) >= 1) {
            if (cal_save(toff, rhoff)) {
                printf("> cal t%+.1f rh%+.1f 已存\n", toff, rhoff);
            } else {
                printf("> cal 存储失败\n");
            }
        }
    } else if (strcmp(line, "cal?") == 0) {
        printf("> cal t%+.1f rh%+.1f\n", s_t_off, s_rh_off);
    } else if (strcmp(line, "pres?") == 0) {
        csi_status_t st;
        csi_station_get_status(&st);
        printf("> pc=%s m=%.1f loc=%s（enter>%.0f exit<%.0f）\n",
               st.pc_linked ? "linked" : "-", st.motion,
               st.local_present == 1 ? "IN"
               : (st.local_present == 0 ? "OUT" : "-"),
               st.mot_enter, st.mot_exit);
    } else if (strcmp(line, "probe?") == 0) {
        uint8_t n = 0;
        const probe_target_t *ts = app_probe_get_targets(&n);
        const probe_result_t *rs = app_probe_get_results(&n);
        printf("> 探测目标 %u 个（配置经维护页 /api/probe）\n", n);
        for (uint8_t i = 0; i < n; i++) {
            printf("> %s %s:%u %lus %s %s(%lums)\n", ts[i].name, ts[i].target,
                   ts[i].port, (unsigned long)(ts[i].interval_ms / 1000),
                   ts[i].module_name,
                   rs[i].success ? "OK" : "FAIL",
                   (unsigned long)rs[i].duration_ms);
        }
    } else if (strncmp(line, "probe run ", 10) == 0) {
        probe_result_t r;
        if (app_probe_run_one(line + 10, &r)) {
            printf("> %s %s %lums%s%s\n", line + 10, r.success ? "OK" : "FAIL",
                   (unsigned long)r.duration_ms, r.error_msg[0] ? " " : "",
                   r.error_msg);
        } else {
            printf("> 目标不存在或忙（probe? 查看名单）\n");
        }
    } else if (strcmp(line, "probe reset") == 0) {
        app_probe_config_reset();
        printf("> 探测配置已恢复出厂（4 模块 0 目标）\n");
    } else {
        printf("> 未知命令（wifi <ssid> <pass>|wifi?|cal <t> <rh>|cal?|pres?|"
               "probe?|probe run <name>|probe reset）\n");
    }
}

bool env_latest(float *t, float *rh)
{
    if (s_seq == 0) {
        return false;
    }
    *t = s_last.t_c;
    *rh = s_last.rh;
    return true;
}

/* ---------------- 双色屏 UI ----------------
 * 黄带 y0..15   状态带：页名 | WiFi 信号条 | 串流呼吸点 | PC 在链
 * 蓝区 y16..62  页面数据（ENV/CSI/SYS）
 * 底行 y55      按键提示；动作后 1.5s 内让位给 toast 回执
 * 坐标均为像素（ssd1306_text 的 row 参 = y 像素），scale1 行高 7。
 */

/* 居中文本（scale 放大；超宽贴左裁剪） */
static void centered(int row, const char *s, int scale)
{
    int w = (int)(strlen(s) * 6 * scale);
    ssd1306_text(w >= 128 ? 0 : (128 - w) / 2, row, s, scale);
}

/* toast：占用底行提示位 1.5s（单缓冲，后到覆盖） */
static char s_toast[16];
static int64_t s_toast_until_ms;

static bool s_screen_off;
static int64_t s_peek_until_ms;   /* >0 = 窥视中 */
static int64_t s_next_peek_ms;
static bool s_peek_expr;
static int s_expr_kind, s_expr_frame;
static int64_t s_expr_next_ms, s_peek_ui_next_ms;

/* —— 随机窥视表情动画（OLED 128x64）——
 * kind 0：圆眼（眨眼 + 视线 ±4px）+ 微笑；kind 1：开心 ∧∧ 眼 + 大笑。
 * 250ms 一帧，变体在每次窥视开始时随机挑选。 */
static void draw_expr(void)
{
    char line[24];
    ssd1306_clear();
    s_expr_frame++;
    int blink = (s_expr_frame % 16) >= 14;
    int gaze = ((s_expr_frame / 8) % 3) - 1;
    if (s_expr_kind == 0) {
        int eh = blink ? 2 : 14;
        int ey = 24 + (14 - eh) / 2;
        ssd1306_fill_rect(34 + gaze * 4, ey, 14, eh);
        ssd1306_fill_rect(80 + gaze * 4, ey, 14, eh);
        ssd1306_hline(46, 82, 48);           /* 微笑（三段弦线） */
        ssd1306_hline(52, 76, 50);
        ssd1306_hline(58, 70, 52);
    } else {
        int bounce = (s_expr_frame % 8) < 2 ? -3 : 0;
        ssd1306_hline(40 + bounce, 44 + bounce, 22);   /* ∧∧ 开心眼 */
        ssd1306_hline(37 + bounce, 47 + bounce, 24);
        ssd1306_hline(34 + bounce, 50 + bounce, 26);
        ssd1306_hline(84 + bounce, 88 + bounce, 22);
        ssd1306_hline(81 + bounce, 91 + bounce, 24);
        ssd1306_hline(78 + bounce, 94 + bounce, 26);
        ssd1306_hline(42, 86, 46);           /* 大笑 */
        ssd1306_hline(48, 80, 49);
        ssd1306_hline(54, 74, 50);
    }
    (void)line;
    ssd1306_flush();
}

/* 周期性屏保（2026-09-27 规范：带屏必配，防 OLED 烧屏）：空闲 10min 熄屏 */
#define SCREENSAVER_MS (10 * 60 * 1000)
/* 随机窥视：熄屏期间每 1-5min 亮 1-60s，随机显示当前页数据或表情动画 */
#define PEEK_EVERY_MAX_S (5 * 60)
#define PEEK_LEN_MAX_S   60
#define EXPR_TICK_MS     250

static void ui_toast(const char *text)
{
    strlcpy(s_toast, text, sizeof(s_toast));
    s_toast_until_ms = esp_timer_get_time() / 1000 + 1500;
}

/* 状态带：页名左，WiFi 条 x84..94，串流点 x100..103，PC x114..125 */
static void draw_status_band(int page, const csi_status_t *st)
{
    static const char *names[] = { "ENV", "CSI", "SYS" };
    ssd1306_text(2, 4, names[page], 1);

    if (st->connected) { /* 信号条：基线 y14，杆高 4/7/10，按 RSSI 点亮 */
        int bars = st->rssi >= -60 ? 3 : (st->rssi >= -72 ? 2 : 1);
        for (int i = 0; i < 3; i++) {
            int h = 4 + i * 3;
            if (i < bars) {
                ssd1306_fill_rect(84 + i * 4, 14 - h, 3, h);
            } else {
                ssd1306_vline(85 + i * 4, 14 - h, 14); /* 未亮杆画 1px 骨架 */
            }
        }
    } else {
        ssd1306_text(84, 8, "-", 1);
    }

    bool blink_on = ((esp_timer_get_time() / 1000000) & 1) == 0;
    if (st->streaming && blink_on) {
        ssd1306_fill_rect(100, 11, 4, 4); /* 串流：1s 呼吸点 */
    } else {                              /* 停流/灭相：空心框 */
        ssd1306_hline(100, 103, 11);
        ssd1306_hline(100, 103, 14);
        ssd1306_vline(100, 11, 14);
        ssd1306_vline(103, 11, 14);
    }
    if (st->pc_linked) {
        ssd1306_text(114, 4, "PC", 1);
    }
}

static void draw_page_env(void)
{
    char line[24];
    ssd1306_text(2, 17, "TEMP", 1);
    ssd1306_text(78, 17, "RH", 1);
    if (s_seq > 0) {
        /* 温度 ≤6 字：-10℃ 以下舍小数（"-15\x80C" 5 字），否则负温两位数
         * 7 字 ×12px = 84px 会压到 x78 的 RH 列（2026-09-27 排版审计） */
        if (s_last.t_c <= -10.0f) {
            snprintf(line, sizeof(line), "%.0f\x80" "C", (double)s_last.t_c);
        } else {
            snprintf(line, sizeof(line), "%.1f\x80" "C", (double)s_last.t_c);
        }
        ssd1306_text(2, 26, line, 2);  /* ≤6 字 ×12px = 72 → x2..73 */
        snprintf(line, sizeof(line), "%.0f%%", (double)s_last.rh);
        ssd1306_text(78, 26, line, 2); /* ≤4 字 ×12px = 48 → x78..126 */
        /* 露点（Magnus）+ 读数次列（K 计数：长跑后 "N 414720" 会超宽） */
        float gamma = logf(s_last.rh / 100.0f) +
                      17.62f * s_last.t_c / (33.33f + s_last.t_c);
        snprintf(line, sizeof(line), "DEW %.1f\x80" "C",
                 33.33f * gamma / (17.62f - gamma));
        ssd1306_text(2, 45, line, 1);  /* ≤"DEW -40.0\x80C" 12 字 = 72 → x2..73 */
        snprintf(line, sizeof(line), "N %luk",
                 (unsigned long)(s_reads / 1000));
        ssd1306_text(84, 45, line, 1); /* ≤"N 4194K" 7 字 = 42 → x84..126 */
    } else {
        ssd1306_text(2, 26, "--.-\x80" "C", 2);
        ssd1306_text(78, 26, "--%", 2);
        snprintf(line, sizeof(line), "DHT: %s", dht_err_str(s_err_last));
        ssd1306_text(2, 45, line, 1);
    }
}

static void draw_page_csi(const csi_status_t *st)
{
    char line[20];
    /* 在场权威随链路切换：PC 在链=平台判定，否则本地估计（协议 v3） */
    int pres = st->pc_linked ? st->pc_present : st->local_present;
    /* 大字 "OUT" 占 x2..55（3 字 ×18px）——右列锚 x60 起才不重叠
     * （曾锚 x48：OUT 一显示就压住 PC/LOCAL/M%/Hz，2026-09-27 修正） */
    ssd1306_text(2, 18, pres < 0 ? "--" : (pres ? "IN" : "OUT"), 3);
    ssd1306_text(60, 19, st->pc_linked ? "PC" : "LOCAL", 1);
    snprintf(line, sizeof(line), "M %d%%", (int)(st->motion * 100));
    ssd1306_text(60, 28, line, 1); /* ≤"M 100%" 6 字 = 36px → x60..96 */
    snprintf(line, sizeof(line), "%.1fHz", st->rate_hz);
    ssd1306_text(60, 37, line, 1);

    /* 运动条 x2..91 + 学习阈值刻度（sense_cfg 下发的 enter/exit） */
    int fill = (int)((st->motion > 1.0f ? 1.0f : st->motion) * 89);
    if (fill > 0) {
        ssd1306_fill_rect(2, 46, fill, 5);
    }
    ssd1306_hline(2, 91, 52);
    ssd1306_vline(2 + (int)(st->mot_enter * 89), 43, 45);
    ssd1306_vline(2 + (int)(st->mot_exit * 89), 43, 45);
    snprintf(line, sizeof(line), ">%.2f", st->mot_enter);
    ssd1306_text(96, 45, line, 1);
}

static void draw_page_sys(const csi_status_t *st)
{
    char line[26];
    const int64_t up = esp_timer_get_time() / 1000000;
    snprintf(line, sizeof(line), "UP %02d:%02d:%02d H%uK",
             (int)((up / 3600) % 100), (int)((up / 60) % 60), (int)(up % 60),
             (unsigned)(esp_get_free_heap_size() / 1024));
    ssd1306_text(2, 17, line, 1);
    snprintf(line, sizeof(line), "CSI %luk V0.1.0",
             (unsigned long)(st->csi_count / 1000));
    ssd1306_text(2, 26, line, 1);
    snprintf(line, sizeof(line), "IP %s", st->ip[0] ? st->ip : "192.168.4.1");
    ssd1306_text(2, 35, line, 1);
    uint8_t n = 0, ok = 0;
    const probe_result_t *rs = app_probe_get_results(&n);
    for (uint8_t i = 0; i < n; i++) {
        ok += rs[i].success ? 1 : 0;
    }
    snprintf(line, sizeof(line), "DHT %luk E%lu P%u/%u",
             (unsigned long)(s_reads / 1000), (unsigned long)s_errs, ok, n);
    ssd1306_text(2, 44, line, 1); /* ≤"DHT 4194K E99999 P8/8" 21 字 = 126px 恰满 */
}

static void draw_footer(int page, const csi_status_t *st)
{
    const char *hint =
        page == 0 ? "L:PAGE R:READ" :
        page == 1 ? (st->streaming ? "L:PAGE R:STOP" : "L:PAGE R:GO") :
                    "L:PAGE R:LOG R1S:DFU";
    if (esp_timer_get_time() / 1000 < s_toast_until_ms) {
        ssd1306_text(2, 55, s_toast, 1);
    } else {
        ssd1306_text(2, 55, hint, 1);
    }
}

static void ui_draw(int page)
{
    csi_status_t st;
    csi_station_get_status(&st);
    ssd1306_clear();
    draw_status_band(page, &st);
    if (page == 0) {
        draw_page_env();
    } else if (page == 1) {
        draw_page_csi(&st);
    } else {
        draw_page_sys(&st);
    }
    draw_footer(page, &st);
    ssd1306_flush();
}

/* 诊断快照 → 串口日志（心跳与 SYS 页 R 短按共用一行格式，serialtap 可见） */
static void diag_log(void)
{
    csi_status_t st;
    csi_station_get_status(&st);
    if (s_seq > 0) {
        ESP_LOGI(TAG,
                 "diag | up=%us heap=%u T=%.1f RH=%.1f csi=%.1fHz rssi=%d "
                 "pc=%s m=%.1f loc=%s",
                 (unsigned)(esp_timer_get_time() / 1000000),
                 (unsigned)esp_get_free_heap_size(), s_last.t_c, s_last.rh,
                 st.rate_hz, st.rssi, st.pc_linked ? "linked" : "-",
                 st.motion,
                 st.local_present == 1 ? "IN"
                 : (st.local_present == 0 ? "OUT" : "-"));
    } else {
        ESP_LOGI(TAG, "diag | up=%us heap=%u DHT:%s",
                 (unsigned)(esp_timer_get_time() / 1000000),
                 (unsigned)esp_get_free_heap_size(),
                 dht_err_str(s_err_last));
    }
}

/* R 短按的页内主操作（ENV=立即读数 CSI=串流开关 SYS=诊断快照） */
static void page_r_action(int page)
{
    csi_status_t st;
    switch (page) {
    case 0:
        s_force_read = true;
        ESP_LOGI(TAG, "BTN_R：立即读取 DHT");
        ui_toast("READ NOW");
        break;
    case 1:
        csi_stream_toggle();
        csi_station_get_status(&st);
        ui_toast(st.streaming ? "STREAM ON" : "STREAM OFF");
        break;
    default:
        diag_log();
        ui_toast("DIAG LOGGED");
        break;
    }
    ui_draw(page);
}

/* log_dev_descriptor: #DEV 自述行 —— serialtap 身份事实引擎的固件侧契约
 * （开机 + 每 60s）：插上串口工具即自动识别板卡身份，无需探测。全字段
 * 运行时取值，WiFi 未连时 ip/ssid/ch 缺省。 */
static void log_dev_descriptor(void)
{
    char mac[18] = "", ip[16] = "", ssid[33] = "";
    uint8_t m[6];
    if (esp_read_mac(m, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    uint32_t flash_mb = 0, psram_kb = 0;
    {
        const esp_flash_t *f = esp_flash_default_chip();
        if (f) {
            uint32_t sz = 0;
            if (esp_flash_get_size((esp_flash_t *)f, &sz) == ESP_OK && sz) {
                flash_mb = sz / (1024 * 1024);
            }
        }
        size_t ps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        if (ps) psram_kb = (uint32_t)ps / 1024;
    }
    wifi_ap_record_t ap = {0};
    int ch = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(ssid, sizeof(ssid), "%s", ap.ssid);
        wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel((uint8_t *)&ch, &sec);
    }
    esp_netif_ip_info_t ipi = {0};
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif && esp_netif_get_ip_info(nif, &ipi) == ESP_OK && ipi.ip.addr) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ipi.ip));
    }
    char ch_s[8] = "";
    if (ch) snprintf(ch_s, sizeof(ch_s), " ch=%d", ch);
    printf("#DEV model=env-station fw=0.1.0 flash=%luM psram=%luK mac=%s%s%s%s%s%s
",
           (unsigned long)flash_mb, (unsigned long)psram_kb, mac,
           ip[0] ? " ip=" : "", ip[0] ? ip : "",
           ssid[0] ? " ssid=" : "", ssid[0] ? ssid : "", ch_s);
}

void app_main(void)
{
    log_dev_descriptor();
    srand((unsigned)(esp_timer_get_time() & 0x7fffffff)); /* 窥视随机序列 */
    ESP_LOGI(TAG, "env-station v0.1.0 (+blackbox) | heap=%u | psram=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

    buttons_init();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    ESP_ERROR_CHECK(ssd1306_init(bus, 0x3C));
    centered(18, "ENV", 3);
    centered(46, "STATION V0.1.0", 1);
    ssd1306_flush();

    ESP_ERROR_CHECK(dht_init(DHT_IO));
    xTaskCreate(dht_task, "dht", 4096, NULL, 5, NULL);

    csi_station_init(); /* 内含 nvs_flash_init —— cal 依赖它，必须先跑 */
    cal_load();
    app_disc_init();    /* WFP 网络发现播报器（UDP :7789，IP 事件自驱动） */
    app_web_init();     /* 板端维护页 :80（状态/配网/OTA 刷机/重启） */
    xTaskCreate(cmd_task, "cmd", 4096, NULL, 5, NULL);
    wfp_tcp_init(); /* WFP TCP 端点：WiFi 在网后 homepulse 可脱串口直连 */
    app_probe_init(); /* 网络探测（blackbox 能力）：NVS 配置 + 调度任务 */

    /* 主循环看门狗：20ms 一拍喂狗，卡死 >5s 自动重启（应用级死锁兜底） */
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

    /* 按键状态机（20ms 一拍）：短按 <400ms，长按 ≥1s，L+R 同按 2.5s 软重启；
     * 空闲 10min 自动熄屏（防烧屏规范），任意键唤醒并吞掉该次按键 */
    int page = 0;
    int l_hold = 0, r_hold = 0, combo_hold = 0;
    bool l_fired = false;
    uint32_t ticks = 0;
    uint32_t seq_seen = 0;
    int64_t last_activity_ms = esp_timer_get_time() / 1000;
    while (1) {
        esp_task_wdt_reset();
        if (ticks % (60 * 50) == 0) { /* 50Hz 主循环 → 每 60s 重发 #DEV 自述 */
            log_dev_descriptor();
        }
        int l = gpio_get_level(BTN_L_IO);
        int r = gpio_get_level(BTN_R_IO);
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool any_pressed = (l == 0 || r == 0);
        if (any_pressed) {
            last_activity_ms = now_ms;
        }
        if (s_screen_off) {
            if (any_pressed) { /* 唤醒并吞掉本次按键，不触发任何动作 */
                ssd1306_display_on(true);
                s_screen_off = false;
                s_peek_until_ms = 0;
                l_hold = r_hold = combo_hold = 0;
                l_fired = false;
                ESP_LOGI(TAG, "screensaver：唤醒");
            } else {
                /* 随机窥视：每 1-5min 亮 1-60s，随机=当前页数据/表情动画 */
                if (s_peek_until_ms) {
                    if (now_ms >= s_peek_until_ms) {
                        ssd1306_display_on(false);
                        s_peek_until_ms = 0;
                        s_next_peek_ms = now_ms +
                            (60 + rand() % (PEEK_EVERY_MAX_S - 59)) * 1000;
                        ESP_LOGI(TAG, "peek 结束，回熄屏");
                    } else if (s_peek_expr) {
                        if (now_ms >= s_expr_next_ms) {
                            draw_expr();
                            s_expr_next_ms = now_ms + EXPR_TICK_MS;
                        }
                    } else if (now_ms >= s_peek_ui_next_ms) {
                        ui_draw(page); /* 数据窥视：当前页照常刷新 */
                        s_peek_ui_next_ms = now_ms + 1000;
                    }
                } else if (now_ms >= s_next_peek_ms) {
                    s_peek_expr = (rand() & 1) != 0;
                    s_expr_frame = 0;
                    s_peek_until_ms = now_ms + (1 + rand() % PEEK_LEN_MAX_S) * 1000;
                    s_expr_next_ms = s_peek_ui_next_ms = now_ms;
                    ssd1306_display_on(true);
                    ESP_LOGI(TAG, "peek：%s %ds",
                             s_peek_expr ? "表情动画" : "采集数据",
                             (int)((s_peek_until_ms - now_ms) / 1000));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue; /* 熄屏/窥视期间不跑按键 FSM（窥视自刷新） */
        }
        if (now_ms - last_activity_ms >= SCREENSAVER_MS) {
            s_screen_off = true;
            s_peek_until_ms = 0;
            s_next_peek_ms = now_ms +
                (60 + rand() % (PEEK_EVERY_MAX_S - 59)) * 1000;
            ssd1306_display_on(false); /* GRAM 保留，唤醒即恢复 */
            ESP_LOGI(TAG, "screensaver：空闲 %d 分钟熄屏（任意键唤醒）",
                     (int)(SCREENSAVER_MS / 60000));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (l == 0 && r == 0) {
            /* 组合键：期间吞掉单键事件，避免顺带翻页/进 DFU */
            combo_hold++;
            if (combo_hold == 75) {
                ui_toast("REBOOT IN 1S");
                ui_draw(page);
            }
            if (combo_hold >= 125) {
                ESP_LOGI(TAG, "L+R 长按：软重启");
                ssd1306_clear();
                centered(28, "REBOOT", 2);
                ssd1306_flush();
                esp_restart();
            }
        } else {
            combo_hold = 0;
            if (l == 0) {
                l_hold++;
                if (l_hold == 50) { /* 长按 1s：串流开关（全局） */
                    csi_stream_toggle();
                    csi_status_t st;
                    csi_station_get_status(&st);
                    ui_toast(st.streaming ? "STREAM ON" : "STREAM OFF");
                    ui_draw(page);
                    l_fired = true;
                }
            } else {
                if (l_hold > 0 && l_hold < 20 && !l_fired) {
                    page = (page + 1) % 3;
                    ESP_LOGI(TAG, "BTN_L 短按：切到第 %d 页", page);
                    ui_draw(page);
                }
                l_hold = 0;
                l_fired = false;
            }
            if (r == 0) {
                r_hold++;
                if (r_hold == 50) { /* 长按 1s：ROM 下载模式（自救刷机） */
                    ESP_LOGI(TAG, "BTN_R 长按：重启进 ROM 下载模式（可刷机）");
                    ssd1306_clear();
                    centered(28, "BOOTLOADER", 2);
                    ssd1306_flush();
                    REG_WRITE(RTC_CNTL_OPTION1_REG,
                              RTC_CNTL_FORCE_DOWNLOAD_BOOT);
                    esp_restart();
                }
            } else {
                if (r_hold > 0 && r_hold < 20) {
                    page_r_action(page);
                }
                r_hold = 0;
            }
        }

        if (s_seq != seq_seen) {
            seq_seen = s_seq;
            ui_draw(page);
        }
        ticks++;
        if (ticks % 100 == 0) { /* 2s：状态带呼吸点/页数据周期刷新 */
            ui_draw(page);
        }
        if (ticks % 500 == 0) { /* 10s：串口心跳 */
            diag_log();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
