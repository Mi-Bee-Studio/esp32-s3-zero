/* probe_types.h — 网络探测（blackbox 能力）公共类型。
 *
 * 自包含：类型定义与探测函数声明合一（上游 esp32-blackbox 的
 * config_manager.h 类型部分 + probe_types.h 合并，去掉其 WiFi/SPIFFS
 * 耦合——env-station 的 WiFi/存储由 csi.c/app_web 自有体系承担）。
 * 探测实现 probe_http/tcp/dns/icmp/ws.c 与上游逐字一致，include 本头。
 */
#ifndef PROBE_TYPES_H
#define PROBE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* 探测模块类型（与 blackbox_exporter prober 对应） */
typedef enum {
    MODULE_HTTP,       /* HTTP */
    MODULE_HTTPS,      /* HTTPS */
    MODULE_TCP,        /* TCP 连接 */
    MODULE_TCP_TLS,    /* TCP+TLS 握手 */
    MODULE_DNS,        /* DNS 解析 */
    MODULE_ICMP,       /* ICMP ping */
    MODULE_WS,         /* WebSocket */
    MODULE_WSS,        /* WebSocket Secure */
} probe_module_type_t;

#define MAX_MODULES   16
#define MAX_TARGETS   32
#define MAX_HEADERS   4

typedef struct {
    char method[8];
    uint16_t valid_status_codes[8];
    uint8_t valid_status_count;
    bool no_follow_redirects;
    char headers[MAX_HEADERS][64];
    uint8_t header_count;
} http_module_config_t;

typedef struct {
    bool tls;
    char query[64];
    char expected_response[64];
} tcp_module_config_t;

typedef struct {
    char query_name[128];
    uint8_t query_type;   /* 1=A, 28=AAAA 等 */
} dns_module_config_t;

typedef struct {
    uint8_t packets;
    uint16_t payload_size;
    uint8_t pattern;
} icmp_module_config_t;

typedef union {
    http_module_config_t http;
    tcp_module_config_t tcp;
    dns_module_config_t dns;
    icmp_module_config_t icmp;
} module_config_union_t;

typedef struct {
    probe_module_type_t type;
    uint32_t timeout_ms;
    module_config_union_t config;
} probe_module_config_t;

typedef struct {
    char name[32];
    probe_module_config_t config;
} probe_module_t;

typedef struct {
    char name[64];
    char target[256];
    uint16_t port;
    uint32_t interval_ms;
    char module_name[32];
} probe_target_t;

/* 探测结果（上游同构） */
typedef struct {
    bool success;
    uint32_t duration_ms;
    int status_code;
    char error_msg[128];

    union {
        struct {
            uint32_t connect_time_ms;
            uint32_t tls_time_ms;
            uint32_t ttfb_ms;
            int http_status;
        } http;
        struct {
            uint32_t connect_time_ms;
            uint32_t tls_time_ms;
        } tcp;
        struct {
            uint32_t resolve_time_ms;
            char resolved_ip[16];
        } dns;
        struct {
            uint32_t rtt_ms;
            uint8_t packets_sent;
            uint8_t packets_received;
        } icmp;
        struct {
            uint32_t connect_time_ms;
            uint32_t tls_time_ms;
            uint32_t handshake_time_ms;
        } ws;
    } details;
} probe_result_t;

/* 探测实现（probe_*.c，与上游 esp32-blackbox 逐字一致） */
probe_result_t probe_http_execute(const probe_target_t *target, const probe_module_config_t *module_config);
probe_result_t probe_https_execute(const probe_target_t *target, const probe_module_config_t *module_config);
probe_result_t probe_tcp_execute(const probe_target_t *target, const probe_module_config_t *module_config);
probe_result_t probe_tcp_tls_execute(const probe_target_t *target, const probe_module_config_t *module_config);
probe_result_t probe_dns_execute(const probe_target_t *target, const probe_module_config_t *module_config);
probe_result_t probe_icmp_execute(const probe_target_t *target, const probe_module_config_t *module_config);
probe_result_t probe_ws_execute(const probe_target_t *target, const probe_module_config_t *module_config);
probe_result_t probe_wss_execute(const probe_target_t *target, const probe_module_config_t *module_config);

#endif
