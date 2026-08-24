#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "csi_capture.h"

static const char *TAG = "csi";

/* ---------------------------------------------------------------------------
 * Sub-carrier map
 *
 * The ESP32 returns 64 complex values for a 20 MHz legacy (LLTF) frame, stored
 * as interleaved int8 pairs in the order [imag, real].
 *
 * Array index -> OFDM sub-carrier index:
 *     i in  0..31  ->   +i        (0 is DC)
 *     i in 32..63  ->   i - 64    (so 32 -> -32, 63 -> -1)
 *
 * In 802.11a/g only sub-carriers +/-1..26 carry energy. DC (index 0), the
 * guard bands (+/-27..31) and index 32 (-32) are null and would only add noise,
 * so we keep the 52 usable ones.
 * ------------------------------------------------------------------------ */
static const uint8_t kSubcarrierIdx[CSI_SC_COUNT] = {
    /* -26 .. -1  ->  raw indices 38..63 */
    38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
    51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    /*  +1 .. +26 ->  raw indices  1..26 */
     1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
};

/* ------------------------------------------------------------------------ */

static QueueHandle_t   s_queue;
static TaskHandle_t    s_task;
static volatile bool   s_running;
static csi_sample_cb_t s_cb;
static void           *s_cb_ctx;
static uint8_t         s_ap_bssid[6];
static uint32_t        s_seq;

static csi_stats_t     s_stats;
static portMUX_TYPE    s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---------------------------------------------------------------------------
 * Driver callback.
 *
 * This runs in the Wi-Fi task (not an ISR, but the same rules apply in
 * spirit). It must be short and must never block, or the Wi-Fi stack starts
 * dropping frames. So: filter, memcpy, enqueue, return. All arithmetic
 * happens later in the worker task.
 * ------------------------------------------------------------------------ */
static void csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;

    if (!s_running || info == NULL || info->buf == NULL || info->len == 0) {
        return;
    }

    portENTER_CRITICAL(&s_stats_lock);
    s_stats.received++;
    portEXIT_CRITICAL(&s_stats_lock);

    /* Only keep frames from our own access point. Neighbouring networks share
     * the channel and their CSI describes a completely different radio path,
     * which would look like constant motion. */
    if (memcmp(info->mac, s_ap_bssid, 6) != 0) {
        return;
    }

    /* We need at least the full LLTF block. Short records (some ACK variants)
     * are not comparable across time, so drop them rather than pad. */
    if (info->len < 128) {
        return;
    }

    csi_raw_t raw;
    raw.seq                = ++s_seq;
    raw.ts_us              = esp_timer_get_time();
    raw.rssi               = info->rx_ctrl.rssi;
    raw.noise_floor        = info->rx_ctrl.noise_floor;
    raw.channel            = info->rx_ctrl.channel;
    raw.sig_mode           = info->rx_ctrl.sig_mode;
    raw.rate               = info->rx_ctrl.rate;
    raw.first_word_invalid = info->first_word_invalid;
    raw.len                = (info->len > CSI_RAW_MAX_LEN) ? CSI_RAW_MAX_LEN : info->len;
    memcpy(raw.buf, info->buf, raw.len);

    if (xQueueSend(s_queue, &raw, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_stats_lock);
        s_stats.dropped++;
        portEXIT_CRITICAL(&s_stats_lock);
    }
}

/* ------------------------------------------------------------------------ */

void csi_decode(const csi_raw_t *raw, csi_sample_t *out)
{
    out->seq         = raw->seq;
    out->ts_us       = raw->ts_us;
    out->rssi        = raw->rssi;
    out->noise_floor = raw->noise_floor;
    out->n_sc        = CSI_SC_COUNT;

    for (int k = 0; k < CSI_SC_COUNT; k++) {
        const int i    = kSubcarrierIdx[k];
        const int imag = raw->buf[i * 2];
        const int real = raw->buf[i * 2 + 1];

        /* Hardware quirk: when first_word_invalid is set the first four bytes
         * (sub-carriers 0 and 1) are garbage. Sub-carrier 0 is DC and already
         * excluded; +1 is index 26 in our map, so patch it from its neighbour
         * rather than letting a garbage value poison the feature vector. */
        if (raw->first_word_invalid && i <= 1) {
            out->amp[k] = (k > 0) ? out->amp[k - 1] : 0.0f;
            continue;
        }

        out->amp[k] = sqrtf((float)(real * real + imag * imag));
    }
}

/* ------------------------------------------------------------------------ */

static void csi_worker(void *arg)
{
    (void)arg;
    csi_raw_t    raw;
    csi_sample_t sample;

    int64_t  window_start = esp_timer_get_time();
    uint32_t window_count = 0;

    while (s_running) {
        if (xQueueReceive(s_queue, &raw, pdMS_TO_TICKS(200)) != pdTRUE) {
            /* No CSI for 200 ms. Fall through so the rate estimate still
             * decays towards zero and the dashboard shows the stall. */
        } else {
            csi_decode(&raw, &sample);

            portENTER_CRITICAL(&s_stats_lock);
            s_stats.accepted++;
            portEXIT_CRITICAL(&s_stats_lock);
            window_count++;

            if (s_cb) {
                s_cb(&sample, s_cb_ctx);
            }
        }

        /* Update the smoothed sample rate once a second. */
        const int64_t now = esp_timer_get_time();
        if (now - window_start >= 1000000) {
            const float instant = window_count * 1000000.0f / (float)(now - window_start);
            portENTER_CRITICAL(&s_stats_lock);
            s_stats.rate_hz = s_stats.rate_hz * 0.5f + instant * 0.5f;
            portEXIT_CRITICAL(&s_stats_lock);
            window_start = now;
            window_count = 0;
        }
    }

    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------ */

esp_err_t csi_capture_start(csi_sample_cb_t on_sample, void *ctx)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Remember which AP we are bonded to, so the callback can filter. */
    wifi_ap_record_t ap;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "not associated, cannot start CSI: %s", esp_err_to_name(err));
        return err;
    }
    memcpy(s_ap_bssid, ap.bssid, 6);
    ESP_LOGI(TAG, "locking CSI to BSSID %02x:%02x:%02x:%02x:%02x:%02x on channel %d",
             s_ap_bssid[0], s_ap_bssid[1], s_ap_bssid[2],
             s_ap_bssid[3], s_ap_bssid[4], s_ap_bssid[5], ap.primary);

    s_queue = xQueueCreate(CSI_QUEUE_DEPTH, sizeof(csi_raw_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "out of memory allocating CSI queue");
        return ESP_ERR_NO_MEM;
    }

    memset(&s_stats, 0, sizeof(s_stats));
    s_cb     = on_sample;
    s_cb_ctx = ctx;
    s_seq    = 0;

    const wifi_csi_config_t csi_cfg = {
        .lltf_en           = true,
        .htltf_en          = true,
        .stbc_htltf2_en    = true,
        /* Use HT-LTF directly instead of averaging it with LLTF - averaging
         * smears the very fluctuations we are trying to measure. */
        .ltf_merge_en      = false,
        /* The channel filter smooths adjacent sub-carriers, which destroys the
         * independent per-sub-carrier detail that makes CSI useful. Off. */
        .channel_filter_en = false,
        /* Automatic scaling. Manual scaling gives more consistent magnitudes
         * but needs per-environment tuning; we normalise in software instead. */
        .manu_scale        = false,
        .shift             = 0,
        /* The important one: capture CSI from 802.11 ACK frames. This is what
         * lets a single ESP32 sample at 50-100 Hz using its own traffic. */
        .dump_ack_en       = true,
    };

    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(&csi_rx_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));

    s_running = true;
    if (xTaskCreate(csi_worker, "csi_worker", 4096, NULL, 6, &s_task) != pdPASS) {
        s_running = false;
        esp_wifi_set_csi(false);
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "CSI capture running (%d usable sub-carriers)", CSI_SC_COUNT);
    return ESP_OK;
}

void csi_capture_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    esp_wifi_set_csi(false);
    vTaskDelay(pdMS_TO_TICKS(300));   /* let the worker exit its loop */
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
}

void csi_capture_get_stats(csi_stats_t *out)
{
    portENTER_CRITICAL(&s_stats_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
}
