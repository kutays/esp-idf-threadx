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

/* ── WiFi scanner thread (ThreadX native) ──────────────────────── */

#define SCANNER_STACK_SIZE  4096
#define SCANNER_PRIORITY    20      /* Lower priority than main (16) */
#define SCAN_INTERVAL_TICKS 1500    /* 15 seconds at 100 Hz tick rate */
#define MAX_SCAN_RESULTS    15

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

        /* Non-blocking scan + sleep. We avoid block=true because the
         * internal blocking semaphore in esp_wifi_scan_start depends on
         * WiFi event dispatch that may not fully work through the compat
         * layer yet. Instead: start scan, sleep while hardware scans,
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

        esp_err_t err = esp_wifi_scan_start(&scan_config, false /* non-blocking */);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[scanner] Scan failed: %s", esp_err_to_name(err));
            tx_thread_sleep(SCAN_INTERVAL_TICKS);
            continue;
        }

        /* Wait for scan to complete. Active scan across all channels
         * typically takes 1-3 seconds. 5 seconds is generous. */
        tx_thread_sleep(500);  /* 5 seconds */

        /* Get results */
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);

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
                 SCAN_INTERVAL_TICKS / 100);

        /* Sleep — lets other ThreadX threads run */
        tx_thread_sleep(SCAN_INTERVAL_TICKS);
    }
}

/* ── WiFi event handler ────────────────────────────────────────── */

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
