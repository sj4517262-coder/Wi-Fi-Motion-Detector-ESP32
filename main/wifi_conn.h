/*
 * wifi_conn.h - Station mode connection with all-channel scan and retry.
 *
 * This is the original fast_scan.c logic, cleaned up:
 *   - exponential-ish retry instead of an unconditional reconnect storm
 *   - a blocking wait so the rest of the app can be sequenced properly
 *   - the gateway address is captured for the traffic generator
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

/* Bring up Wi-Fi in station mode and start connecting. Returns as soon as the
 * driver is started; use wifi_conn_wait_for_ip() to block until associated. */
esp_err_t wifi_conn_start(void);

/* Block until we have an IP, or until timeout. */
esp_err_t wifi_conn_wait_for_ip(uint32_t timeout_ms);

/* True while the station holds a valid IP lease. */
bool wifi_conn_is_connected(void);

/* Gateway address of the current lease - the traffic generator pings this. */
esp_ip4_addr_t wifi_conn_gateway(void);

/* Our own address, for printing the dashboard URL. */
esp_ip4_addr_t wifi_conn_ip(void);
