idf_component_register(
    SRCS "main.c" "jkbms.c" "app_eth.c" "app_mqtt.c" "app_ota.c"
         "app_config.c" "app_web.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "ca_cert.pem" "config_page.html"
    PRIV_REQUIRES
        driver esp_timer esp_netif esp_eth lwip esp_event mqtt
        esp_http_server nvs_flash mbedtls
        esp_https_ota app_update
)
