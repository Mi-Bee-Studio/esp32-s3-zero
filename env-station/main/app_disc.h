#pragma once

#include <stddef.h>

/* WFP 网络发现（协议 v3，UDP :7789）——板端入网主动播报：
 * 拿到 IP 后每 10s 广播一行 #HELLO（含 ip/tcp），PC 监听即识别未知设备；
 * 收到 PC 广播 {"cmd":"wfp_probe"} 立即单播应答（加速冷启动发现）。 */
void app_disc_init(void);

/* #HELLO 载荷（发现播报与 hello 命令应答同源），返回行长 */
int wfp_hello_line(char *buf, size_t len);

/* 芯片唯一 ID（efuse MAC）派生 12 位十六进制，out 需 13 字节 */
void wfp_devid(char out[13]);
