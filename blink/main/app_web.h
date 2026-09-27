#pragma once

/* 板端 Web 维护页（app_web.c）：:80 —— 状态 / WiFi 配网 / OTA 刷机 / 重启。
 * blink 无独立 wifi 模块，本模块自带 APSTA 与凭据 NVS（Web 即配网入口）：
 * 在网用 STA IP 访问；未配网时热点 "blink-s3"（密码 12345678）→ 192.168.4.1。 */
void app_web_init(void);
