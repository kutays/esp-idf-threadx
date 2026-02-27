/*
 * freertos/FreeRTOSConfig.h — Redirect for ESP-IDF components that include
 * with the "freertos/" prefix (e.g., esp_task.h does #include "freertos/FreeRTOSConfig.h").
 * The actual config lives one directory up where the upstream compat layer expects it.
 */
#include "../FreeRTOSConfig.h"
