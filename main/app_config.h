/*
 * app_config.h - Central tunables for the Wi-Fi CSI Motion Detector.
 *
 * Anything you are likely to change while experimenting lives here or in
 * `idf.py menuconfig` -> "Wi-Fi Motion Detector".
 */
#pragma once

#include "sdkconfig.h"

/* ---------------------------------------------------------------------------
 * Wi-Fi connection
 * ------------------------------------------------------------------------ */
#define APP_WIFI_SSID            CONFIG_WMD_WIFI_SSID
#define APP_WIFI_PASSWORD        CONFIG_WMD_WIFI_PASSWORD
#define APP_WIFI_MAX_RETRY       CONFIG_WMD_WIFI_MAX_RETRY

/* ---------------------------------------------------------------------------
 * CSI capture
 * ------------------------------------------------------------------------ */

/* Raw CSI buffer for a legacy (LLTF) 20 MHz frame: 64 sub-carriers x 2 bytes.
 * HT (HT-LTF) frames can return up to 384 bytes; we cap and truncate because
 * the LLTF block alone is enough for motion sensing and keeps RAM bounded. */
#define CSI_RAW_MAX_LEN          384

/* Number of usable LLTF sub-carriers we keep (+/-1..26, DC and guard bands
 * are null and carry no information). */
#define CSI_SC_COUNT             52

/* Depth of the queue between the Wi-Fi driver callback and our worker task.
 * The callback must never block, so overflow simply drops the newest sample
 * and increments a counter we report. */
#define CSI_QUEUE_DEPTH          32

/* ---------------------------------------------------------------------------
 * Traffic generator
 *
 * A single ESP32 only receives CSI when a frame actually arrives from the AP.
 * An idle station sees only beacons (~10 Hz), which is far too slow. So we
 * transmit small UDP packets to the gateway; the AP replies with an 802.11
 * ACK for every one of them, and with `dump_ack_en` the driver hands us CSI
 * for those ACKs. That turns the AP into a metronome at whatever rate we pick.
 * ------------------------------------------------------------------------ */
#define TRAFFIC_RATE_HZ          CONFIG_WMD_TRAFFIC_RATE_HZ
#define TRAFFIC_UDP_PORT         9999
#define TRAFFIC_PAYLOAD_LEN      16

/* ---------------------------------------------------------------------------
 * Outputs
 * ------------------------------------------------------------------------ */
#define LED_GPIO                 CONFIG_WMD_LED_GPIO

/* Emit one CSV line per CSI sample on the serial console. Needed for the
 * Python data-collection tooling in Step 4. */
#define CSI_SERIAL_CSV           CONFIG_WMD_SERIAL_CSV
