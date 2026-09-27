/* wfp_tcp.c: 板端 WFP TCP 端点 + 协议行双宿输出。
 * 服务生命周期随 WiFi：GOT_IP 起 listen，断网收口；协议行（#S1/#ENV/
 * 命令应答）经 wfp_out 同时写 USB 串口与已连接的 TCP 客户端，两个
 * 入口（USB 控制台 / TCP 行）共用 cmd_dispatch。 */
#include "wfp_tcp.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "main.h"

static const char *TAG = "wfp_tcp";
#define WFP_TCP_PORT 7788

static int s_srv = -1;
static int s_cli = -1;
static volatile bool s_want;      /* WiFi 在网 */
static SemaphoreHandle_t s_tx_mu; /* wfp_out 并发写 TCP；同时守护 s_cli 生命周期 */

/* cli_close: 收口当前客户端。调用方必须已持 s_tx_mu —— s_cli 会被
 * tcp_task（accept 顶替/断开）、wfp_out 各调用任务（写失败收口）、
 * WiFi 事件（wfp_net_up）三方触达，曾经无锁：两条路径并发时按读到
 * 的旧值各 close 一次，期间 lwIP 把该 fd 号复用给 httpd 的新套接字，
 * 第二次 close 就误杀了它 —— 现场：httpd accept 报 EBADF(23) 后永久
 * 失灵而 WiFi/CSI/USB 一切正常（2026-09-27 排障实录）。 */
static void cli_close(void)
{
    if (s_cli >= 0) {
        close(s_cli);
        s_cli = -1;
    }
}

void wfp_net_up(bool up)
{
    s_want = up;
    if (!up && s_tx_mu != NULL) {
        if (xSemaphoreTake(s_tx_mu, pdMS_TO_TICKS(1000)) == pdTRUE) {
            cli_close();
            xSemaphoreGive(s_tx_mu);
        }
    }
}

void wfp_out(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    printf("%.*s\n", n, buf); /* USB 侧（serialtap 照常采集） */
    /* 2026-09-27 03:01 事故加固：对端不读时 TCP write 曾无限阻塞并持互斥，
     * 拖死 USB 遥测/DHT（DHT 任务因此挂狗重启）。现在互斥限时获取，拿不到
     * 就丢弃本行的 TCP 侧（USB 已发），发送超时由 SO_SNDTIMEO 兜底。 */
    if (s_tx_mu != NULL) {
        if (xSemaphoreTake(s_tx_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
            return;
        }
    }
    if (s_cli >= 0) {
        buf[n] = '\n';
        if (write(s_cli, buf, n + 1) < 0) {
            cli_close(); /* 客户端已死/不读（SNDTIMEO 超时）：收口，accept 等重连 */
        }
    }
    if (s_tx_mu != NULL) {
        xSemaphoreGive(s_tx_mu);
    }
}

static void tcp_task(void *arg)
{
    (void)arg;
    for (;;) {
        while (!s_want) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (s < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        struct sockaddr_in sa = { 0 };
        sa.sin_family = AF_INET;
        sa.sin_port = htons(WFP_TCP_PORT);
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(s, 1) != 0) {
            ESP_LOGE(TAG, "bind/listen :%d 失败 errno=%d", WFP_TCP_PORT, errno);
            close(s);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int nb = 1;
        ioctl(s, FIONBIO, &nb); /* 非阻塞 accept：断网时能退出收口 */
        s_srv = s;
        ESP_LOGI(TAG, "WFP TCP 端点 :%d 就绪（homepulse 可脱串口直连）", WFP_TCP_PORT);
        while (s_want) {
            struct sockaddr_in ca;
            socklen_t cl = sizeof(ca);
            int c = accept(s, (struct sockaddr *)&ca, &cl);
            if (c < 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            ESP_LOGI(TAG, "TCP 客户端接入（单客户端，后连顶前连）");
            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            /* 发送同样限时 1s：对端不读（读者挂死）时 write 报错而非永久阻塞 */
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            /* 顶替前收口旧 fd（防 socket 表泄漏）：s_cli 生命周期全程持锁 */
            if (xSemaphoreTake(s_tx_mu, pdMS_TO_TICKS(1000)) == pdTRUE) {
                cli_close();
                s_cli = c;
                xSemaphoreGive(s_tx_mu);
            } else {
                close(c); /* 拿不到锁（极端）：宁可放弃本连接也不留下竞态 */
                continue;
            }
            char line[256];
            size_t ln = 0;
            while (s_want) {
                uint8_t ch;
                int r = recv(c, &ch, 1, 0);
                if (r < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;
                    }
                    break;
                }
                if (r == 0) {
                    break;
                }
                if (ch == '\n' || ch == '\r') {
                    if (ln > 0) {
                        line[ln] = 0;
                        cmd_dispatch(line); /* 与 USB 控制台同一分发 */
                        ln = 0;
                    }
                    continue;
                }
                if (ln < sizeof(line) - 1) {
                    line[ln++] = ch;
                }
            }
            if (xSemaphoreTake(s_tx_mu, pdMS_TO_TICKS(1000)) == pdTRUE) {
                cli_close(); /* 客户端已断开：收口（若 wfp_out 已收过则空操作） */
                xSemaphoreGive(s_tx_mu);
            }
            ESP_LOGI(TAG, "TCP 客户端断开");
        }
        close(s);
        s_srv = -1;
        ESP_LOGI(TAG, "WiFi 离网，TCP 端点收口");
    }
}

void wfp_tcp_init(void)
{
    s_tx_mu = xSemaphoreCreateMutex();
    xTaskCreate(tcp_task, "wfp_tcp", 4096, NULL, 4, NULL);
}
