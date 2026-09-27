# probe_*.c 与上游 esp32-blackbox 逐字一致（include 本地 probe_types.h）
idf_component_register(SRCS "main.c" "dht.c" "ssd1306.c" "csi.c" "wfp_tcp.c"
                                     "app_web.c" "app_disc.c"
                                     "app_probe.c" "probe_http.c" "probe_tcp.c"
                                     "probe_dns.c" "probe_icmp.c" "probe_ws.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip
                                     esp_driver_gpio esp_driver_i2c esp_driver_rmt
                                     esp_timer esp_driver_usb_serial_jtag
                                     esp_http_server app_update cjson
                                     esp_http_client mbedtls)
