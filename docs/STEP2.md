# Step 2 — It actually detects motion now

## The idea

Step 1 gave you 52 numbers, 50 times a second. Step 2 turns that firehose into
one yes/no answer.

Two design decisions carry the whole thing.

### 1. Normalise every sample

The ESP32's automatic gain control constantly rescales the raw CSI magnitudes as the
link fluctuates. If you feed raw amplitudes to a detector, **you measure the AGC, not
the room** — and you get false alarms every time the radio adjusts itself.

The fix is one line: divide each sample's 52 amplitudes by their own mean. That removes
the gain completely and leaves only the *shape* of the channel response across
subcarriers — which is the part a moving body actually changes.

I verified this matters: in simulation, a sudden **2.5× gain jump** produces **zero**
false detections. Without normalisation it would look like a violent disturbance.

### 2. Learn the room instead of hardcoding a threshold

Every room has a different amount of background fluctuation. A fixed threshold that
works in your bedroom will either scream constantly or stay silent in a different space.

So the firmware spends its first 10 seconds measuring what "quiet" looks like *here*,
then reports how far the current reading departs from it, measured in standard
deviations (a **z-score**). Trigger at 4σ, release at 2σ.

The same firmware works anywhere with no retuning. It also means **the room must be
empty and still during the first 10 seconds after boot** — otherwise it learns your
movement as normal.

### The subtle part: the guard band

There's a trap in adaptive baselines. The baseline stops updating while motion is
asserted (obvious). But what about motion that's *just below* the trigger threshold?

It gets quietly absorbed. The "quiet" level creeps up to match it, the score decays back
to zero, and the detector goes blind to exactly the subtle activity you most want to catch.

I hit this in testing and fixed it with a **guard band**: the baseline stops learning
above 1.5σ, well below the 4σ trigger. Anything already looking mildly unusual isn't
allowed to teach the baseline that it's normal.

The measured effect on sensitivity:

| Disturbance (fraction of a walking body) | z-score before fix | after fix |
|---|---|---|
| 0.3× | 0.2 | **1.1** |
| 0.9× | 3.9 | **6.4** |
| 1.2× | 6.9 | **11.2** |

## The eight features

Beyond the yes/no, every 100 ms the firmware computes a feature vector. **These eight
numbers are what the machine learning model in Step 5 will be trained on** — they're
being computed now so that on-device behaviour and offline training see identical inputs.

| # | Feature | What it captures |
|---|---|---|
| 0 | `mean_var` | Average variance across subcarriers — overall disturbance |
| 1 | `max_var` | The single most disturbed subcarrier — localised changes |
| 2 | `mean_absdiff` | Mean sample-to-sample change — **the primary detection statistic** |
| 3 | `baseline_dist` | Cosine distance from the learned quiet profile — catches someone who moved in and *stopped* |
| 4 | `amp_mean` | Mean raw amplitude — link strength context |
| 5 | `amp_var` | Bulk power fluctuation |
| 6 | `rssi_std` | RSSI spread — the old, weak signal, kept for comparison |
| 7 | `zcr` | Zero-crossing rate — crude motion-speed proxy |

Why is `mean_absdiff` the primary and not variance? Variance over a 1-second window
counts slow thermal drift as signal. A first difference is naturally high-pass and
therefore drift-immune, which matters enormously over a run of hours.

## Verification performed

Compiles clean against ESP-IDF v5.5.3 (0 errors, 0 warnings). I also ran the **real
`motion.c`** on the host against synthetic CSI containing quiet periods, an AGC gain
jump, and walk-throughs:

```
FALSE POSITIVES:        0 of 292 quiet windows (0.00%)
AGC GAIN JUMP x2.5:     0 of 100 windows flagged
MOTION EVENTS:          4 of 4 walk-throughs detected
RECALL during motion:   185 of 200 windows (92.5%)
```

**Known limit, stated honestly:** disturbances below roughly 40% of a walking body are
*not* detected by this statistical method. That's the gap Step 5's ML is meant to close —
it can use all eight features together, where this detector uses one.

## What to do

Save the four files over the existing ones, then:

```
idf.py build flash monitor
```

**Keep the room empty and still for the first 10 seconds after it boots.**

### What you should see

During warm-up:

```
I (1234) motion: window=50 samples (1.0 s), hop=5 samples (10 decisions/s)
I (1240) motion: learning the room - detection starts in ~10 s
```

Then:

```
I (11500) motion: baseline learned: quiet=0.01477 spread=0.00023 - detection active
```

Now walk around:

```
W (15200) motion: >>> MOTION  score=8.4  (level=0.04120 quiet=0.01477 spread=0.00023)
I (19100) motion:     clear   after 3.9 s of motion
```

And the health line every 5 seconds now carries the detector state:

```
I (20000) app: csi rate=49.8 Hz | accepted=990 dropped=0 | clear   score=0.4 events=3 | heap=175000
```

The LED on GPIO 19 lights whenever `MOTION` is asserted.

## Tuning, if you need it

All in `main/motion.h`:

- **Too many false alarms** → raise `MOTION_Z_ON` from 4.0 to 6.0
- **Not sensitive enough** → lower `MOTION_Z_ON` to 3.0
- **LED flickers on/off** → raise `MOTION_CONSEC_OFF` from 10 (each unit is 100 ms)
- **Moved the device to a new room** → just reboot it; it relearns in 10 s

## Next

Step 3 puts a live dashboard on the ESP32 itself — open its IP in your browser and watch
all 52 subcarriers move in real time. That's where you'll be able to *see* whether the
sensing genuinely works in your room, rather than trusting a log line.
