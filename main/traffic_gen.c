#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "traffic_gen.h"
#include "wifi_conn.h"
#include "app_config.h"

static const char *TAG = "traffic";

static TaskHandle_t   s_task;
static volatile bool  s_running;
static volatile uint32_t s_sent;

static void traffic_task(void *arg)
{
    (void)arg;

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    esp_ip4_addr_t gw = wifi_conn_gateway();

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(TRAFFIC_UDP_PORT),
    };
    dest.sin_addr.s_addr = gw.addr;

    ESP_LOGI(TAG, "generating %d pkt/s towards " IPSTR ":%d",
             TRAFFIC_RATE_HZ, IP2STR(&gw), TRAFFIC_UDP_PORT);

    uint8_t payload[TRAFFIC_PAYLOAD_LEN];
    memset(payload, 0x5A, sizeof(payload));

    /* Fixed-period loop using absolute deadlines. vTaskDelayUntil keeps the
     * spacing even if a send occasionally takes longer, which matters: our
     * frequency-domain features assume a roughly uniform sample interval. */
    const TickType_t period = pdMS_TO_TICKS(1000 / TRAFFIC_RATE_HZ);
    TickType_t last_wake = xTaskGetTickCount();

    while (s_running) {
        if (wifi_conn_is_connected()) {
            /* Stamp the packet so a sniffer can correlate if you ever need to. */
            const uint32_t seq = s_sent;
            memcpy(payload, &seq, sizeof(seq));

            const int n = sendto(sock, payload, sizeof(payload), 0,
                                 (struct sockaddr *)&dest, sizeof(dest));
            if (n > 0) {
                s_sent++;
            } else if (errno != ENOMEM && errno != EAGAIN) {
                /* ENOMEM/EAGAIN just mean the TX queue is briefly full; that is
                 * normal at high rates and self-corrects. Anything else is worth
                 * knowing about, but do not spam the log. */
                static int64_t last_log;
                const int64_t now = esp_timer_get_time();
                if (now - last_log > 5000000) {
                    ESP_LOGW(TAG, "sendto failed: errno %d", errno);
                    last_log = now;
                }
            }
        }
        xTaskDelayUntil(&last_wake, period > 0 ? period : 1);
    }

    close(sock);
    vTaskDelete(NULL);
}

esp_err_t traffic_gen_start(void)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sent    = 0;
    s_running = true;

    /* Priority 5: below the CSI worker (6) so decoding never starves, but well
     * above idle so the transmit cadence stays regular. */
    if (xTaskCreate(traffic_task, "traffic_gen", 4096, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void traffic_gen_stop(void)
{
    s_running = false;
}

uint32_t traffic_gen_sent(void)
{
    return s_sent;
}
