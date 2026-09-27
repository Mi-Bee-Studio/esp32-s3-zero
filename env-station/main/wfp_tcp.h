#pragma once
#include <stdbool.h>

/* WFP TCP 端点（端口约定 7788，见 homepulse/docs/wfp-protocol.md）：
 * WiFi 在网后 homepulse 可直连——板子脱 USB 仅供电时的常态链路。
 * 与 USB 串口同一份文本行协议；单客户端，后连拒绝即排他。 */

void wfp_tcp_init(void);      /* app_main 调一次：起服务任务 */
void wfp_net_up(bool up);     /* GOT_IP 置 true / WiFi 断开置 false */
void wfp_out(const char *fmt, ...); /* 协议行双宿输出：USB + 已连 TCP 客户端 */
