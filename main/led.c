#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "led.h"
#include "app_config.h"

static volatile int64_t s_hold_until_us;
static volatile bool    s_steady;

static void led_task(void *arg)
{
    (void)arg;
    while (1) {
        const bool on = s_steady || (esp_timer_get_time() < s_hold_until_us);
        gpio_set_level(LED_GPIO, on ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void led_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(LED_GPIO, 0);

    /* A tiny dedicated task means callers never block on the LED, and the
     * original bug - a detector loop that held the CPU for 2 s with
     * vTaskDelay just to keep an LED lit - cannot come back. */
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
}

void led_set(bool on)
{
    s_steady = on;
}

void led_pulse(uint32_t ms)
{
    const int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    if (until > s_hold_until_us) {
        s_hold_until_us = until;
    }
}
