# Step 3 — Web dashboard

## What it does

The ESP32 now runs a small website. Open its address in your browser and you can
watch the Wi-Fi signal live.

You see three things:

1. **A big word at the top** — `MOTION` in red, or `clear` in green.
2. **A bar chart** — one bar per Wi-Fi subcarrier. Small bars mean the room is
   still. Tall bars mean something is moving.
3. **A line chart** — the motion score over the last 30 seconds. Above the red
   dashed line means motion was detected.

There is also a **Relearn the room** button. Press it after you move the device,
then stay still for 10 seconds.

## How to use it

1. Flash and run:

   ```
   idf.py build flash monitor
   ```

2. Look for this in the terminal:

   ```
   I (5432) app: =====================================
   I (5432) app:   DASHBOARD:  http://192.168.1.47/
   I (5432) app: =====================================
   ```

3. Open that address in your browser. Your phone works too, as long as it is on
   the same Wi-Fi.

4. Stand still for 10 seconds while it says *learning the room*.

5. Then walk around and watch the bars jump.

## Notes

- The page is stored inside the firmware itself. There is no separate file to
  upload to the board.
- It asks the ESP32 for new data 10 times a second.
- The web server runs at a lower priority than the sensing code, so opening the
  page cannot slow down motion detection.

## One design decision worth knowing

The bar chart does **not** show the raw signal strength of each subcarrier. It
shows how far each one has moved *away from the empty room*.

The reason: a real Wi-Fi channel is uneven — some subcarriers are naturally
strong and some are naturally weak, and that pattern is fixed. If you draw the
raw values, that fixed pattern fills the whole chart and the actual disturbance
(usually a few percent) is invisible. Subtracting the empty-room baseline puts
every subcarrier on the same zero line, so what you see is only the change.

I caught this by rendering the page in a real browser with simulated data before
shipping it — the first version looked correct but the motion was almost
impossible to see.

## Verified

- Firmware builds clean against ESP-IDF v5.5.3 (0 errors, 0 warnings).
- Binary is 826 kB, 21% of the app partition still free.
- The page was loaded in a real headless Chromium against a simulated device,
  in both the `clear` and `MOTION` states. No JavaScript errors.

## Next

Step 4: a Python script on your PC that records training data — you walk around
for a while, then sit still for a while, and it saves both to a file. That file
is what the machine learning model in Step 5 learns from.
