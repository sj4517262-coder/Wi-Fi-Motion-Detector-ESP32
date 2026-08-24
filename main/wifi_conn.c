#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "wifi_conn.h"
#include "app_config.h"

static const char *TAG = "wifi";

#define BIT_CONNECTED  BIT0
#define BIT_FAILED     BIT1

static EventGroupHandle_t s_events;
static esp_netif_t       *s_netif;
static int                s_retries;
static volatile bool      s_connected;
static esp_ip4_addr_t     s_gateway;
static esp_ip4_addr_t     s_ip;

/* ---------------------------------------------------------------------------
 * Scan / sort configuration, mirroring the original Kconfig choices.
 * ------------------------------------------------------------------------ */
#if CONFIG_WMD_WIFI_ALL_CHANNEL_SCAN
#define APP_SCAN_METHOD WIFI_ALL_CHANNEL_SCAN
#else
#define APP_SCAN_METHOD WIFI_FAST_SCAN
#endif

#if CONFIG_WMD_WIFI_CONNECT_AP_BY_SECURITY
#define APP_SORT_METHOD WIFI_CONNECT_AP_BY_SECURITY
#else
#define APP_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#endif

/* ------------------------------------------------------------------------ */

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        const wifi_event_sta_disconnected_t *ev = data;

        if (s_retries < APP_WIFI_MAX_RETRY) {
            s_retries++;
            /* Back off a little. Reconnecting instantly in a tight loop - as
             * the original code did - hammers the AP and can get the station
             * temporarily blacklisted. */
            vTaskDelay(pdMS_TO_TICKS(500 * s_retries));
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d/%d",
                     ev->reason, s_retries, APP_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "giving up after %d retries (last reason %d)",
                     s_retries, ev->reason);
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        s_ip        = ev->ip_info.ip;
        s_gateway   = ev->ip_info.gw;
        s_retries   = 0;
        s_connected = true;
        ESP_LOGI(TAG, "connected, ip=" IPSTR " gw=" IPSTR,
                 IP2STR(&s_ip), IP2STR(&s_gateway));
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

/* ------------------------------------------------------------------------ */

esp_err_t wifi_conn_start(void)
{
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_netif = esp_netif_create_default_wifi_sta();
    assert(s_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t sta = {
        .sta = {
            .scan_method        = APP_SCAN_METHOD,
            .sort_method        = APP_SORT_METHOD,
            .threshold.rssi     = -127,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)sta.sta.ssid,     APP_WIFI_SSID,     sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, APP_WIFI_PASSWORD, sizeof(sta.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    /* Power save must be off. With PS enabled the radio sleeps between beacons,
     * which both drops the CSI rate and adds huge timing jitter - fatal for a
     * sensing application that relies on evenly spaced samples. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to \"%s\"", APP_WIFI_SSID);
    return ESP_OK;
}

esp_err_t wifi_conn_wait_for_ip(uint32_t timeout_ms)
{
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
        timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));

    if (bits & BIT_CONNECTED) {
        return ESP_OK;
    }
    if (bits & BIT_FAILED) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    return ESP_ERR_TIMEOUT;
}

bool wifi_conn_is_connected(void)   { return s_connected; }
esp_ip4_addr_t wifi_conn_gateway(void) { return s_gateway; }
esp_ip4_addr_t wifi_conn_ip(void)      { return s_ip; }
