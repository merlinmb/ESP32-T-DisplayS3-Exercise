idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "../include" ".."
    REQUIRES
        esp_event
        esp_http_client
        esp-tls
        esp_lcd
        esp_netif
        esp_wifi
        freertos
        nvs_flash
        driver
)
