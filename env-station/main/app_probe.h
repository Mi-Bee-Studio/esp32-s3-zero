/* app_probe — 网络探测能力（esp32-blackbox 移植，v3.0）。
 *
 * 与上游的差异（资源与归属适配）：
 * - 配置后端 SPIFFS → NVS（namespace "probe" / key "cfg"，JSON 字符串，
 *   上限 ~3.5KB——本板 4MB flash 被 OTA 双槽占满，无 storage 分区）；
 * - 指标端点不另起 :9090，挂在 app_web 维护页 :80（/metrics、/probe）；
 * - 探测任务不挂 TWDT（主循环看门狗保持 5s 灵敏；探测自身有
 *   socket/HTTP 超时上限，模块超时 1-120s）；
 * - WiFi 未关联时空转（CSI 激励/判活等原有能力不受影响）。
 *
 * JSON 配置 schema 与上游 blackbox 完全一致（modules/targets/
 * scrape_interval；metrics_port 字段接受但忽略——端口恒为 :80）。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "probe_types.h"

/* 初始化：NVS 加载配置（无/坏→工厂默认并回存），起探测任务 */
void app_probe_init(void);

/* Prometheus 文本（全部目标聚合）写入 buf，返回长度；size 不足时安全截断 */
int app_probe_metrics(char *buf, int size);

/* 按需探测任意 host（blackbox_exporter /probe 兼容）：
 * 成功返回指标文本长度；module 不存在返回 -1。 */
int app_probe_probe_metrics(const char *host, int port, const char *module,
                            char *buf, int size);

/* 当前配置 JSON（malloc 分配，调用方 free）；schema 同上游 */
char *app_probe_config_json(void);

/* 提交新配置（JSON）：解析→校验→NVS 持久→版本++ 热加载。
 * 解析/校验失败不改现有配置，返回错误码。 */
esp_err_t app_probe_config_update(const char *json);

/* 重载 NVS 配置（热加载） */
esp_err_t app_probe_config_reload(void);

/* 恢复出厂探测配置（4 个标准模块、0 个目标）并持久化 */
esp_err_t app_probe_config_reset(void);

/* 控制台/状态查询 */
const probe_target_t *app_probe_get_targets(uint8_t *count);
const probe_result_t *app_probe_get_results(uint8_t *count);
const probe_module_t *app_probe_get_module(const char *name);

/* 立即探测一个已配置目标（控制台 `probe run <name>`） */
bool app_probe_run_one(const char *name, probe_result_t *out);
