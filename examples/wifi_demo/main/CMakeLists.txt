idf_component_register(
    SRCS "main.c"
    REQUIRES threadx esp_wifi esp_event esp_netif nvs_flash log
)
