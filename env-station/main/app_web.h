#pragma once

/* 板端 Web 维护页（app_web.c）：:80 —— 状态 / WiFi 配网 / OTA 刷机 / 重启。
 * WiFi 在网时用 STA IP 访问；未配网或掉线时 SoftAP "env-station"（见 csi.c）
 * 下访问 192.168.4.1，永远保留一条救援通道。 */
void app_web_init(void);
