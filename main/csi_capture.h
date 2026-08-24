/*
 * csi_capture.h - Channel State Information acquisition.
 *
 * Responsibilities:
 *   1. Configure and enable the ESP32 CSI engine.
 *   2. Receive CSI records in the Wi-Fi driver callback (fast path, no work).
 *   3. Hand raw records to a worker task over a queue.
 *   4. Convert raw int8 I/Q pairs into per-sub-carrier amplitudes.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "app_config.h"

/* Raw CSI record as it comes out of the driver callback. Deliberately POD and
 * small so it can be memcpy'd into a FreeRTOS queue without allocation. */
typedef struct {
    uint32_t seq;          /* our own monotonic counter                     */
    int64_t  ts_us;        /* esp_timer_get_time() at callback entry        */
    int8_t   rssi;         /* dBm                                           */
    int8_t   noise_floor;  /* dBm                                           */
    uint8_t  channel;      /* primary channel                               */
    uint8_t  sig_mode;     /* 0 = non-HT (11bg), 1 = HT (11n)               */
    uint8_t  rate;         /* PHY rate code                                 */
    bool     first_word_invalid;
    uint16_t len;          /* valid bytes in buf                            */
    int8_t   buf[CSI_RAW_MAX_LEN];
} csi_raw_t;

/* A decoded sample: amplitudes for the usable sub-carriers only. */
typedef struct {
    uint32_t seq;
    int64_t  ts_us;
    int8_t   rssi;
    int8_t   noise_floor;
    uint8_t  n_sc;
    float    amp[CSI_SC_COUNT];
} csi_sample_t;

/* Runtime statistics, exposed for the dashboard and for sanity checking. */
typedef struct {
    uint32_t received;     /* CSI records handed to us by the driver        */
    uint32_t accepted;     /* records that passed filtering                 */
    uint32_t dropped;      /* records lost because the queue was full       */
    float    rate_hz;      /* smoothed accepted-samples-per-second          */
} csi_stats_t;

/*
 * Start CSI capture. Must be called after esp_wifi_start() and after the
 * station has associated (the BSSID filter needs the AP address).
 *
 * `on_sample` is invoked from the CSI worker task for every accepted sample.
 * It may block and do real work - it is not the driver callback.
 */
typedef void (*csi_sample_cb_t)(const csi_sample_t *sample, void *ctx);

esp_err_t csi_capture_start(csi_sample_cb_t on_sample, void *ctx);

/* Stop capture and free the worker task. Safe to call if not started. */
void csi_capture_stop(void);

/* Snapshot the current statistics. */
void csi_capture_get_stats(csi_stats_t *out);

/* Convert one raw record into amplitudes. Exposed for unit testing. */
void csi_decode(const csi_raw_t *raw, csi_sample_t *out);
