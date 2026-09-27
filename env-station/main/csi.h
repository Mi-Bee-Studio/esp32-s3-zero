#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool connected;
    bool csi_on;
    bool streaming;
    char ssid[33];
    char ip[16];         /* STA IP（未联网为空串；SoftAP 兜底 192.168.4.1 见 csi.c） */
    int8_t rssi;         /* 最近 RSSI（dBm） */
    float rate_hz;       /* 最近一秒 CSI 帧率 */
    uint32_t csi_count;  /* 累计 CSI 帧 */
    uint32_t ping_replies;
    int pc_present;      /* homepulse sense_status 的在场判定：-1 未知 0 无 1 有 */
    bool pc_linked;      /* PC 在链（30s 内收到过平台命令，协议 v3） */
    int local_present;   /* 端侧本地在场估计：-1 未知 0 无 1 有（PC 不在链时权威） */
    float motion;        /* 本地估计的运动量 EWMA（诊断/调阈值用） */
    float mot_enter;     /* 本地估计双阈值（增强协议 sense_cfg 可下发覆盖） */
    float mot_exit;
} csi_status_t;

/* 初始化 netif/event/WiFi STA；NVS 有凭据则自动关联 */
void csi_station_init(void);

/* 命令下发凭据：存 NVS 并立即关联。成功返回 true。 */
bool csi_station_set_wifi(const char *ssid, const char *pass);

/* homepulse 协议行入口（JSON：hello/sense_start/sense_stop/sense_status） */
void csi_on_cmd(const char *json_line);

/* 板载按键：暂停/恢复 #S1 串流（等同 sense_stop/sense_start 的流开关） */
void csi_stream_toggle(void);

/* PC 链路判活（协议 v3）：csi_on_cmd 每识别一条平台命令自动续链 */
void csi_note_pc(void);
bool csi_pc_linked(void);

void ping_restart(int hz);

void csi_station_get_status(csi_status_t *out);
