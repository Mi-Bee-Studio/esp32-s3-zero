/* app_probe — 网络探测能力实现（esp32-blackbox 移植版）。
 *
 * 组成（上游对应关系）：
 * - 配置：config_manager.c 的 JSON schema/解析/校验/序列化逐字移植，
 *   存储后端 SPIFFS → NVS 字符串；
 * - 调度：probe_manager.c 的任务模型逐字移植（每目标独立调度 + 版本号
 *   热加载），去 TWDT 挂载、加 WiFi 关联门控；
 * - 指标文本：metrics_server.c 的 write_* 函数移植，端点由 app_web 注册。
 */
#include "app_probe.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "csi.h"

static const char *TAG = "probe";

#define PROBE_NVS_NS    "probe"
#define PROBE_NVS_KEY   "cfg"
#define PROBE_CFG_MAX   3500 /* NVS 变长项安全上限（页 4096） */

/* ---- 状态 ---- */

static probe_module_t s_modules[MAX_MODULES];
static probe_target_t s_targets[MAX_TARGETS];
static probe_result_t s_results[MAX_TARGETS];
static int64_t s_next_run[MAX_TARGETS];   /* 各目标下次运行（us since boot） */
static bool s_busy[MAX_TARGETS];          /* /probe 与调度防并发 */
static uint8_t s_module_count, s_target_count;
static uint8_t s_version;                 /* 热加载版本号 */
static uint32_t s_scrape_interval_ms = 30000;
static TaskHandle_t s_task;

/* ---- JSON 解析（上游 config_manager.c 移植） ---- */

static probe_module_type_t prober_str_to_type(const char *prober)
{
    if (strcmp(prober, "http") == 0)     return MODULE_HTTP;
    if (strcmp(prober, "https") == 0)    return MODULE_HTTPS;
    if (strcmp(prober, "tcp") == 0)      return MODULE_TCP;
    if (strcmp(prober, "tcp_tls") == 0)  return MODULE_TCP_TLS;
    if (strcmp(prober, "dns") == 0)      return MODULE_DNS;
    if (strcmp(prober, "icmp") == 0)     return MODULE_ICMP;
    if (strcmp(prober, "ws") == 0)       return MODULE_WS;
    if (strcmp(prober, "wss") == 0)      return MODULE_WSS;
    return MODULE_HTTP;
}

static const char *prober_type_to_str(probe_module_type_t type)
{
    switch (type) {
        case MODULE_HTTP:     return "http";
        case MODULE_HTTPS:    return "https";
        case MODULE_TCP:      return "tcp";
        case MODULE_TCP_TLS:  return "tcp_tls";
        case MODULE_DNS:      return "dns";
        case MODULE_ICMP:     return "icmp";
        case MODULE_WS:       return "ws";
        case MODULE_WSS:      return "wss";
        default:              return "http";
    }
}

static void parse_http_config(const cJSON *http_obj, http_module_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->method, "GET", sizeof(cfg->method) - 1);

    const cJSON *method = cJSON_GetObjectItem(http_obj, "method");
    if (method && cJSON_IsString(method)) {
        strncpy(cfg->method, method->valuestring, sizeof(cfg->method) - 1);
    }

    const cJSON *codes = cJSON_GetObjectItem(http_obj, "valid_status_codes");
    if (codes && cJSON_IsArray(codes)) {
        int count = cJSON_GetArraySize(codes);
        if (count > 8) count = 8;
        cfg->valid_status_count = (uint8_t)count;
        for (int i = 0; i < count; i++) {
            const cJSON *code = cJSON_GetArrayItem(codes, i);
            if (code && cJSON_IsNumber(code)) {
                cfg->valid_status_codes[i] = (uint16_t)code->valueint;
            }
        }
    } else {
        cfg->valid_status_codes[0] = 200;
        cfg->valid_status_count = 1;
    }

    const cJSON *no_follow = cJSON_GetObjectItem(http_obj, "no_follow_redirects");
    if (no_follow && cJSON_IsBool(no_follow)) {
        cfg->no_follow_redirects = cJSON_IsTrue(no_follow);
    }
}

static void parse_tcp_config(const cJSON *tcp_obj, tcp_module_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    const cJSON *tls = cJSON_GetObjectItem(tcp_obj, "tls");
    if (tls && cJSON_IsBool(tls)) {
        cfg->tls = cJSON_IsTrue(tls);
    }
    const cJSON *query = cJSON_GetObjectItem(tcp_obj, "query");
    if (query && cJSON_IsString(query)) {
        strncpy(cfg->query, query->valuestring, sizeof(cfg->query) - 1);
    }
    const cJSON *resp = cJSON_GetObjectItem(tcp_obj, "expected_response");
    if (resp && cJSON_IsString(resp)) {
        strncpy(cfg->expected_response, resp->valuestring, sizeof(cfg->expected_response) - 1);
    }
}

static void parse_dns_config(const cJSON *dns_obj, dns_module_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->query_name, "dns.google", sizeof(cfg->query_name) - 1);
    cfg->query_type = 1;
    const cJSON *qname = cJSON_GetObjectItem(dns_obj, "query_name");
    if (qname && cJSON_IsString(qname)) {
        strncpy(cfg->query_name, qname->valuestring, sizeof(cfg->query_name) - 1);
    }
    const cJSON *qtype = cJSON_GetObjectItem(dns_obj, "query_type");
    if (qtype && cJSON_IsNumber(qtype)) {
        cfg->query_type = (uint8_t)qtype->valueint;
    }
}

static void parse_icmp_config(const cJSON *icmp_obj, icmp_module_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->packets = 3;
    cfg->payload_size = 56;
    const cJSON *packets = cJSON_GetObjectItem(icmp_obj, "packets");
    if (packets && cJSON_IsNumber(packets)) {
        cfg->packets = (uint8_t)packets->valueint;
    }
    const cJSON *size = cJSON_GetObjectItem(icmp_obj, "payload_size");
    if (size && cJSON_IsNumber(size)) {
        cfg->payload_size = (uint16_t)size->valueint;
    }
    const cJSON *pattern = cJSON_GetObjectItem(icmp_obj, "pattern");
    if (pattern && cJSON_IsNumber(pattern)) {
        cfg->pattern = (uint8_t)pattern->valueint;
    }
}

static bool parse_module_json(const char *name, const cJSON *module_obj, probe_module_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->name, name, sizeof(out->name) - 1);

    const cJSON *prober = cJSON_GetObjectItem(module_obj, "prober");
    if (!prober || !cJSON_IsString(prober)) {
        ESP_LOGE(TAG, "模块 '%s' 缺少 prober 字段", name);
        return false;
    }
    out->config.type = prober_str_to_type(prober->valuestring);

    const cJSON *timeout = cJSON_GetObjectItem(module_obj, "timeout");
    if (timeout && cJSON_IsNumber(timeout)) {
        out->config.timeout_ms = (uint32_t)(timeout->valueint * 1000);
    } else {
        out->config.timeout_ms = 5000;
    }

    switch (out->config.type) {
        case MODULE_HTTP:
        case MODULE_HTTPS: {
            const cJSON *http = cJSON_GetObjectItem(module_obj, "http");
            if (http) {
                parse_http_config(http, &out->config.config.http);
            } else {
                memset(&out->config.config.http, 0, sizeof(http_module_config_t));
                strncpy(out->config.config.http.method, "GET",
                        sizeof(out->config.config.http.method) - 1);
                out->config.config.http.valid_status_codes[0] = 200;
                out->config.config.http.valid_status_count = 1;
            }
            break;
        }
        case MODULE_TCP:
        case MODULE_TCP_TLS: {
            const cJSON *tcp = cJSON_GetObjectItem(module_obj, "tcp");
            if (tcp) {
                parse_tcp_config(tcp, &out->config.config.tcp);
            }
            break;
        }
        case MODULE_DNS: {
            const cJSON *dns = cJSON_GetObjectItem(module_obj, "dns");
            if (dns) {
                parse_dns_config(dns, &out->config.config.dns);
            }
            break;
        }
        case MODULE_ICMP: {
            const cJSON *icmp = cJSON_GetObjectItem(module_obj, "icmp");
            if (icmp) {
                parse_icmp_config(icmp, &out->config.config.icmp);
            }
            break;
        }
        case MODULE_WS:
        case MODULE_WSS: {
            const cJSON *http = cJSON_GetObjectItem(module_obj, "http");
            if (http) {
                parse_http_config(http, &out->config.config.http);
            }
            break;
        }
    }
    return true;
}

/* 解析到 tmp 数组，校验通过才提交（上游在 validate 前即 memcpy 的
 * 半应用问题在此修正：坏配置不再污染当前生效配置） */
static esp_err_t config_parse_json(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "JSON 解析失败");
        return ESP_FAIL;
    }

    static probe_module_t tmp_modules[MAX_MODULES];
    static probe_target_t tmp_targets[MAX_TARGETS];
    uint8_t mod_count = 0, tgt_count = 0;

    const cJSON *modules_obj = cJSON_GetObjectItem(root, "modules");
    if (modules_obj && cJSON_IsObject(modules_obj)) {
        cJSON *mod_item = NULL;
        cJSON_ArrayForEach(mod_item, modules_obj) {
            if (mod_count >= MAX_MODULES) {
                ESP_LOGW(TAG, "模块数量超过上限 %d，截断", MAX_MODULES);
                break;
            }
            if (!cJSON_IsObject(mod_item)) continue;
            if (!parse_module_json(mod_item->string, mod_item, &tmp_modules[mod_count])) {
                ESP_LOGW(TAG, "跳过无效模块: %s", mod_item->string);
                continue;
            }
            mod_count++;
        }
    }

    uint32_t scrape_interval_ms = 30000;
    const cJSON *scrape = cJSON_GetObjectItem(root, "scrape_interval");
    if (scrape && cJSON_IsNumber(scrape)) {
        scrape_interval_ms = (uint32_t)(scrape->valueint * 1000);
    }
    /* metrics_port 字段接受但忽略：端点恒在 app_web 的 :80 */

    const cJSON *targets_arr = cJSON_GetObjectItem(root, "targets");
    if (targets_arr && cJSON_IsArray(targets_arr)) {
        int arr_size = cJSON_GetArraySize(targets_arr);
        for (int i = 0; i < arr_size && tgt_count < MAX_TARGETS; i++) {
            const cJSON *tgt = cJSON_GetArrayItem(targets_arr, i);
            if (!cJSON_IsObject(tgt)) continue;

            probe_target_t *t = &tmp_targets[tgt_count];
            memset(t, 0, sizeof(*t));

            const cJSON *name = cJSON_GetObjectItem(tgt, "name");
            if (name && cJSON_IsString(name)) {
                strncpy(t->name, name->valuestring, sizeof(t->name) - 1);
            }
            const cJSON *target = cJSON_GetObjectItem(tgt, "target");
            if (target && cJSON_IsString(target)) {
                strncpy(t->target, target->valuestring, sizeof(t->target) - 1);
            }
            const cJSON *port = cJSON_GetObjectItem(tgt, "port");
            if (port && cJSON_IsNumber(port)) {
                t->port = (uint16_t)port->valueint;
            }
            const cJSON *interval = cJSON_GetObjectItem(tgt, "interval");
            if (interval && cJSON_IsNumber(interval)) {
                t->interval_ms = (uint32_t)(interval->valueint * 1000);
            }
            const cJSON *module = cJSON_GetObjectItem(tgt, "module");
            if (module && cJSON_IsString(module)) {
                strncpy(t->module_name, module->valuestring, sizeof(t->module_name) - 1);
            }
            tgt_count++;
        }
    }

    cJSON_Delete(root);

    for (int i = 0; i < tgt_count; i++) {
        if (tmp_targets[i].interval_ms == 0) {
            tmp_targets[i].interval_ms = scrape_interval_ms;
        }
    }

    /* 提交 */
    memcpy(s_modules, tmp_modules, sizeof(s_modules));
    memcpy(s_targets, tmp_targets, sizeof(s_targets));
    s_module_count = mod_count;
    s_target_count = tgt_count;
    s_scrape_interval_ms = scrape_interval_ms;

    ESP_LOGI(TAG, "配置解析完成: %d 模块, %d 目标", s_module_count, s_target_count);
    return ESP_OK;
}

static esp_err_t config_validate(void)
{
    for (int i = 0; i < s_module_count; i++) {
        uint32_t t = s_modules[i].config.timeout_ms / 1000;
        if (t < 1 || t > 120) {
            ESP_LOGE(TAG, "模块 '%s' 超时 %us 超出 [1,120]", s_modules[i].name, (unsigned)t);
            return ESP_ERR_INVALID_ARG;
        }
    }
    for (int i = 0; i < s_target_count; i++) {
        const probe_target_t *t = &s_targets[i];
        if (t->port == 0) {
            ESP_LOGE(TAG, "目标 '%s' 端口不能为 0", t->name);
            return ESP_ERR_INVALID_ARG;
        }
        bool found = false;
        for (int j = 0; j < s_module_count; j++) {
            if (strcmp(t->module_name, s_modules[j].name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            ESP_LOGE(TAG, "目标 '%s' 引用的模块 '%s' 不存在", t->name, t->module_name);
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (s_scrape_interval_ms < 5000) {
        ESP_LOGE(TAG, "抓取间隔低于 5000ms");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ---- 序列化 + NVS 持久化 ---- */

static cJSON *serialize_module(const probe_module_t *mod)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "prober", prober_type_to_str(mod->config.type));
    cJSON_AddNumberToObject(obj, "timeout", mod->config.timeout_ms / 1000);

    switch (mod->config.type) {
        case MODULE_HTTP:
        case MODULE_HTTPS: {
            cJSON *http = cJSON_CreateObject();
            cJSON_AddStringToObject(http, "method", mod->config.config.http.method);
            cJSON *codes = cJSON_CreateArray();
            for (int i = 0; i < mod->config.config.http.valid_status_count; i++) {
                cJSON_AddItemToArray(codes, cJSON_CreateNumber(
                    mod->config.config.http.valid_status_codes[i]));
            }
            cJSON_AddItemToObject(http, "valid_status_codes", codes);
            cJSON_AddBoolToObject(http, "no_follow_redirects",
                                  mod->config.config.http.no_follow_redirects);
            cJSON_AddItemToObject(obj, "http", http);
            break;
        }
        case MODULE_TCP:
        case MODULE_TCP_TLS: {
            cJSON *tcp = cJSON_CreateObject();
            cJSON_AddBoolToObject(tcp, "tls", mod->config.config.tcp.tls);
            cJSON_AddStringToObject(tcp, "query", mod->config.config.tcp.query);
            cJSON_AddStringToObject(tcp, "expected_response",
                                    mod->config.config.tcp.expected_response);
            cJSON_AddItemToObject(obj, "tcp", tcp);
            break;
        }
        case MODULE_DNS: {
            cJSON *dns = cJSON_CreateObject();
            cJSON_AddStringToObject(dns, "query_name", mod->config.config.dns.query_name);
            cJSON_AddNumberToObject(dns, "query_type", mod->config.config.dns.query_type);
            cJSON_AddItemToObject(obj, "dns", dns);
            break;
        }
        case MODULE_ICMP: {
            cJSON *icmp = cJSON_CreateObject();
            cJSON_AddNumberToObject(icmp, "packets", mod->config.config.icmp.packets);
            cJSON_AddNumberToObject(icmp, "payload_size", mod->config.config.icmp.payload_size);
            cJSON_AddNumberToObject(icmp, "pattern", mod->config.config.icmp.pattern);
            cJSON_AddItemToObject(obj, "icmp", icmp);
            break;
        }
        case MODULE_WS:
        case MODULE_WSS: {
            cJSON *http = cJSON_CreateObject();
            cJSON_AddStringToObject(http, "method", mod->config.config.http.method);
            cJSON_AddBoolToObject(http, "no_follow_redirects",
                                  mod->config.config.http.no_follow_redirects);
            cJSON_AddItemToObject(obj, "http", http);
            break;
        }
    }
    return obj;
}

static char *config_serialize(void)
{
    cJSON *root = cJSON_CreateObject();

    cJSON *modules = cJSON_CreateObject();
    for (int i = 0; i < s_module_count; i++) {
        cJSON_AddItemToObject(modules, s_modules[i].name,
                              serialize_module(&s_modules[i]));
    }
    cJSON_AddItemToObject(root, "modules", modules);

    cJSON *targets = cJSON_CreateArray();
    for (int i = 0; i < s_target_count; i++) {
        cJSON *tgt = cJSON_CreateObject();
        cJSON_AddStringToObject(tgt, "name", s_targets[i].name);
        cJSON_AddStringToObject(tgt, "target", s_targets[i].target);
        cJSON_AddNumberToObject(tgt, "port", s_targets[i].port);
        if (s_targets[i].interval_ms > 0) {
            cJSON_AddNumberToObject(tgt, "interval", s_targets[i].interval_ms / 1000);
        }
        cJSON_AddStringToObject(tgt, "module", s_targets[i].module_name);
        cJSON_AddItemToArray(targets, tgt);
    }
    cJSON_AddItemToObject(root, "targets", targets);

    cJSON_AddNumberToObject(root, "scrape_interval", s_scrape_interval_ms / 1000);
    cJSON_AddNumberToObject(root, "metrics_port", 80); /* schema 兼容上游 */

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t config_save_nvs(void)
{
    char *json = config_serialize();
    if (!json) {
        return ESP_FAIL;
    }
    if (strlen(json) >= PROBE_CFG_MAX) {
        ESP_LOGE(TAG, "配置 %u 字节超 NVS 上限 %d，请精简目标",
                 (unsigned)strlen(json), PROBE_CFG_MAX);
        cJSON_free(json);
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(PROBE_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        cJSON_free(json);
        return err;
    }
    err = nvs_set_str(h, PROBE_NVS_KEY, json);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    cJSON_free(json);
    ESP_LOGI(TAG, "配置已存 NVS (%s/%s)", PROBE_NVS_NS, PROBE_NVS_KEY);
    return err;
}

static esp_err_t config_load_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PROBE_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    static char buf[PROBE_CFG_MAX + 1];
    size_t len = sizeof(buf);
    err = nvs_get_str(h, PROBE_NVS_KEY, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    return config_parse_json(buf);
}

static void config_factory_defaults(void)
{
    s_module_count = 0;
    s_target_count = 0;
    memset(s_modules, 0, sizeof(s_modules));
    memset(s_targets, 0, sizeof(s_targets));
    memset(s_results, 0, sizeof(s_results));
    memset(s_next_run, 0, sizeof(s_next_run));

    /* 四个标准模块（与上游一致）；目标默认为空——家庭设备不主动外联，
     * 用 /probe 按需探测或经维护页添加目标 */
    probe_module_t *m;
    m = &s_modules[s_module_count++];
    strncpy(m->name, "http_2xx", sizeof(m->name) - 1);
    m->config.type = MODULE_HTTP;
    m->config.timeout_ms = 10000;
    strncpy(m->config.config.http.method, "GET", sizeof(m->config.config.http.method) - 1);
    m->config.config.http.valid_status_codes[0] = 200;
    m->config.config.http.valid_status_count = 1;

    m = &s_modules[s_module_count++];
    strncpy(m->name, "tcp_connect", sizeof(m->name) - 1);
    m->config.type = MODULE_TCP;
    m->config.timeout_ms = 5000;

    m = &s_modules[s_module_count++];
    strncpy(m->name, "dns_resolve", sizeof(m->name) - 1);
    m->config.type = MODULE_DNS;
    m->config.timeout_ms = 5000;
    strncpy(m->config.config.dns.query_name, "dns.google",
            sizeof(m->config.config.dns.query_name) - 1);
    m->config.config.dns.query_type = 1;

    m = &s_modules[s_module_count++];
    strncpy(m->name, "icmp_ping", sizeof(m->name) - 1);
    m->config.type = MODULE_ICMP;
    m->config.timeout_ms = 5000;
    m->config.config.icmp.packets = 3;
    m->config.config.icmp.payload_size = 56;

    s_scrape_interval_ms = 30000;
}

/* ---- 探测调度（上游 probe_manager.c 移植） ---- */

static probe_result_t dispatch_probe(const probe_target_t *target,
                                     const probe_module_t *module)
{
    switch (module->config.type) {
        case MODULE_HTTP:     return probe_http_execute(target, &module->config);
        case MODULE_HTTPS:    return probe_https_execute(target, &module->config);
        case MODULE_TCP:      return probe_tcp_execute(target, &module->config);
        case MODULE_TCP_TLS:  return probe_tcp_tls_execute(target, &module->config);
        case MODULE_DNS:      return probe_dns_execute(target, &module->config);
        case MODULE_ICMP:     return probe_icmp_execute(target, &module->config);
        case MODULE_WS:       return probe_ws_execute(target, &module->config);
        case MODULE_WSS:      return probe_wss_execute(target, &module->config);
        default: {
            probe_result_t err = {0};
            snprintf(err.error_msg, sizeof(err.error_msg),
                     "unknown module type %d", module->config.type);
            return err;
        }
    }
}

static void probe_task(void *arg)
{
    (void)arg;
    const probe_target_t *targets = NULL;
    uint8_t target_count = 0;
    uint8_t last_version = 0;

    targets = app_probe_get_targets(&target_count);
    last_version = s_version;

    while (1) {
        /* WiFi 未关联：空转等待（探测全部会失败，无意义） */
        csi_status_t st;
        csi_station_get_status(&st);
        if (!st.connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (s_version != last_version) {
            uint8_t n = 0;
            targets = app_probe_get_targets(&n);
            target_count = n;
            last_version = s_version;
            ESP_LOGI(TAG, "配置热加载: %d 目标", target_count);
        }

        if (target_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int64_t now = esp_timer_get_time();
        int best = -1;
        for (int i = 0; i < target_count && i < MAX_TARGETS; i++) {
            if (s_next_run[i] <= now) {
                best = i;
                break; /* 轮转取到期目标，避免同拍饿尾 */
            }
        }
        if (best < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const probe_target_t *t = &targets[best];
        const probe_module_t *mod = app_probe_get_module(t->module_name);
        if (!mod) {
            ESP_LOGE(TAG, "目标 '%s' 模块 '%s' 不存在，5s 后重试",
                     t->name, t->module_name);
            s_next_run[best] = now + 5000000LL;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        probe_result_t r = dispatch_probe(t, mod);
        s_results[best] = r;
        s_next_run[best] = now + (int64_t)t->interval_ms * 1000LL;

        ESP_LOGI(TAG, "探测 %s (%s:%u): ok=%d %ums",
                 t->name, t->target, t->port, r.success, r.duration_ms);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ---- Prometheus 文本（上游 metrics_server.c 移植） ---- */

static int append_line(char *buf, int offset, int max_size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + offset, max_size - offset, fmt, ap);
    va_end(ap);
    if (n < 0 || offset + n >= max_size) {
        return offset;
    }
    return offset + n;
}

static int write_common_headers(char *buf, int o, int m)
{
    return append_line(buf, o, m,
        "# HELP probe_success Whether the probe succeeded\n"
        "# TYPE probe_success gauge\n"
        "# HELP probe_duration_seconds Duration of the probe in seconds\n"
        "# TYPE probe_duration_seconds gauge\n"
        "# HELP probe_http_status_code HTTP status code\n"
        "# TYPE probe_http_status_code gauge\n"
        "# HELP probe_ip_protocol IP protocol version\n"
        "# TYPE probe_ip_protocol gauge\n");
}

static int write_icmp_headers(char *buf, int o, int m)
{
    return append_line(buf, o, m,
        "# HELP probe_icmp_rtt_ms ICMP round trip time in ms\n"
        "# TYPE probe_icmp_rtt_ms gauge\n"
        "# HELP probe_icmp_packets_sent ICMP packets sent\n"
        "# TYPE probe_icmp_packets_sent gauge\n"
        "# HELP probe_icmp_packets_received ICMP packets received\n"
        "# TYPE probe_icmp_packets_received gauge\n");
}

static int write_target_metrics(char *buf, int o, int m,
                                const probe_target_t *t,
                                const probe_result_t *r,
                                const probe_module_t *mod)
{
    const char *tg = t->target;
    const char *mn = t->module_name;

    o = append_line(buf, o, m,
        "probe_success{target=\"%s\",module=\"%s\"} %d\n", tg, mn, r->success ? 1 : 0);
    o = append_line(buf, o, m,
        "probe_duration_seconds{target=\"%s\",module=\"%s\"} %.3f\n",
        tg, mn, r->duration_ms / 1000.0f);
    o = append_line(buf, o, m,
        "probe_http_status_code{target=\"%s\",module=\"%s\"} %d\n", tg, mn, r->status_code);
    o = append_line(buf, o, m,
        "probe_ip_protocol{target=\"%s\",module=\"%s\"} 4\n", tg, mn);

    if (mod && mod->config.type == MODULE_ICMP) {
        o = append_line(buf, o, m,
            "probe_icmp_rtt_ms{target=\"%s\",module=\"%s\"} %lu\n",
            tg, mn, (unsigned long)r->details.icmp.rtt_ms);
        o = append_line(buf, o, m,
            "probe_icmp_packets_sent{target=\"%s\",module=\"%s\"} %d\n",
            tg, mn, r->details.icmp.packets_sent);
        o = append_line(buf, o, m,
            "probe_icmp_packets_received{target=\"%s\",module=\"%s\"} %d\n",
            tg, mn, r->details.icmp.packets_received);
    }
    return o;
}

/* ---- 公共接口 ---- */

void app_probe_init(void)
{
    if (config_load_nvs() == ESP_OK && config_validate() == ESP_OK) {
        ESP_LOGI(TAG, "NVS 配置已加载: %d 模块, %d 目标",
                 s_module_count, s_target_count);
    } else {
        ESP_LOGW(TAG, "NVS 无有效配置，使用出厂默认并回存");
        config_factory_defaults();
        config_save_nvs(); /* 存不下只告警，不影响运行 */
    }
    s_version++;

    BaseType_t ok = xTaskCreate(probe_task, "probe_task", 16384, NULL, 5, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "探测任务创建失败");
    }
}

int app_probe_metrics(char *buf, int size)
{
    int o = write_common_headers(buf, 0, size);
    o = write_icmp_headers(buf, o, size);

    uint8_t n = 0;
    const probe_target_t *targets = app_probe_get_targets(&n);
    for (int i = 0; i < n; i++) {
        const probe_module_t *mod = app_probe_get_module(targets[i].module_name);
        o = write_target_metrics(buf, o, size, &targets[i], &s_results[i], mod);
    }
    return o;
}

int app_probe_probe_metrics(const char *host, int port, const char *module_name,
                            char *buf, int size)
{
    const probe_module_t *mod = app_probe_get_module(module_name);
    if (!mod) {
        return -1;
    }

    probe_target_t tmp = {0};
    strncpy(tmp.target, host, sizeof(tmp.target) - 1);
    tmp.port = (uint16_t)port;
    strncpy(tmp.module_name, module_name, sizeof(tmp.module_name) - 1);

    probe_result_t r = dispatch_probe(&tmp, mod);

    int o = write_common_headers(buf, 0, size);
    if (mod->config.type == MODULE_ICMP) {
        o = write_icmp_headers(buf, o, size);
    }
    return write_target_metrics(buf, o, size, &tmp, &r, mod);
}

char *app_probe_config_json(void)
{
    return config_serialize();
}

esp_err_t app_probe_config_update(const char *json)
{
    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 先快照，失败回滚（parse 成功后 validate 失败不能留半套） */
    static probe_module_t keep_modules[MAX_MODULES];
    static probe_target_t keep_targets[MAX_TARGETS];
    uint8_t keep_m = s_module_count, keep_t = s_target_count;
    uint32_t keep_scrape = s_scrape_interval_ms;
    memcpy(keep_modules, s_modules, sizeof(s_modules));
    memcpy(keep_targets, s_targets, sizeof(s_targets));

    esp_err_t err = config_parse_json(json);
    if (err == ESP_OK) {
        err = config_validate();
    }
    if (err != ESP_OK) {
        memcpy(s_modules, keep_modules, sizeof(s_modules));
        memcpy(s_targets, keep_targets, sizeof(s_targets));
        s_module_count = keep_m;
        s_target_count = keep_t;
        s_scrape_interval_ms = keep_scrape;
        return err;
    }
    err = config_save_nvs();
    if (err != ESP_OK) {
        memcpy(s_modules, keep_modules, sizeof(s_modules));
        memcpy(s_targets, keep_targets, sizeof(s_targets));
        s_module_count = keep_m;
        s_target_count = keep_t;
        s_scrape_interval_ms = keep_scrape;
        return err;
    }
    s_version++;
    return ESP_OK;
}

esp_err_t app_probe_config_reload(void)
{
    esp_err_t err = config_load_nvs();
    if (err == ESP_OK) {
        err = config_validate();
    }
    if (err == ESP_OK) {
        s_version++;
    }
    return err;
}

esp_err_t app_probe_config_reset(void)
{
    config_factory_defaults();
    esp_err_t err = config_save_nvs();
    s_version++;
    return err;
}

const probe_target_t *app_probe_get_targets(uint8_t *count)
{
    if (count) *count = s_target_count;
    return s_targets;
}

const probe_result_t *app_probe_get_results(uint8_t *count)
{
    if (count) *count = s_target_count;
    return s_results;
}

const probe_module_t *app_probe_get_module(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].name, name) == 0) {
            return &s_modules[i];
        }
    }
    return NULL;
}

bool app_probe_run_one(const char *name, probe_result_t *out)
{
    for (int i = 0; i < s_target_count; i++) {
        if (strcmp(s_targets[i].name, name) == 0) {
            const probe_module_t *mod = app_probe_get_module(s_targets[i].module_name);
            if (!mod) {
                return false;
            }
            if (s_busy[i]) {
                return false;
            }
            s_busy[i] = true;
            probe_result_t r = dispatch_probe(&s_targets[i], mod);
            s_results[i] = r;
            s_busy[i] = false;
            if (out) {
                *out = r;
            }
            return true;
        }
    }
    return false;
}
