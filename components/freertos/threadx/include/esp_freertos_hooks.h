/*
 * esp_freertos_hooks.h — Stub hooks API for ESP-IDF components that
 * register idle/tick hooks. Under ThreadX these are no-ops.
 */

#ifndef ESP_FREERTOS_HOOKS_H
#define ESP_FREERTOS_HOOKS_H

#include "esp_err.h"

typedef bool (*esp_freertos_idle_cb_t)(void);
typedef void (*esp_freertos_tick_cb_t)(void);

static inline esp_err_t esp_register_freertos_idle_hook_for_cpu(esp_freertos_idle_cb_t cb, UBaseType_t cpu) {
    (void)cb; (void)cpu; return ESP_OK;
}
static inline esp_err_t esp_register_freertos_idle_hook(esp_freertos_idle_cb_t cb) {
    (void)cb; return ESP_OK;
}
static inline esp_err_t esp_register_freertos_tick_hook_for_cpu(esp_freertos_tick_cb_t cb, UBaseType_t cpu) {
    (void)cb; (void)cpu; return ESP_OK;
}
static inline esp_err_t esp_register_freertos_tick_hook(esp_freertos_tick_cb_t cb) {
    (void)cb; return ESP_OK;
}
static inline void esp_deregister_freertos_idle_hook_for_cpu(esp_freertos_idle_cb_t cb, UBaseType_t cpu) {
    (void)cb; (void)cpu;
}
static inline void esp_deregister_freertos_idle_hook(esp_freertos_idle_cb_t cb) {
    (void)cb;
}
static inline void esp_deregister_freertos_tick_hook_for_cpu(esp_freertos_tick_cb_t cb, UBaseType_t cpu) {
    (void)cb; (void)cpu;
}
static inline void esp_deregister_freertos_tick_hook(esp_freertos_tick_cb_t cb) {
    (void)cb;
}

#endif /* ESP_FREERTOS_HOOKS_H */
