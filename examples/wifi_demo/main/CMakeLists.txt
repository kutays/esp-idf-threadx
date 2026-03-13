# SPDX-License-Identifier: Apache-2.0
idf_component_register(
    SRCS "main.c"
    REQUIRES threadx esp_wifi esp_event esp_netif esp_timer nvs_flash log
)
