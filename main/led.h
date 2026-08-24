/*
 * led.h - Motion indicator output.
 *
 * Deliberately trivial, but isolated so Step 2's detector does not have to
 * know anything about GPIO, and so a buzzer or relay can be swapped in later.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void led_init(void);

/* Set the steady state of the indicator. */
void led_set(bool on);

/* Latch the indicator on for `ms` milliseconds. Repeated calls extend the
 * hold, so a burst of detections produces one continuous light rather than
 * a stutter. Non-blocking. */
void led_pulse(uint32_t ms);
