/*
 * Wi-Fi CSI Motion Detector - application entry point.
 *
 * Pipeline as of Step 2:
 *
 *     [AP] --ACK--> [CSI engine] --> csi_rx_cb --queue--> csi_worker
 *                                                             |
 *                                                             v
 *                                                     on_csi_sample()
 *                                                             |
 *                                                        motion_push()
 *                                                     (normalise, window,
 *                                                      features, detect)
 *                                                             |
 *                                                      +------+------+
 *                                                      |             |
 *                                                     LED       serial log
 *
 *     [traffic_gen] --UDP 50/s--> [AP]        (creates the ACKs above)
 *
 * Step 3 adds a web dashboard fed from motion_get_state().
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "wifi_conn.h"
#include "csi_capture.h"
#include "traffic_gen.h"
#include "motion.h"
#include "web_server.h"
#include "led.h"

static const char *TAG = "app";

/* ---------------------------------------------------------------------------
 * CSI sink - runs in the csi_worker task.
 * ------------------------------------------------------------------------ */
static void on_csi_sample(const csi_sample_t *s, void *ctx)
{
    (void)ctx;

    /* Detection first: it is cheap, and it must not be starved by printing. */
    const bool new_decision = motion_push(s);

#if CSI_SERIAL_CSV
    /* Raw CSI, one line per sample. Step 4's Python recorder consumes this.
     *   CSI,<seq>,<ts_us>,<rssi>,<noise>,<n_sc>,a0,...,a51
     * Off by default because at 50 Hz it is ~15 kB/s and floods the console. */
    printf("CSI,%" PRIu32 ",%lld,%d,%d,%d",
           s->seq, (long long)s->ts_us, s->rssi, s->noise_floor, s->n_sc);
    for (int i = 0; i < s->n_sc; i++) {
        printf(",%.1f", s->amp[i]);
    }
    printf("\n");

    /* Features and label, once per decision window. Much lower bandwidth, and
     * this is exactly what the model in Step 5 sees.
     *   FEAT,<ts_us>,<motion>,<score>,f0,...,f7 */
    if (new_decision) {
        motion_state_t st;
        motion_get_state(&st);
        float fv[MOTION_N_FEATURES];
        motion_features_to_array(&st.f, fv);

        printf("FEAT,%lld,%d,%.3f", (long long)s->ts_us, st.motion ? 1 : 0, st.score);
        for (int i = 0; i < MOTION_N_FEATURES; i++) {
            printf(",%.6f", fv[i]);
        }
        printf("\n");
    }
#else
    (void)new_decision;
#endif
}

/* ---------------------------------------------------------------------------
 * Health reporter - one line every 5 s.
 * ------------------------------------------------------------------------ */
static void health_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        csi_stats_t st;
        csi_capture_get_stats(&st);

        motion_state_t m;
        motion_get_state(&m);

        ESP_LOGI(TAG,
                 "csi rate=%.1f Hz | accepted=%" PRIu32 " dropped=%" PRIu32
                 " | %s score=%.1f events=%" PRIu32 " | heap=%" PRIu32,
                 st.rate_hz, st.accepted, st.dropped,
                 m.warmed_up ? (m.motion ? "MOTION " : "clear  ") : "warmup ",
                 m.score, m.events, esp_get_free_heap_size());

        if (st.rate_hz < 1.0f) {
            ESP_LOGW(TAG, "CSI rate is near zero - check CONFIG_ESP_WIFI_CSI_ENABLED "
                          "and that Wi-Fi power save is off");
        }
    }
}

/* ------------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "Wi-Fi CSI Motion Detector starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    led_init();
    led_set(false);
    motion_init();

    ESP_ERROR_CHECK(wifi_conn_start());

    ESP_LOGI(TAG, "waiting for association...");
    ret = wifi_conn_wait_for_ip(30000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "could not join \"%s\" (%s). Check credentials with "
                      "`idf.py menuconfig`. Restarting in 10 s.",
                 APP_WIFI_SSID, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    ESP_ERROR_CHECK(csi_capture_start(on_csi_sample, NULL));
    ESP_ERROR_CHECK(traffic_gen_start());
    ESP_ERROR_CHECK(web_server_start());

    xTaskCreate(health_task, "health", 3072, NULL, 3, NULL);

    esp_ip4_addr_t ip = wifi_conn_ip();
    ESP_LOGI(TAG, "=====================================");
    ESP_LOGI(TAG, "  DASHBOARD:  http://" IPSTR "/", IP2STR(&ip));
    ESP_LOGI(TAG, "=====================================");
    ESP_LOGI(TAG, "keep the room still while it learns the baseline");
}
