# Step 1 — Working CSI capture

## What changed and why

Your original `fast_scan.c` was the ESP-IDF scan example with a motion task bolted on.
Three things stopped it being a motion detector:

**1. It measured the wrong thing.** `esp_wifi_sta_get_ap_info()` returns RSSI — a single
number describing total received power, updated slowly and quantised to whole dBm. A
person walking across a room changes it by maybe 1–2 dB, well inside its own noise. The
threshold `strength < -50` doesn't detect motion; it detects *distance from the router*.
Stand still 10 m away and the LED stays on forever.

**2. The detector task starved the CPU.** The `vTaskDelay` at the bottom of the loop was
commented out, so `MotionDetector` spun at priority 5 with no yield. On a single-core
ESP32 build that either trips the task watchdog or blocks lower-priority work. The only
delay was inside the `if` branch — meaning the loop yielded *only* when it thought it saw
motion.

**3. Reconnects hammered the AP.** `WIFI_EVENT_STA_DISCONNECTED` called `esp_wifi_connect()`
immediately and unconditionally. A wrong password produces an infinite tight retry loop.

## What replaces it

The firmware now reads **CSI (Channel State Information)** — the per-subcarrier complex
channel response the ESP32's PHY computes for every frame it receives. A 20 MHz Wi-Fi
channel is 52 usable OFDM subcarriers, each a separate narrowband probe of the room.
A body moving through the space changes the multipath geometry, and each subcarrier
responds differently. That gives you a 52-dimensional signal at 50 Hz instead of one
noisy scalar at 2 Hz.

### The sampling problem, and the trick that solves it

CSI only exists when a frame *arrives*. An idle station hears beacons at ~10 Hz — far
too slow and far too irregular.

The solution in `traffic_gen.c`: transmit small UDP packets to the gateway at a fixed
50 Hz. Every frame the ESP32 sends is acknowledged at the 802.11 layer by the AP, and
with `dump_ack_en = true` in the CSI config the driver reports CSI for those ACK frames.
**You get one CSI record per packet you send**, at any rate you choose, from a single
ESP32, with no cooperation from the router.

The packets go to a port nothing listens on. That's intentional — we only care about the
link-layer ACK, not a reply.

### New module layout

| File | Role |
|---|---|
| `app_main.c` | Boot sequence, wires the pipeline together, health reporting |
| `wifi_conn.c` | Station connect, all-channel scan, backoff retry, gateway capture |
| `csi_capture.c` | CSI config, driver callback, queue, subcarrier decode |
| `traffic_gen.c` | Fixed-rate UDP transmitter that generates the ACKs |
| `led.c` | Non-blocking indicator (fixes bug 2 structurally) |
| `app_config.h` | Every tunable in one place |

`fast_scan.c` is no longer in the build — `main/CMakeLists.txt` lists sources explicitly.
It's left on disk so you can diff against it; delete it whenever you like.

### Critical configuration

Two settings are not optional and are pre-set in `sdkconfig.defaults`:

- `CONFIG_ESP_WIFI_CSI_ENABLED=y` — without it the whole CSI API is compiled out.
- `CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600` — 52 amplitudes at 50 Hz is ~15 kB/s of CSV;
  115200 baud silently drops samples.

Power save is forced off (`WIFI_PS_NONE`) in `wifi_conn.c`. With power save on, the radio
sleeps between beacons — the sample rate collapses and the timing jitter destroys any
frequency-domain feature you'd want to compute later.

## Build and flash

Your existing `sdkconfig` predates all of this, so start clean:

```
idf.py fullclean
idf.py set-target esp32
idf.py menuconfig      # -> "Wi-Fi Motion Detector" -> set SSID and password
idf.py build flash monitor
```

The `sdkconfig` shipped alongside this is already correct for ESP32 + CSI, so you can
skip `set-target` if you keep it — you still need `menuconfig` for your credentials.

Monitor at the new baud rate:

```
idf.py -b 921600 monitor
```

## What you should see

Every 5 seconds, a health line:

```
I (12345) app: csi rate=49.8 Hz | accepted=2490 dropped=0 seen=2510 | tx=2500 | heap=180000
```

and a continuous CSV stream:

```
CSI,1,12345678,-47,-92,52,18.0,19.4,17.2,20.1, ... (52 values)
```

### Reading the health line

| Field | Healthy | If it's wrong |
|---|---|---|
| `rate` | within ~10% of 50 Hz | 0 → CSI not enabled, or power save on. Low → AP is rate-limiting or far away |
| `dropped` | 0 | Consumer too slow — lower `WMD_TRAFFIC_RATE_HZ` or disable CSV |
| `accepted` vs `tx` | roughly equal | Much lower → many ACKs aren't producing CSI; try moving closer to the AP |
| `seen` vs `accepted` | `seen` slightly higher | Much higher → lots of neighbour traffic being correctly filtered out |

**Sanity check before moving on:** with the room empty, the amplitude values should be
fairly steady. Wave your hand between the ESP32 and the router and several columns
should visibly jump. If nothing changes when you move, CSI isn't really flowing — fix
that before Step 2, because everything downstream depends on it.

## Wiring

LED anode → GPIO 19 → 220 Ω resistor → LED → GND. Change the pin under
`menuconfig → Wi-Fi Motion Detector → Outputs`.

## Verification performed

- Firmware compiles clean against ESP-IDF v5.5.3 for ESP32 (0 errors, 0 warnings).
  Binary: 761 kB, 27% of the app partition free.
- `csi_decode()` unit-tested on host: subcarrier map covers exactly the 52 usable
  carriers with no duplicates; amplitude math verified; the `first_word_invalid`
  hardware quirk is confirmed not to leak garbage into subcarrier +1.

## Next

Step 2 turns this stream into a motion decision: sliding-window features over the
subcarrier amplitudes, an adaptive baseline, and a statistical detector that works
before any ML is involved.
