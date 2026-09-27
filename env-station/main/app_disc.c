/* app_disc — WFP 网络发现（协议 v3，UDP :7789）。
 *
 * 品牌内部私有交互协议的板端侧：入网（拿到 IP）即开始周期广播一行
 * #HELLO（与串口/TCP 的 hello 应答同载荷，含 ip/tcp 可达性自描述），
 * homepulse 侧只需监听 :7789 就能发现**从未配置过**的板子并自动建会话；
 * 反向 PC 启动广播 {"cmd":"wfp_probe"} 时本板即时单播应答。
 * 静态配置优先：发现只补漏，不顶替平台侧既有配置。
 */
#include "app_disc.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "csi.h"

#define DISC_PORT 7789
#define BEACON_EVERY_S 10 /* 播报周期：够 PC 快速发现，又不成广播噪声 */

static const char *TAG = "disc";
static int s_sock = -1;
static volatile bool s_ip_up;

void wfp_devid(char out[13])
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
}

int wfp_hello_line(char *buf, size_t len)
{
    csi_status_t st;
    csi_station_get_status(&st);
    char id[13];
    wfp_devid(id);
    return snprintf(buf, len,
                    "#HELLO {\"devid\":\"%s\",\"proto\":3,\"fw\":\"v2.7\","
                    "\"name\":\"env-station\",\"caps\":[\"csi\",\"env\","
                    "\"wifi\",\"net\",\"display\",\"web\"],"
                    "\"ip\":\"%s\",\"tcp\":7788}",
                    id, st.ip[0] ? st.ip : "");
}

/* 单 Socket 双职责：1s 收包超时轮询——收 wfp_probe 即应答；
 * 累计超时满 BEACON_EVERY_S 拍广播一次 #HELLO。 */
static void disc_task(void *arg)
{
    (void)arg;
    char line[320];
    char rbuf[128];
    int beat = 0;
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(DISC_PORT),
        .sin_addr.s_addr = PP_HTONL(LWIP_MAKEU32(255, 255, 255, 255)),
    };
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int r = recvfrom(s_sock, rbuf, sizeof(rbuf) - 1, 0,
                         (struct sockaddr *)&from, &flen);
        if (r > 0) {
            rbuf[r] = 0;
            if (s_ip_up && strstr(rbuf, "wfp_probe")) {
                int n = wfp_hello_line(line, sizeof(line));
                sendto(s_sock, line, n, 0, (struct sockaddr *)&from, flen);
            }
        }
        if (s_ip_up && ++beat >= BEACON_EVERY_S) {
            beat = 0;
            int n = wfp_hello_line(line, sizeof(line));
            int sent = sendto(s_sock, line, n, 0,
                              (struct sockaddr *)&dst, sizeof(dst));
            if (sent < 0) {
                ESP_LOGW(TAG, "播报发送失败 errno=%d", errno);
            }
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == IP_EVENT_STA_GOT_IP) {
        if (!s_ip_up) {
            s_ip_up = true;
            ESP_LOGI(TAG, "IP 到手，开始 :%d 周期播报（%ds）", DISC_PORT,
                     BEACON_EVERY_S);
        }
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_ip_up = false;
    }
}

void app_disc_init(void)
{
    /* 事件循环由 csi_station_init 先建好（app_main 顺序保证） */
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               on_ip_event, NULL));
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_ERROR_CHECK(s_sock < 0 ? ESP_FAIL : ESP_OK);
    int on = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DISC_PORT),
        .sin_addr.s_addr = PP_HTONL(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "UDP :%d 绑定失败 errno=%d", DISC_PORT, errno);
        return;
    }
    xTaskCreate(disc_task, "disc", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "发现播报器就绪（:7789，应答 wfp_probe）");
}
