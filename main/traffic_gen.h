/*
 * traffic_gen.h - Keeps CSI flowing.
 *
 * A station that is not transmitting receives almost nothing: beacons at
 * ~10 Hz and not much else. Wi-Fi sensing needs a steady, evenly spaced
 * stream of frames, so we create one ourselves.
 *
 * The trick: send small UDP datagrams to the gateway at a fixed rate. Every
 * transmitted frame is acknowledged at the 802.11 layer by the AP, and with
 * `dump_ack_en` set in the CSI config the driver reports CSI for those ACKs.
 * We therefore get one CSI record per packet we send, at whatever rate we
 * choose, without needing a second ESP32 or any cooperation from the router.
 *
 * The datagrams go to a port nothing is listening on. That is fine and
 * intentional: we only care about the link-layer ACK, not any reply. The
 * gateway will silently drop them (or send an ICMP port-unreachable, which is
 * also a frame we can measure).
 */
#pragma once

#include "esp_err.h"

esp_err_t traffic_gen_start(void);
void      traffic_gen_stop(void);

/* Packets sent since start - compare against csi_stats.accepted to see how
 * many of our transmissions actually produced a usable CSI record. */
uint32_t  traffic_gen_sent(void);
