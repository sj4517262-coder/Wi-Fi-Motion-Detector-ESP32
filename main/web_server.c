#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "web_server.h"
#include "csi_capture.h"
#include "motion.h"
#include "traffic_gen.h"

static const char *TAG = "web";

static httpd_handle_t s_server;

/* The dashboard page is embedded into the firmware image at build time - see
 * EMBED_FILES in main/CMakeLists.txt. No filesystem partition needed. */
extern const uint8_t dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_html_end[]   asm("_binary_dashboard_html_end");

/* ------------------------------------------------------------------------ */

static esp_err_t index_handler(httpd_req_t *req)
{
    const size_t len = dashboard_html_end - dashboard_html_start;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)dashboard_html_start, len);
}

/* ------------------------------------------------------------------------ */

static esp_err_t data_handler(httpd_req_t *req)
{
    /* Built on the heap, not the stack: httpd task stacks are small and this
     * buffer is ~2 kB. */
    const size_t cap = 2600;
    char *buf = malloc(cap);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    csi_stats_t cs;
    csi_capture_get_stats(&cs);

    motion_state_t ms;
    motion_get_state(&ms);

    float amp[CSI_SC_COUNT], base[CSI_SC_COUNT];
    motion_get_display(amp, base);

    float fv[MOTION_N_FEATURES];
    motion_features_to_array(&ms.f, fv);

    size_t n = 0;
    n += snprintf(buf + n, cap - n,
                  "{\"rate\":%.1f,\"accepted\":%lu,\"dropped\":%lu,\"sent\":%lu,"
                  "\"motion\":%d,\"warm\":%d,\"score\":%.2f,\"events\":%lu,"
                  "\"quiet\":%.6f,\"spread\":%.6f,\"windows\":%lu,\"held\":%.1f",
                  cs.rate_hz,
                  (unsigned long)cs.accepted, (unsigned long)cs.dropped,
                  (unsigned long)traffic_gen_sent(),
                  ms.motion ? 1 : 0, ms.warmed_up ? 1 : 0, ms.score,
                  (unsigned long)ms.events,
                  ms.baseline_level, ms.baseline_spread,
                  (unsigned long)ms.windows,
                  (esp_timer_get_time() - ms.last_change_us) / 1e6f);

    n += snprintf(buf + n, cap - n, ",\"amp\":[");
    for (int i = 0; i < CSI_SC_COUNT && n < cap - 16; i++) {
        n += snprintf(buf + n, cap - n, "%s%.3f", i ? "," : "", amp[i]);
    }

    n += snprintf(buf + n, cap - n, "],\"base\":[");
    for (int i = 0; i < CSI_SC_COUNT && n < cap - 16; i++) {
        n += snprintf(buf + n, cap - n, "%s%.3f", i ? "," : "", base[i]);
    }

    n += snprintf(buf + n, cap - n, "],\"f\":[");
    for (int i = 0; i < MOTION_N_FEATURES && n < cap - 16; i++) {
        n += snprintf(buf + n, cap - n, "%s%.6f", i ? "," : "", fv[i]);
    }
    n += snprintf(buf + n, cap - n, "]}");

    httpd_resp_set_type(req, "application/json");
    /* The page is polled continuously; caching it would freeze the display. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, buf, n);

    free(buf);
    return err;
}

/* ------------------------------------------------------------------------ */

static esp_err_t reset_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "baseline reset requested from dashboard");
    motion_reset_baseline();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------------ */

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 8;
    /* The dashboard polls fast; a bigger backlog avoids refused connections
     * when the browser opens several sockets at once. */
    cfg.backlog_conn     = 5;
    cfg.stack_size       = 5120;
    /* Below the CSI worker (6) and traffic generator (5) - serving a web page
     * must never delay the sensing pipeline. */
    cfg.task_priority    = 4;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = index_handler },
        { .uri = "/api/data",   .method = HTTP_GET,  .handler = data_handler  },
        { .uri = "/api/reset",  .method = HTTP_POST, .handler = reset_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uris[i]));
    }

    ESP_LOGI(TAG, "dashboard running");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
