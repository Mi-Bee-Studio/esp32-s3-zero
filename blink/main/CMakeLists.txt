idf_component_register(SRCS "main.c" "app_web.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip
                                     esp_http_server app_update esp_timer
                                     esp_driver_gpio cjson)
