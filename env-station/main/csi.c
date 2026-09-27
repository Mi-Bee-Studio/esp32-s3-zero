/* WiFi STA + CSI 采集 + ping 网关激励 + #S1 串流。
 *
 * 拷贝自 luatos wifi-csi-sensing 的验证结论（项目间不共享代码，先拷贝）：
 * - 勿用 WIFI_PS_NONE（繁忙信道上会饿死 IDLE 引发看门狗风暴）；
 * - 激励 = ping 网关（回包=下行单播数据帧 → 稳定 CSI 源），dump_ack_en 出 ACK CSI；
 * - 子载波抽 16 个自 [nsc/8, 3nsc/4]，避开边缘空载波与直流；
 * - 输出行 "#S1 seq t_ms rssi hex64"（与 luatos 终端同格式，homepulse 可统一解析）。
 *
 * 凭据：NVS 命名空间 "wifi" 的 ssid/pass，经串口命令 "wifi <ssid> <pass>"、
 * homepulse 的 wifi_connect 或板端维护页（app_web.c）下发。
 * 常开 SoftAP "env-station"（APSTA）：配网/OTA 救援通道，见 csi_station_init。
 */
#include "csi.h"

#include "esp_task_wdt.h"

#include <string.h>

#include "app_disc.h"
#include "wfp_tcp.h"

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"

static const char *TAG = "csi";

#define NSEL 16
#define PING_HZ 10

typedef struct {
    uint32_t t_ms;
    int8_t rssi;
    int8_t iq[NSEL * 2];
} csi_rec_t;

static QueueHandle_t s_q;
static esp_ping_handle_t s_ping;
static bool s_csi_on;
static char s_ssid[33];
static volatile bool s_connected;
static volatile int8_t s_rssi;
static volatile uint32_t s_csi_count, s_ping_replies, s_rate_hz_x10;
static uint32_t s_seq;
static volatile bool s_streaming = true; /* WiFi 连上即流；sense_stop 可停 */
static volatile int s_pc_present = -1;   /* homepulse sense_status 的在场判定：-1 未知 */
static volatile int s_last_nsc;          /* 最近一帧子载波总数（HELLO 用） */
static bool s_hello_pending;             /* 首帧前收到 sense_start，待补发 HELLO */
static volatile bool s_scan_busy;        /* wifi_scan 进行中：事件处理器勿抢重连 */
static bool s_have_creds;                /* NVS 存有凭据：没凭据不自动关联（radio 空闲，扫描可用） */

/* ---- PC 链路判活（协议 v3）：任一平台命令即续链，30s 无命令回本地自治 ---- */
static volatile int64_t s_pc_last_ms;
static volatile uint32_t s_pc_cmds;

/* ---- 本地在场估计（协议 v3 #PRES）：幅度帧间差 EWMA + 双阈值迟滞 ----
 * 幅度取 |I|+|Q|（免开方）。阈值出厂为猜测值；增强协议（PC 在链时）由
 * homepulse 采样 m 对照其 DSP 判定后经 sense_cfg 下发专属值（NVS 持久）。 */
#define MOT_ENTER_DEF 25.0f
#define MOT_EXIT_DEF  12.0f
#define MOT_RISE_N 8    /* 连续超起阈值帧数（防抖） */
#define MOT_FALL_N 16
static float s_mot_enter = MOT_ENTER_DEF;
static float s_mot_exit = MOT_EXIT_DEF;
static float s_motion_ewma;
static int s_local_present = -1; /* -1 未知 0 无 1 有 */
static uint32_t s_pres_seq;
static int16_t s_prev_amp[NSEL];

static void emit_pres(void);

/* 阈值持久化（NVS "env"/moten,motex ×0.1）：换固件/重启不丢学习成果 */
static void mot_load(void)
{
    nvs_handle_t h;
    if (nvs_open("env", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t en = 0, ex = 0;
    nvs_get_i32(h, "moten", &en);
    nvs_get_i32(h, "motex", &ex);
    nvs_close(h);
    if (en > 5 && ex > 2 && en > ex + 2) {
        s_mot_enter = en / 10.0f;
        s_mot_exit = ex / 10.0f;
        ESP_LOGI(TAG, "本地估计阈值（NVS）：enter>%.0f exit<%.0f", s_mot_enter,
                 s_mot_exit);
    }
}

static void mot_save(void)
{
    nvs_handle_t h;
    if (nvs_open("env", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, "moten", (int32_t)(s_mot_enter * 10));
    nvs_set_i32(h, "motex", (int32_t)(s_mot_exit * 10));
    nvs_commit(h);
    nvs_close(h);
}

void csi_note_pc(void)
{
    s_pc_last_ms = esp_timer_get_time() / 1000;
    s_pc_cmds++;
}

bool csi_pc_linked(void)
{
    return s_pc_last_ms != 0
        && (esp_timer_get_time() / 1000 - s_pc_last_ms) < 30000;
}

static void local_presence_tick(const csi_rec_t *rec)
{
    int32_t dsum = 0;
    for (int k = 0; k < NSEL; k++) {
        int16_t i = rec->iq[k * 2], q = rec->iq[k * 2 + 1];
        int16_t a = (int16_t)((i < 0 ? -i : i) + (q < 0 ? -q : q));
        int16_t d = (int16_t)(a - s_prev_amp[k]);
        dsum += d < 0 ? -d : d;
        s_prev_amp[k] = a;
    }
    s_motion_ewma = s_motion_ewma * 0.95f + ((float)dsum / NSEL) * 0.05f;
    /* 暖机：静室里初始态会一直卡"未知"，600 帧（约 3-30s 视帧率）无动作
     * 则默认"无人"，本地模式显示不再悬空 */
    static uint32_t warm_n;
    if (s_local_present == -1 && ++warm_n > 600) {
        s_local_present = 0;
        emit_pres();
    }
    static int rise_n, fall_n;
    if (s_local_present != 1 && s_motion_ewma > s_mot_enter) {
        fall_n = 0;
        if (++rise_n >= MOT_RISE_N) {
            rise_n = 0;
            s_local_present = 1;
            emit_pres();
        }
    } else if (s_local_present != 0 && s_local_present != -1
               && s_motion_ewma < s_mot_exit) {
        rise_n = 0;
        if (++fall_n >= MOT_FALL_N) {
            fall_n = 0;
            s_local_present = 0;
            emit_pres();
        }
    } else {
        rise_n = fall_n = 0;
    }
}

/* 状态变化才上送；PC 在链时平台权威、不发（本地值仍更新供 OLED 降级显示） */
static void emit_pres(void)
{
    ESP_LOGI(TAG, "本地在场：%s（m=%.1f pc=%s）",
             s_local_present == 1 ? "IN" : "OUT", s_motion_ewma,
             csi_pc_linked() ? "linked" : "local");
    if (csi_pc_linked()) {
        return;
    }
    wfp_out("#PRES {\"seq\":%lu,\"present\":%s,\"src\":\"csi-local\","
            "\"m\":%.1f}",
            (unsigned long)s_pres_seq++, s_local_present == 1 ? "true" : "false",
            s_motion_ewma);
}

static void csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    const int nsc = info->len / 2;
    if (nsc < NSEL) {
        return;
    }
    s_last_nsc = nsc;
    csi_rec_t rec = {
        .t_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .rssi = info->rx_ctrl.rssi,
    };
    const int lo = nsc / 8;
    const int span = (nsc * 3) / 4;
    for (int k = 0; k < NSEL; k++) {
        int idx = lo + (k * span) / (NSEL - 1);
        if (idx >= nsc) {
            idx = nsc - 1;
        }
        rec.iq[k * 2] = (int8_t)info->buf[idx * 2];
        rec.iq[k * 2 + 1] = (int8_t)info->buf[idx * 2 + 1];
    }
    if (xQueueSend(s_q, &rec, 0) != pdTRUE) {
        /* 队满丢弃：感知分析要新鲜度不要完整性 */
    }
}

static void csi_send_hello(void);

static void stream_task(void *arg)
{
    (void)arg;
    csi_rec_t rec;
    char hex[NSEL * 2 * 2 + 1];
    /* 挂任务看门狗（03:01 事故教训：输出路径死锁时此任务无声卡死）。
     * 队列等待限时 1s：空闲也周期醒来喂狗，WiFi 离网不误杀 */
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        if (xQueueReceive(s_q, &rec, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        local_presence_tick(&rec);
        if (!s_streaming) {
            continue;
        }
        static const char HEX[] = "0123456789abcdef";
        for (int i = 0; i < NSEL * 2; i++) {
            uint8_t v = (uint8_t)rec.iq[i];
            hex[i * 2] = HEX[v >> 4];
            hex[i * 2 + 1] = HEX[v & 0xF];
        }
        hex[NSEL * 4] = 0;
        wfp_out("#S1 %lu %lu %d %s", (unsigned long)s_seq++,
                (unsigned long)rec.t_ms, rec.rssi, hex);
        s_csi_count++;
        if (s_hello_pending) {
            s_hello_pending = false;
            csi_send_hello();
        }
    }
}

/* 每秒统计帧率（简单差分） */
static void stats_tick(void)
{
    static uint32_t last;
    uint32_t now = s_csi_count;
    s_rate_hz_x10 = (now - last) * 10;
    last = now;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_rssi = ap.rssi;
    }
}

/* 会话头：子载波选定索引与限频（homepulse ParseHello 兼容格式） */
static void csi_send_hello(void)
{
    int nsc = s_last_nsc;
    if (nsc < NSEL) {
        s_hello_pending = true;
        return;
    }
    char sel[16 * 4 + 1] = "";
    int off = 0;
    const int lo = nsc / 8;
    const int span = (nsc * 3) / 4;
    for (int k = 0; k < NSEL; k++) {
        int idx = lo + (k * span) / (NSEL - 1);
        if (idx >= nsc) {
            idx = nsc - 1;
        }
        off += snprintf(sel + off, sizeof(sel) - off, "%s%d", k ? "," : "", idx);
    }
    wfp_out("#S1-HELLO nsc=%d sel=%s rate=%d", nsc, sel, PING_HZ);
}

/* WiFi 配网命令（homepulse 面板经串口下发；响应格式与 luatos app_wifi.c
 * 的 wifi_scan/wifi_connect 协议处理器一致，平台 boardCtl 无需区分板型）。
 * SSID/密码可含空格引号，故走 cJSON 正经解析，不用子串抽取。 */
static void wifi_json_scan(void)
{
    wifi_scan_config_t sc = { 0 };
    /* 未配网时 radio 空闲（无凭据不自动关联），直接扫；正处关联尝试则
     * 报 WIFI_STATE，稍候重试一次 */
    s_scan_busy = true;
    esp_err_t sret = esp_wifi_scan_start(&sc, true);
    if (sret == ESP_ERR_WIFI_STATE) {
        vTaskDelay(pdMS_TO_TICKS(500));
        sret = esp_wifi_scan_start(&sc, true);
    }
    s_scan_busy = false;
    if (sret != ESP_OK) {
        wfp_out("{\"cmd\":\"wifi_scan\",\"status\":\"error\",\"error\":\"scan failed\"}");
        return;
    }
    enum { MAX_AP = 16 };
    wifi_ap_record_t aps[MAX_AP];
    uint16_t n = MAX_AP;
    if (esp_wifi_scan_get_ap_records(&n, aps) != ESP_OK) {
        n = 0;
    }
    ESP_LOGI(TAG, "wifi_scan：%u 个网络", (unsigned)n);
    /* 整条应答一次组好再发（wfp_out 每次调用即一行） */
    char out[2400];
    int off = snprintf(out, sizeof(out), "{\"cmd\":\"wifi_scan\",\"status\":\"ok\",\"data\":[");
    for (uint16_t i = 0; i < n && off < (int)sizeof(out) - 64; i++) {
        char esc[sizeof(aps[i].ssid) * 2 + 1]; /* SSID 可含引号/反斜杠：转义 */
        char *q = esc;
        for (const char *p = (const char *)aps[i].ssid; *p; p++) {
            if (*p == '"' || *p == '\\') {
                *q++ = '\\';
            }
            *q++ = *p;
        }
        *q = '\0';
        off += snprintf(out + off, sizeof(out) - off, "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                        i ? "," : "", esc, aps[i].rssi);
    }
    snprintf(out + off, sizeof(out) - off, "]}");
    wfp_out("%s", out);
}

static void wifi_json_connect(const cJSON *msg)
{
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(msg, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(msg, "pass");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        wfp_out("{\"cmd\":\"wifi_connect\",\"status\":\"error\",\"error\":\"missing ssid\"}");
        return;
    }
    if (csi_station_set_wifi(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "")) {
        wfp_out("{\"cmd\":\"wifi_connect\",\"status\":\"ok\"}");
    } else {
        wfp_out("{\"cmd\":\"wifi_connect\",\"status\":\"error\",\"error\":\"set failed\"}");
    }
}

/* homepulse 命令/推送处理（sense_* 子串级解析够用；wifi_* 走 cJSON）。
 * 任一可识别命令都视作 PC 在链（协议 v3 链路判活）。 */
void csi_on_cmd(const char *line)
{
    if (!line || !strchr(line, ':')) {
        return;
    }
    if (strstr(line, "\"wifi_scan\"")) {
        csi_note_pc();
        wifi_json_scan();
        return;
    }
    if (strstr(line, "\"wifi_connect\"")) {
        csi_note_pc();
        cJSON *msg = cJSON_Parse(line);
        if (msg != NULL) {
            wifi_json_connect(msg);
            cJSON_Delete(msg);
        }
        return;
    }
    if (strstr(line, "\"hello\"")) {
        csi_note_pc();
        char hl[320];
        if (wfp_hello_line(hl, sizeof(hl)) > 0) {
            wfp_out("%s", hl);
            ESP_LOGI(TAG, "hello 握手应答（PC 在链）");
        }
        return;
    }
    if (strstr(line, "\"sense_stat\"")) {
        /* 增强协议：PC 采样板端本地估计器的运动量（对照其 DSP 判定学阈值） */
        csi_note_pc();
        wfp_out("{\"cmd\":\"sense_stat\",\"status\":\"ok\",\"m\":%.1f,"
                "\"loc\":%d,\"enter\":%.0f,\"exit\":%.0f}",
                s_motion_ewma, s_local_present, s_mot_enter, s_mot_exit);
        return;
    }
    if (strstr(line, "\"sense_cfg\"")) {
        /* 增强协议：PC 下发为这块板学出的专属阈值（NVS 持久，掉线仍生效） */
        csi_note_pc();
        cJSON *msg = cJSON_Parse(line);
        if (msg != NULL) {
            const cJSON *en = cJSON_GetObjectItemCaseSensitive(msg, "mot_enter");
            const cJSON *ex = cJSON_GetObjectItemCaseSensitive(msg, "mot_exit");
            if (cJSON_IsNumber(en) && cJSON_IsNumber(ex)) {
                float fen = (float)en->valuedouble, fex = (float)ex->valuedouble;
                if (fen > 5 && fen < 300 && fex > 2 && fex < 300
                    && fen > fex + 2) {
                    s_mot_enter = fen;
                    s_mot_exit = fex;
                    mot_save();
                    ESP_LOGI(TAG, "增强协议：阈值已更新 enter>%.0f exit<%.0f",
                             s_mot_enter, s_mot_exit);
                }
            }
            cJSON_Delete(msg);
        }
        wfp_out("{\"cmd\":\"sense_cfg\",\"status\":\"ok\","
                "\"enter\":%.0f,\"exit\":%.0f}",
                s_mot_enter, s_mot_exit);
        return;
    }
    if (strstr(line, "sense_start")) {
        csi_note_pc();
        s_streaming = true;
        const char *p = strstr(line, "stimulus_hz");
        if (p) {
            int hz = atoi(strchr(p, ':') + 1);
            if (hz > 0 && hz <= 100) {
                ping_restart(hz);
            }
        }
        ESP_LOGI(TAG, "sense_start（homepulse）");
        csi_send_hello();
    } else if (strstr(line, "sense_stop")) {
        csi_note_pc();
        s_streaming = false;
        ESP_LOGI(TAG, "sense_stop：暂停串流");
    } else if (strstr(line, "sense_status")) {
        csi_note_pc();
        if (strstr(line, "\"present\":true")) {
            s_pc_present = 1;
        } else if (strstr(line, "\"present\":false")) {
            s_pc_present = 0;
        }
    }
}

static void ping_start_hz(int hz)
{
    if (s_ping) {
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = NULL;
    }
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.gw.addr == 0) {
        return;
    }
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 0; /* 无限 */
    cfg.interval_ms = 1000 / (uint32_t)hz;
    cfg.timeout_ms = 1000;
    cfg.data_size = 8;
    ip_addr_copy_from_ip4(cfg.target_addr, ip.gw);
    esp_ping_callbacks_t cbs = { 0 };
    if (esp_ping_new_session(&cfg, &cbs, &s_ping) == ESP_OK) {
        esp_ping_start(s_ping);
        ESP_LOGI(TAG, "激励 ping 网关 %dms 周期", cfg.interval_ms);
    } else {
        s_ping = NULL;
    }
}

static void ping_start(void)
{
    if (!s_ping) {
        ping_start_hz(PING_HZ);
    }
}

void ping_restart(int hz)
{
    ping_start_hz(hz);
}

static void csi_enable(void)
{
    if (s_csi_on) {
        return;
    }
    /* 保持默认 MODEM 省电（WIFI_PS_NONE 会饿死 IDLE，luatos 实测教训） */
    wifi_csi_config_t cfg = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = false,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = false,
        .dump_ack_en = true,
    };
    if (esp_wifi_set_csi_config(&cfg) != ESP_OK
        || esp_wifi_set_csi_rx_cb(csi_rx_cb, NULL) != ESP_OK
        || esp_wifi_set_csi(true) != ESP_OK) {
        ESP_LOGW(TAG, "CSI 使能失败（sdkconfig 需 CONFIG_ESP_WIFI_CSI_ENABLED）");
        return;
    }
    s_csi_on = true;
    ESP_LOGI(TAG, "CSI 采集已使能");
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_creds) {
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "无凭据：待配网（wifi_scan/wifi_connect），不自动关联");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wfp_net_up(false);
        if (s_scan_busy) {
            return; /* 扫描占着 radio：别抢着重连 */
        }
        if (!s_have_creds) {
            return; /* 没凭据的重连只是空转占 radio（曾因此扫描恒空表） */
        }
        if (s_connected) {
            s_connected = false;
            ESP_LOGW(TAG, "WiFi 断开，5s 后重试");
            vTaskDelay(pdMS_TO_TICKS(5000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000)); /* 关联未成：稍退再试，别热循环 */
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_connected = true;
        ESP_LOGI(TAG, "WiFi 已连接：%s ip=" IPSTR, s_ssid, IP2STR(&e->ip_info.ip));
        wfp_net_up(true); /* WFP TCP 端点开始服务（homepulse 可脱串口直连） */
        csi_enable();
        ping_start();
    }
}

static bool load_creds(char *ssid, size_t slen, char *pass, size_t plen)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t s = slen, p = plen;
    bool ok = nvs_get_str(h, "ssid", ssid, &s) == ESP_OK
        && nvs_get_str(h, "pass", pass, &p) == ESP_OK && ssid[0];
    nvs_close(h);
    return ok;
}

void csi_station_init(void)
{
    /* NVS 必须先初始化（WiFi 驱动与凭据存储都依赖；拷贝自 luatos 的教训：
     * 漏了这步 esp_wifi_init 会以 0x1101 abort） */
    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nerr = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nerr);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap(); /* 配网/救援热点的 netif */
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    /* SoftAP "env-station"（密码 12345678 → http://192.168.4.1）：未配网或
     * 掉线时的配网/OTA 救援通道；STA 关联后 AP 信道自动跟随，CSI 不受影响 */
    wifi_config_t ap = {
        .ap = {
            .ssid = "env-station",
            .ssid_len = 11,
            .password = "12345678",
            .channel = 1,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    s_q = xQueueCreate(32, sizeof(csi_rec_t));
    xTaskCreate(stream_task, "csi_stream", 4096, NULL, 5, NULL);
    mot_load(); /* 增强协议学到的阈值（若有） */

    char ssid[33] = {0}, pass[65] = {0};
    if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        s_have_creds = true;
        strlcpy(s_ssid, ssid, sizeof(s_ssid));
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        ESP_LOGI(TAG, "NVS 有凭据，关联 \"%s\"…", ssid);
    } else {
        ESP_LOGW(TAG, "无 WiFi 凭据：串口发 \"wifi <ssid> <pass>\" 下发");
    }
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool csi_station_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败");
        return false;
    }
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    s_have_creds = true; /* 此后断线自动重连 */
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password));
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        return false;
    }
    s_connected = false;
    if (s_ping) { /* 换网先停旧激励 */
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = NULL;
    }
    esp_wifi_disconnect(); /* 触发重连流程 */
    ESP_LOGI(TAG, "凭据已存并开始关联 \"%s\"", ssid);
    return true;
}

void csi_stream_toggle(void)
{
    s_streaming = !s_streaming;
    ESP_LOGI(TAG, "串流%s（板载按键）", s_streaming ? "恢复" : "暂停");
}

void csi_station_get_status(csi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->connected = s_connected;
    out->csi_on = s_csi_on;
    out->streaming = s_streaming;
    strlcpy(out->ssid, s_ssid, sizeof(out->ssid));
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
        }
    }
    out->rssi = s_rssi;
    out->rate_hz = s_rate_hz_x10 / 10.0f;
    out->csi_count = s_csi_count;
    out->ping_replies = s_ping_replies;
    out->pc_present = s_pc_present;
    out->pc_linked = csi_pc_linked();
    out->local_present = s_local_present;
    out->motion = s_motion_ewma;
    out->mot_enter = s_mot_enter;
    out->mot_exit = s_mot_exit;
    stats_tick();
}
