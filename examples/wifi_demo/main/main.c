// SPDX-License-Identifier: Apache-2.0
/*
 * WiFi STA demo — connects to an access point using ThreadX as the RTOS.
 *
 * All FreeRTOS calls from esp_wifi, esp_event, lwip etc. are transparently
 * routed through the ThreadX FreeRTOS compatibility layer via the freertos
 * component override.
 *
 * This demo also creates a background ThreadX thread that periodically scans
 * for nearby WiFi networks and prints results. This tests:
 *   - ThreadX multi-thread scheduling alongside WiFi
 *   - WiFi scan API (esp_wifi_scan_start/get_ap_records)
 *   - FreeRTOS event group (xEventGroupWaitBits) via compat layer
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "tx_api.h"

static const char *TAG = "wifi_demo";

/* Set your WiFi credentials here or via menuconfig */
#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID "your_ssid"
#endif
#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD "your_password"
#endif

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           5

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

/* Scan-done semaphore — signaled by WIFI_EVENT_SCAN_DONE handler */
static TX_SEMAPHORE scan_done_sem;

/* ── Diagnostic: does esp_timer work? ──────────────────────────── */
static void diag_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "[diag] esp_timer fired! tick=%lu", (unsigned long)tx_time_get());
}

/* ── WiFi scanner thread (ThreadX native) ──────────────────────── */

#define SCANNER_STACK_SIZE  4096
#define SCANNER_PRIORITY    20      /* Lower priority than main (16) */
#define SCAN_INTERVAL_MS    15000
#define SCAN_INTERVAL_TICKS pdMS_TO_TICKS(SCAN_INTERVAL_MS)
#define MAX_SCAN_RESULTS    20

static TX_THREAD scanner_thread;
static UCHAR scanner_stack[SCANNER_STACK_SIZE];

static const char *auth_mode_str(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3-PSK";
    default:                        return "OTHER";
    }
}

static void scanner_thread_entry(ULONG param)
{
    (void)param;
    uint32_t scan_num = 0;

    ESP_LOGI(TAG, "[scanner] Background WiFi scanner started (ThreadX thread, tick=%lu)",
             (unsigned long)tx_time_get());

    /* Short delay for esp_wifi_start() to fully initialize */
    tx_thread_sleep(200);  /* 2 seconds */

    while (1) {
        scan_num++;
        ESP_LOGI(TAG, "[scanner] === Scan #%lu starting (tick=%lu) ===",
                 (unsigned long)scan_num, (unsigned long)tx_time_get());

        /* Non-blocking scan + wait for SCAN_DONE event.
         * We use non-blocking because blocking mode has a known issue
         * (internal semaphore not posted through compat layer). Instead:
         * start scan, wait for WIFI_EVENT_SCAN_DONE via our semaphore,
         * then collect results. */
        wifi_scan_config_t scan_config = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,          /* All channels */
            .show_hidden = true,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active.min = 100,
            .scan_time.active.max = 300,
        };

        /* Drain any stale semaphore counts from previous scans */
        while (tx_semaphore_get(&scan_done_sem, TX_NO_WAIT) == TX_SUCCESS) {}

        esp_err_t err = esp_wifi_scan_start(&scan_config, false /* non-blocking */);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[scanner] Scan failed: %s", esp_err_to_name(err));
            tx_thread_sleep(SCAN_INTERVAL_TICKS);
            continue;
        }

        /* Wait for WIFI_EVENT_SCAN_DONE (timeout 10s).
         * Active scan: 13 channels × 300ms max = ~4s + overhead.
         * On timeout, still try to read results (diagnostic). */
        UINT sem_status = tx_semaphore_get(&scan_done_sem, 1000);
        if (sem_status != TX_SUCCESS) {
            ESP_LOGW(TAG, "[scanner] No SCAN_DONE in 10s — reading results anyway (diagnostic)");
        } else {
            ESP_LOGI(TAG, "[scanner] SCAN_DONE received at tick=%lu",
                     (unsigned long)tx_time_get());
        }

        /* Get results */
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);

        if (ap_count == 0) {
            ESP_LOGW(TAG, "[scanner] No networks found (ap_count=0)");
            tx_thread_sleep(SCAN_INTERVAL_TICKS);
            continue;
        }

        uint16_t fetch_count = (ap_count > MAX_SCAN_RESULTS) ? MAX_SCAN_RESULTS : ap_count;
        wifi_ap_record_t *ap_records = malloc(fetch_count * sizeof(wifi_ap_record_t));
        if (ap_records == NULL) {
            ESP_LOGE(TAG, "[scanner] malloc failed for %u records", fetch_count);
            esp_wifi_scan_get_ap_records(&fetch_count, NULL);  /* Clear scan results */
            tx_thread_sleep(SCAN_INTERVAL_TICKS);
            continue;
        }

        esp_wifi_scan_get_ap_records(&fetch_count, ap_records);

        ESP_LOGI(TAG, "[scanner] Found %u networks (showing %u):", ap_count, fetch_count);
        ESP_LOGI(TAG, "[scanner]  %-32s  %4s  %-15s  %s", "SSID", "RSSI", "Auth", "Channel");
        ESP_LOGI(TAG, "[scanner]  %-32s  %4s  %-15s  %s", "----", "----", "----", "-------");

        for (int i = 0; i < fetch_count; i++) {
            ESP_LOGI(TAG, "[scanner]  %-32s  %4d  %-15s  %d",
                     ap_records[i].ssid,
                     ap_records[i].rssi,
                     auth_mode_str(ap_records[i].authmode),
                     ap_records[i].primary);
        }

        free(ap_records);

        ESP_LOGI(TAG, "[scanner] Next scan in %d seconds...",
                 SCAN_INTERVAL_MS / 1000);

        /* Sleep — lets other ThreadX threads run */
        tx_thread_sleep(SCAN_INTERVAL_TICKS);
    }
}

/* ── WiFi event handlers ───────────────────────────────────────── */

/* Debug: log ALL WiFi events to understand event dispatch timing */
static void wifi_any_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_SCAN_DONE) {
        wifi_event_sta_scan_done_t *info = (wifi_event_sta_scan_done_t *)event_data;
        ESP_LOGI(TAG, "[event] SCAN_DONE status=%lu count=%u tick=%lu",
                 (unsigned long)info->status, info->number,
                 (unsigned long)tx_time_get());
        tx_semaphore_put(&scan_done_sem);
    } else {
        ESP_LOGI(TAG, "[event] WIFI id=%ld tick=%lu",
                 (long)event_id, (unsigned long)tx_time_get());
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "[event] STA_START — calling esp_wifi_connect()");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "[event] Retry #%d connecting to AP...", s_retry_num);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGI(TAG, "[event] Failed to connect after %d attempts", MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "[event] Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else {
        ESP_LOGI(TAG, "[event] base=%s id=%ld", event_base, (long)event_id);
    }
}

/* ── WiFi STA init ─────────────────────────────────────────────── */

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Create scan-done semaphore (initial count 0) */
    tx_semaphore_create(&scan_done_sem, "scan_done", 0);

    /* Register handler for ALL WiFi events — diagnostic + SCAN_DONE semaphore */
    esp_event_handler_instance_t wifi_event_instance;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_any_event_handler, NULL, &wifi_event_instance));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    /* ── Connection code (commented out for scan-only mode) ───────
     * Uncomment the event handlers and esp_wifi_set_config + connect
     * below to enable STA connection. Set real SSID/password first. */

    //ESP_ERROR_CHECK(esp_event_handler_instance_register(
    //    WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    //ESP_ERROR_CHECK(esp_event_handler_instance_register(
    //    IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    //wifi_config_t wifi_config = {
    //    .sta = {
    //        .ssid = CONFIG_WIFI_SSID,
    //        .password = CONFIG_WIFI_PASSWORD,
    //        .threshold.authmode = WIFI_AUTH_WPA2_PSK,
    //    },
    //};

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    //ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA init complete (scan-only mode, no connection)");
}

/* ── app_main ──────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "=== WiFi Demo on ThreadX ===");
    ESP_LOGI(TAG, "ThreadX version: %u", (unsigned)THREADX_MAJOR_VERSION);

    /* Initialize NVS — required by WiFi driver */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Diagnostic: esp_timer hardware state.
     * esp_timer uses SYSTIMER alarm 2 → SYSTIMER_TARGET2 → interrupt source 59.
     * Check if the interrupt is properly routed and enabled. */
    {
        /* INTMTX: source 59 → which CPU line? */
        volatile uint32_t *intmtx_59 = (volatile uint32_t *)(0x60010000 + 59 * 4);
        uint32_t cpu_line = *intmtx_59;

        /* PLIC ENABLE bitmask */
        volatile uint32_t *plic_enable = (volatile uint32_t *)0x20001000;
        uint32_t enable_val = *plic_enable;

        /* mie CSR */
        uint32_t mie_val;
        __asm__ volatile("csrr %0, mie" : "=r"(mie_val));

        ESP_LOGI(TAG, "[diag] INTMTX src59 (esp_timer) → CPU line %lu", (unsigned long)cpu_line);
        ESP_LOGI(TAG, "[diag] PLIC ENABLE = 0x%08lx (line %lu bit = %lu)",
                 (unsigned long)enable_val, (unsigned long)cpu_line,
                 (unsigned long)((enable_val >> cpu_line) & 1));
        ESP_LOGI(TAG, "[diag] mie = 0x%08lx (line %lu bit = %lu)",
                 (unsigned long)mie_val, (unsigned long)cpu_line,
                 (unsigned long)((mie_val >> cpu_line) & 1));

        /* SYSTIMER INT_ENA for alarm 2 (bit 2) */
        volatile uint32_t *systimer_int_ena = (volatile uint32_t *)(0x6000A000 + 0x64);
        ESP_LOGI(TAG, "[diag] SYSTIMER INT_ENA = 0x%08lx (bit2=alarm2=%lu)",
                 (unsigned long)*systimer_int_ena, (unsigned long)((*systimer_int_ena >> 2) & 1));
    }

    /* Also test esp_timer from our (post-ThreadX) context */
    esp_timer_handle_t diag_timer;
    esp_timer_create_args_t diag_args = {
        .callback = diag_timer_cb,
        .name = "diag",
    };
    ESP_ERROR_CHECK(esp_timer_create(&diag_args, &diag_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(diag_timer, 2000000)); /* 2 seconds */
    ESP_LOGI(TAG, "Diagnostic esp_timer started (should fire every 2s)");

    ESP_LOGI(TAG, "NVS initialized, starting WiFi...");
    wifi_init_sta();

    /* Create background scanner thread (ThreadX native API).
     * This runs concurrently with the WiFi connection attempt. */
    UINT status = tx_thread_create(
        &scanner_thread,
        "wifi_scan",
        scanner_thread_entry,
        0,
        scanner_stack,
        SCANNER_STACK_SIZE,
        SCANNER_PRIORITY,       /* Lower priority number = higher priority in ThreadX */
        SCANNER_PRIORITY,       /* Preemption threshold = same as priority */
        TX_NO_TIME_SLICE,
        TX_AUTO_START
    );
    if (status != TX_SUCCESS) {
        ESP_LOGE(TAG, "Failed to create scanner thread: %u", status);
    } else {
        ESP_LOGI(TAG, "Scanner thread created (ThreadX prio %d)", SCANNER_PRIORITY);
    }

    /* Main loop — print ThreadX tick + WiFi event status every 3 seconds.
     * This proves ThreadX scheduling is alive alongside WiFi. */
    while (1) {
        TX_THREAD *current = tx_thread_identify();
        EventBits_t bits = 0;
        if (s_wifi_event_group) {
            bits = xEventGroupGetBits(s_wifi_event_group);
        }
        ESP_LOGI(TAG, "[main] tick=%lu thread='%s' wifi_bits=0x%lx retries=%d",
                 (unsigned long)tx_time_get(),
                 current ? current->tx_thread_name : "?",
                 (unsigned long)bits,
                 s_retry_num);
        tx_thread_sleep(300);  /* 3 seconds at 100 Hz */
    }
}
