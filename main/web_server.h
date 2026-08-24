/*
 * web_server.h - Dashboard served by the ESP32 itself.
 *
 * Three endpoints:
 *   GET  /            the dashboard page (embedded in the firmware binary)
 *   GET  /api/data    current state as JSON, polled ~10x/second by the page
 *   POST /api/reset   forget the learned baseline and relearn
 *
 * Polling rather than WebSockets: it is far simpler, has no reconnection
 * logic to get wrong, and at ten small requests per second the ESP32 is not
 * remotely stressed.
 */
#pragma once

#include "esp_err.h"

/* Call after the station has an IP. */
esp_err_t web_server_start(void);

void web_server_stop(void);
