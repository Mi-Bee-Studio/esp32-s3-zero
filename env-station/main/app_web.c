/* app_web — 板端 Web 维护页（前后端一体，:80）。
 *
 * 后端 esp_http_server：GET /（单页）、GET /api/status、POST /api/wifi（配网，
 * 复用 csi_station_set_wifi）、POST /api/reboot、POST /ota（固件流式写入
 * 备用 OTA 槽 → esp_ota_end 校验 → 切启动槽 → 延迟 1s 重启；校验失败不切槽，
 * 原固件无损）。
 * 前端为下方内嵌单页 HTML：状态卡 / 配网表单 / 固件上传（XHR 进度条）/ 重启。
 *
 * 设计要点：
 * - 上传走 httpd 自身任务，收包缓冲放静态区（httpd 栈 8K，勿放大栈缓冲）；
 * - esp_ota_write 期间仅短暂关 cache，主/DHT 任务照常喂狗，与看门狗无耦合；
 * - 响应先发完再 esp_restart（定时器延迟），否则浏览器拿到的是断连。
 */
#include "app_probe.h"
#include "app_web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "csi.h"
#include "main.h"

#define APP_FW_VERSION "v3.0-blackbox"

static const char *TAG = "web";
static httpd_handle_t s_server;

/* 前端单页（属性用单引号，避免 C 字符串转义；发送用 sendstr，不做格式化） */
static const char PAGE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>env-station</title><style>"
"body{font-family:sans-serif;max-width:440px;margin:12px auto;padding:0 12px;"
"background:#111;color:#eee}h1{font-size:18px}small{color:#888}"
".card{background:#1b1b1b;border-radius:10px;padding:12px;margin:10px 0}"
".card b{display:block;margin-bottom:6px}"
"input{width:100%;box-sizing:border-box;padding:9px;margin:4px 0;border-radius:8px;"
"border:1px solid #3a3a3a;background:#242424;color:#eee;font-size:15px}"
"button{background:#2d6cdf;color:#fff;border:0;border-radius:8px;padding:9px 16px;"
"font-size:15px;margin-top:6px}#bar{height:10px;background:#333;border-radius:5px;"
"overflow:hidden;margin-top:8px}#fill{height:100%;width:0;background:#2d6cdf;"
"transition:width .2s}#msg{margin-top:8px;font-size:13px;color:#9c9}"
"textarea{width:100%;box-sizing:border-box;background:#242424;color:#eee;"
"border:1px solid #3a3a3a;border-radius:8px;font-size:12px;padding:8px;"
"font-family:monospace;margin:4px 0}"
"</style></head><body><h1>env-station <small id='fw'></small></h1>"
"<div class='card' id='st'>加载中…</div>"
"<div class='card'><b>WiFi 配网</b>"
"<input id='ssid' placeholder='SSID'><input id='pass' placeholder='密码' type='password'>"
"<button onclick='wifiSave()'>保存并连接</button></div>"
"<div class='card'><b>网络探测（blackbox）</b>"
"<div id='pb' style='font-size:13px'>加载中…</div>"
"<textarea id='pjson' rows='8' spellcheck='false'></textarea>"
"<button onclick='probeSave()'>保存探测配置（热加载）</button>"
"<small>Prometheus: /metrics 与 /probe?target=X&module=Y</small></div>"
"<div class='card'><b>固件更新（OTA）</b>"
"<input type='file' id='file' accept='.bin'>"
"<button onclick='otaStart()'>上传并刷写</button>"
"<div id='bar'><div id='fill'></div></div></div>"
"<div class='card'><b>维护</b><button onclick='reboot()'>重启设备</button></div>"
"<div class='card' id='msg'></div>"
"<script>"
"const $=i=>document.getElementById(i);function msg(s){$('msg').textContent=s}"
"async function refresh(){try{const s=await(await fetch('/api/status')).json();"
"$('fw').textContent=s.fw;"
"$('st').innerHTML=(s.connected?"
"('WiFi '+s.ssid+' '+s.ip+' RSSI '+s.rssi+'<br>CSI '+s.csi_hz+'Hz'):"
"'未联网。手机连热点 <b>env-station</b>（密码 12345678）后访问 192.168.4.1')"
"+'<br>温度 '+s.t+'C 湿度 '+s.rh+'%<br>heap '+s.heap+'K · 运行 '+s.up+'s'"
";}catch(e){$('st').textContent='状态获取失败'}}"
"async function wifiSave(){if(!$('ssid').value)return msg('SSID 不能为空');"
"const r=await fetch('/api/wifi',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:$('ssid').value,pass:$('pass').value})});"
"msg(r.ok?'已保存，正在连接…查看状态卡':'保存失败：'+await r.text())}"
"function otaStart(){const f=$('file').files[0];if(!f)return msg('先选 .bin 固件文件');"
"const x=new XMLHttpRequest();x.open('POST','/ota');"
"x.upload.onprogress=e=>{$('fill').style.width=(100*e.loaded/e.total)+'%'};"
"x.onload=()=>{if(x.status==200){msg('写入成功，设备重启中…20 秒后自动刷新');"
"setTimeout(()=>location.reload(),20000)}"
"else{msg('失败 HTTP '+x.status+'：'+x.responseText)}};"
"x.onerror=()=>msg('网络错误（设备可能已在重启）');"
"msg('上传 '+f.size+' 字节…');x.send(f)}"
"async function reboot(){if(!confirm('确认重启？'))return;"
"try{await fetch('/api/reboot',{method:'POST'})}catch(e){}"
"msg('重启中…几秒后自动刷新');setTimeout(()=>location.reload(),8000)}"
"async function probeRefresh(){try{const p=await(await fetch('/api/probe')).json();"
"$('pb').innerHTML=p.results.length?p.results.map(r=>"
"r.name+' '+(r.success?('✅ '+r.duration_ms+'ms'):"
"('❌ '+(r.error||r.duration_ms+'ms')))).join('<br>'):"
"'（无目标：/probe 按需可用，或在下方 JSON 添加）';"
"if(!$('pjson').value)$('pjson').value=JSON.stringify(p.config,null,1);"
"}catch(e){}}"
"async function probeSave(){"
"try{JSON.parse($('pjson').value)}catch(e){return msg('JSON 语法错误：'+e.message)}"
"const r=await fetch('/api/probe',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:$('pjson').value});"
"msg(r.ok?'探测配置已保存，已热加载':'保存失败：'+await r.text())}"
"refresh();probeRefresh();setInterval(refresh,15000);setInterval(probeRefresh,15000);"
"</script></body></html>";

/* 延迟重启：先把 HTTP 应答发完再 esp_restart */
static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void schedule_reboot(int ms)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t cfg = {
            .callback = reboot_cb,
            .name = "webreboot",
        };
        ESP_ERROR_CHECK(esp_timer_create(&cfg, &t));
    }
    esp_timer_stop(t); /* 幂等：重复触发只取最后一次 */
    esp_timer_start_once(t, (uint64_t)ms * 1000);
}

static esp_err_t send_status(httpd_req_t *req, bool ok, const char *text)
{
    char out[160];
    snprintf(out, sizeof(out), "{\"status\":\"%s\",\"msg\":\"%s\"}",
             ok ? "ok" : "error", text ? text : "");
    httpd_resp_set_type(req, "application/json");
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    return httpd_resp_sendstr(req, out);
}

static int read_body(httpd_req_t *req, char *buf, size_t len)
{
    size_t total = 0;
    while (total < len - 1) {
        int n = httpd_req_recv(req, buf + total, len - 1 - total);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
    buf[total] = 0;
    return (int)total;
}

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, PAGE_HTML);
}

static esp_err_t h_status(httpd_req_t *req)
{
    csi_status_t st;
    csi_station_get_status(&st);
    float t = 0, rh = 0;
    env_latest(&t, &rh);
    char json[400];
    snprintf(json, sizeof(json),
             "{\"fw\":\"%s\",\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\","
             "\"rssi\":%d,\"t\":%.1f,\"rh\":%.1f,\"heap\":%u,\"up\":%lu,"
             "\"csi_hz\":%.1f,\"pc_linked\":%s,\"local_present\":%d,"
             "\"motion\":%.1f,\"mot_enter\":%.0f,\"mot_exit\":%.0f}",
             APP_FW_VERSION, st.connected ? "true" : "false", st.ssid, st.ip,
             st.rssi, t, rh, (unsigned)esp_get_free_heap_size(),
             (unsigned long)(esp_timer_get_time() / 1000000), st.rate_hz,
             st.pc_linked ? "true" : "false", st.local_present, st.motion,
             st.mot_enter, st.mot_exit);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    char body[384];
    if (read_body(req, body, sizeof(body)) <= 0) {
        return send_status(req, false, "no body");
    }
    cJSON *msg = cJSON_Parse(body);
    if (!msg) {
        return send_status(req, false, "bad json");
    }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(msg, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(msg, "pass");
    bool ok = cJSON_IsString(ssid) && ssid->valuestring[0] != '\0'
              && csi_station_set_wifi(
                     ssid->valuestring,
                     cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(msg);
    return send_status(req, ok, ok ? "saved" : "set failed");
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    schedule_reboot(500);
    return ESP_OK;
}

static esp_err_t h_ota(httpd_req_t *req)
{
    static char buf[4096]; /* 静态收包缓冲（httpd 栈 8K） */
    if (req->content_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"msg\":\"empty body\"}");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"msg\":\"no ota partition\"}");
    }
    esp_ota_handle_t ota;
    if (esp_ota_begin(part, (size_t)req->content_len, &ota) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"msg\":\"ota begin failed\"}");
    }
    ESP_LOGI(TAG, "OTA 开始：目标槽 %s，%u 字节", part->label,
             (unsigned)req->content_len);
    size_t remain = (size_t)req->content_len;
    while (remain > 0) {
        int got = httpd_req_recv(req, buf, remain > sizeof(buf) ? sizeof(buf) : remain);
        if (got <= 0) {
            esp_ota_abort(ota);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"status\":\"error\",\"msg\":\"recv failed\"}");
        }
        if (esp_ota_write(ota, buf, (size_t)got) != ESP_OK) {
            esp_ota_abort(ota);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"status\":\"error\",\"msg\":\"ota write failed\"}");
        }
        remain -= (size_t)got;
    }
    /* end 做镜像头/哈希校验，失败不切启动槽——原固件无损 */
    if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"msg\":\"image invalid\"}");
    }
    ESP_LOGI(TAG, "OTA 写入并通过校验，1s 后重启进新固件");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"msg\":\"done, rebooting\"}");
    schedule_reboot(1000);
    return ESP_OK;
}

/* ---- 网络探测端点（blackbox 能力） ---- */

#define PROMETHEUS_CT "text/plain; version=0.0.4; charset=utf-8"

/* GET /metrics — 全部目标聚合的 Prometheus 指标（静态缓冲，httpd 栈 8K） */
static esp_err_t h_metrics(httpd_req_t *req)
{
    static char buf[8192];
    int n = app_probe_metrics(buf, sizeof(buf));
    httpd_resp_set_type(req, PROMETHEUS_CT);
    return httpd_resp_send(req, buf, n);
}

/* GET /probe?target=X&module=Y[&port=P] — 按需单次探测（exporter 兼容） */
static esp_err_t h_probe_ep(httpd_req_t *req)
{
    char query[256] = {0};
    char target_val[128] = {0};
    char module_val[32] = {0};
    char port_val[8] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
        return ESP_FAIL;
    }
    bool has_target = httpd_query_key_value(query, "target", target_val,
                                            sizeof(target_val)) == ESP_OK;
    bool has_module = httpd_query_key_value(query, "module", module_val,
                                            sizeof(module_val)) == ESP_OK;
    if (!has_target || target_val[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'target'");
        return ESP_FAIL;
    }
    uint16_t port = 0;
    if (httpd_query_key_value(query, "port", port_val, sizeof(port_val)) == ESP_OK
        && port_val[0] != '\0') {
        int p = atoi(port_val);
        if (p > 0 && p <= 65535) {
            port = (uint16_t)p;
        }
    }
    const char *mod_name = (has_module && module_val[0]) ? module_val : "http_2xx";

    static char buf[2048];
    int n = app_probe_probe_metrics(target_val, port, mod_name, buf, sizeof(buf));
    if (n < 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "module not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, PROMETHEUS_CT);
    return httpd_resp_send(req, buf, n);
}

/* GET /api/probe — 配置 JSON + 各目标最近结果（维护页卡片用） */
static esp_err_t h_probe_get(httpd_req_t *req)
{
    char *cfg = app_probe_config_json();
    if (!cfg) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "serialize failed");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *cfg_obj = cJSON_Parse(cfg);
    cJSON_AddItemToObject(root, "config",
                          cfg_obj ? cfg_obj : cJSON_CreateObject());

    cJSON *results = cJSON_CreateArray();
    uint8_t n = 0;
    const probe_target_t *targets = app_probe_get_targets(&n);
    const probe_result_t *rs = app_probe_get_results(&n);
    for (int i = 0; i < n; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "name", targets[i].name);
        cJSON_AddStringToObject(r, "module", targets[i].module_name);
        cJSON_AddBoolToObject(r, "success", rs[i].success);
        cJSON_AddNumberToObject(r, "duration_ms", rs[i].duration_ms);
        cJSON_AddNumberToObject(r, "status_code", rs[i].status_code);
        if (rs[i].error_msg[0]) {
            cJSON_AddStringToObject(r, "error", rs[i].error_msg);
        }
        cJSON_AddItemToArray(results, r);
    }
    cJSON_AddItemToObject(root, "results", results);

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
    cJSON_Delete(root);
    free(cfg);
    return ESP_OK;
}

/* POST /api/probe — 提交完整探测配置 JSON（解析→校验→NVS→热加载） */
static esp_err_t h_probe_set(httpd_req_t *req)
{
    static char body[4096]; /* 静态缓冲（httpd 栈 8K）；配置上限 3.5KB */
    if (read_body(req, body, sizeof(body)) <= 0) {
        return send_status(req, false, "no body");
    }
    esp_err_t err = app_probe_config_update(body);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_status(req, false, "invalid config (see serial log)");
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_status(req, false, "config too large (>3.5KB, trim targets)");
    }
    return send_status(req, err == ESP_OK, err == ESP_OK ? "saved+reloaded" : "nvs error");
}

static void httpd_register_all(httpd_handle_t srv)
{
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_root },
        { .uri = "/api/status", .method = HTTP_GET, .handler = h_status },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot },
        { .uri = "/ota", .method = HTTP_POST, .handler = h_ota },
        { .uri = "/metrics", .method = HTTP_GET, .handler = h_metrics },
        { .uri = "/probe", .method = HTTP_GET, .handler = h_probe_ep },
        { .uri = "/api/probe", .method = HTTP_GET, .handler = h_probe_get },
        { .uri = "/api/probe", .method = HTTP_POST, .handler = h_probe_set },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
}

/* 会话收支计数（open_fn/close_fn 钩子）：leaked = open - close 持续增长
 * 即 httpd 会话泄漏；走平则泄漏在别处（wfp/dns/…）。定位 fd 泄漏用。
 * ⚠ esp_http_server 语义：设了 close_fn 就【取代】默认的 close(fd)
 * （httpd_sess.c：if (config.close_fn) close_fn(...) else close(fd)）——
 * 钩子里必须自己 close，否则每个会话漏 1 个 fd（2026-09-27 实测翻车）。 */
static volatile int s_sess_open, s_sess_close;

static esp_err_t sess_open_fn(httpd_handle_t hd, int fd)
{
    (void)hd; (void)fd;
    s_sess_open++;
    return ESP_OK;
}

static void sess_close_fn(httpd_handle_t hd, int fd)
{
    (void)hd;
    s_sess_close++;
    close(fd); /* close_fn 取代了默认关闭——不关即漏（见上） */
}

static bool httpd_boot(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 10;
    /* 泄漏根因（2026-09-27 lwIP 调试日志定位）：上游路由器每 ~5s
     * 探测 :80，连接建立后不发数据/半途消失 → 会话挂在 recv 上永不完成 →
     * 僵尸会话永久占住 lwIP 槽（默认无会话超时清理）→ ~4 分钟耗尽 32 槽。
     * lru_purge：满员时回收最久未活动会话，僵尸不再积累。 */
    cfg.lru_purge_enable = true;
    cfg.open_fn = sess_open_fn;   /* 会话收支计数（泄漏定位） */
    cfg.close_fn = sess_close_fn;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        return false;
    }
    httpd_register_all(s_server);
    ESP_LOGI(TAG, "维护页 :80 就绪（在网=STA IP；未配网=热点 env-station/12345678 → 192.168.4.1）");
    return true;
}

/* ---- Web 自愈看门狗 ----
 * 实测 httpd 的监听 socket 会在开机 ~1-2 分钟后被静默关闭（accept 报
 * EBADF(23)，WiFi/CSI/USB 全部正常，串口侧毫无征兆，起因不明——fd 表
 * 里被某个组件误 close 的可能性最大，2026-09-27 排障实录）。设备又在
 * 屋里够不着，板端 Web 是唯一救援通道，坏了只能断电重拆 —— 不可接受。
 * 因此本任务每 WEB_PROBE_S 用 loopback GET 探活，连续 2 次失败即
 * httpd_stop + 重建监听：无论死因是什么，10-20 秒内自愈。 */
#define WEB_PROBE_S 10

static bool http_alive(void)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) {
        return false; /* socket 都开不出来是系统级问题，不算 httpd 死 */
    }
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = false;
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        static const char req[] = "GET /api/status HTTP/1.0\r\n\r\n";
        if (write(s, req, sizeof(req) - 1) == (int)(sizeof(req) - 1)) {
            char rsp[64];
            int n = read(s, rsp, sizeof(rsp) - 1);
            /* 收到 HTTP 头即算活；EBADF 死态下 connect 立即被拒/无数据 */
            ok = n > 0;
        }
    }
    close(s);
    return ok;
}

static void web_watchdog_task(void *arg)
{
    (void)arg;
    int fails = 0;
    int heal_streak = 0; /* 连续自愈次数（中间无一次探活成功）。
                          * httpd_boot"成功"≠服务恢复——fd 池半耗尽时新实例
                          * 能建但秒坏，必须看探活结果，连续 3 次自愈无效
                          * 即升级 esp_restart 兜底。 */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WEB_PROBE_S * 1000));
        ESP_LOGI(TAG, "sess 收支: open=%d close=%d leaked=%d",
                 s_sess_open, s_sess_close, s_sess_open - s_sess_close);
        if (!s_server) {
            continue; /* 初始启动失败的情形：下面仍会尝试重建 */
        }
        if (http_alive()) {
            fails = 0;
            heal_streak = 0;
            continue;
        }
        if (++fails < 2) {
            continue; /* 单次失败可能是瞬时拥塞，连续两次才动刀 */
        }
        fails = 0;
        heal_streak++;
        ESP_LOGW(TAG, "loopback 探活连续失败 — 第 %d 次自愈", heal_streak);
        httpd_stop(s_server); /* 对已损坏的监听句柄是安全 no-op 语义 */
        s_server = NULL;
        httpd_boot();
        /* 重建失败 = lwIP 槽被泄漏 fd 占满（socket() EBADF）；重建"成功"
         * 也可能秒坏（半耗尽态）。两者都无法在 httpd 层恢复——连续 3 次
         * 自愈无效就重启设备：~20s 中断、全自动、感知自动回链，胜过 Web
         * 永久失灵后断电重拆。 */
        if (heal_streak >= 3) {
            ESP_LOGW(TAG, "自愈升级：连续 3 次无效，重启设备兜底恢复");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
}

void app_web_init(void)
{
    if (!httpd_boot()) {
        ESP_LOGE(TAG, "httpd 启动失败");
    }
    xTaskCreate(web_watchdog_task, "web_wdg", 3584, NULL, 3, NULL);
}
