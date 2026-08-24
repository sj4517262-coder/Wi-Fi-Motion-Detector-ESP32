#!/usr/bin/env python3
"""
record.py - Record training data from the Wi-Fi motion detector.

It talks to the ESP32 over Wi-Fi (the same address you open in your browser),
so you do not need the USB cable, a serial terminal, or any pip installs -
everything here is Python's standard library.

Typical use:

    python record.py --ip 192.168.1.47

That runs a guided session: it asks you to keep still for a few minutes, then
to walk around for a few minutes, and saves both to a CSV file. That file is
what Step 5 trains the model on.

To record one extra class on its own:

    python record.py --ip 192.168.1.47 --label fan_on --minutes 2
"""

import argparse
import csv
import json
import math
import os
import statistics
import sys
import time
import urllib.error
import urllib.request

POLL_HZ = 20.0          # poll faster than the device decides, then de-duplicate
HTTP_TIMEOUT = 2.0
N_FEATURES = 8
FEATURE_NAMES = ["mean_var", "max_var", "mean_absdiff", "baseline_dist",
                 "amp_mean", "amp_var", "rssi_std", "zcr"]


# --------------------------------------------------------------------------
# Device communication
# --------------------------------------------------------------------------

def fetch(ip, path="/api/data"):
    """One request to the device. Returns a dict, or None if it failed."""
    try:
        with urllib.request.urlopen(f"http://{ip}{path}", timeout=HTTP_TIMEOUT) as r:
            return json.loads(r.read().decode())
    except (urllib.error.URLError, OSError, json.JSONDecodeError, TimeoutError):
        return None


def wait_for_device(ip):
    print(f"Looking for the detector at http://{ip}/ ...")
    for attempt in range(15):
        d = fetch(ip)
        if d is not None:
            print(f"  found it. Sample rate {d['rate']:.0f} Hz.")
            if d["rate"] < 5:
                print("  WARNING: the sample rate is very low. Data may be poor.")
            return d
        time.sleep(1)
        if attempt == 2:
            print("  still trying... check the address and that you are on the "
                  "same Wi-Fi network.")
    print("\nCould not reach the detector.")
    print("Open http://%s/ in your browser first. If that does not work, the "
          "address is wrong or the board is not running." % ip)
    sys.exit(1)


def wait_until_warm(ip):
    """The device needs ~10 s of quiet to learn the room before its numbers
    mean anything. Recording before that would poison the training data."""
    d = fetch(ip)
    if d and d.get("warm"):
        return
    print("\nThe detector is still learning the room.")
    print("Please stay still...")
    while True:
        d = fetch(ip)
        if d and d.get("warm"):
            print("  ready.\n")
            return
        time.sleep(0.5)


# --------------------------------------------------------------------------
# Recording
# --------------------------------------------------------------------------

def countdown(seconds, message):
    print(f"\n{message}")
    for i in range(seconds, 0, -1):
        print(f"  starting in {i}... ", end="\r", flush=True)
        time.sleep(1)
    print("  RECORDING NOW          ")


def record_session(ip, label, seconds):
    """
    Collect rows for one label.

    The device produces a new decision every 100 ms and exposes a 'windows'
    counter. We poll faster than that and keep only rows where the counter
    moved, which gives exactly one row per decision - no duplicates, no gaps.
    """
    rows = []
    last_window = -1
    misses = 0
    period = 1.0 / POLL_HZ
    t_start = time.monotonic()
    next_poll = t_start

    try:
        while time.monotonic() - t_start < seconds:
            next_poll += period
            time.sleep(max(0.0, next_poll - time.monotonic()))

            d = fetch(ip)
            if d is None:
                misses += 1
                continue

            if d["windows"] == last_window:
                continue                      # same decision, already stored
            last_window = d["windows"]

            dev = [a - b for a, b in zip(d["amp"], d["base"])]
            rows.append({
                "t": round(time.monotonic() - t_start, 3),
                "label": label,
                "device_motion": d["motion"],
                "score": d["score"],
                **{f"f_{FEATURE_NAMES[i]}": d["f"][i] for i in range(N_FEATURES)},
                **{f"sc{i:02d}": round(v, 4) for i, v in enumerate(dev)},
            })

            elapsed = time.monotonic() - t_start
            if len(rows) % 10 == 0:
                bar_len = 30
                done = int(bar_len * elapsed / seconds)
                print(f"  [{'#' * done}{'.' * (bar_len - done)}] "
                      f"{elapsed:5.0f}/{seconds}s   {len(rows)} samples   "
                      f"score {d['score']:6.1f}", end="\r", flush=True)

    except KeyboardInterrupt:
        print("\n  stopped early - keeping what was collected.")

    print(f"\n  collected {len(rows)} samples for '{label}'"
          + (f" ({misses} failed requests)" if misses else ""))
    return rows


# --------------------------------------------------------------------------
# Saving and sanity checking
# --------------------------------------------------------------------------

def save(rows, out_dir):
    if not rows:
        print("\nNothing recorded, nothing saved.")
        return None

    os.makedirs(out_dir, exist_ok=True)
    name = time.strftime("session_%Y%m%d_%H%M%S.csv")
    path = os.path.join(out_dir, name)

    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    print(f"\nSaved {len(rows)} rows to {path}")
    return path


def sanity_check(rows):
    """
    The single most useful thing to know before training: do the classes
    actually look different? If they do not, no model can separate them and
    the problem is with the recording, not the algorithm.
    """
    by_label = {}
    for r in rows:
        by_label.setdefault(r["label"], []).append(r["f_mean_absdiff"])

    if len(by_label) < 2:
        return

    print("\n--- Quick check -------------------------------------------")
    stats = {}
    for label, vals in by_label.items():
        mean = statistics.fmean(vals)
        sd = statistics.pstdev(vals) if len(vals) > 1 else 0.0
        stats[label] = (mean, sd, len(vals))
        print(f"  {label:<12} {len(vals):5d} samples   "
              f"signal {mean:.5f} +/- {sd:.5f}")

    labels = list(stats)
    (m1, s1, _), (m2, s2, _) = stats[labels[0]], stats[labels[1]]
    pooled = math.sqrt((s1 ** 2 + s2 ** 2) / 2)
    sep = abs(m1 - m2) / pooled if pooled > 1e-12 else 0.0

    print(f"\n  Separation between '{labels[0]}' and '{labels[1]}': {sep:.1f}")
    if sep > 2.0:
        print("  GOOD - the two classes look clearly different.")
    elif sep > 1.0:
        print("  OK - usable, but more movement while recording would help.")
    else:
        print("  POOR - the classes look almost the same.")
        print("  Likely causes: you did not move enough during the motion")
        print("  session, or the board is too far from the router.")
    print("-----------------------------------------------------------")


# --------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description="Record training data from the detector.")
    p.add_argument("--ip", required=True,
                   help="address shown on the board's serial output, e.g. 192.168.1.47")
    p.add_argument("--minutes", type=float, default=3.0,
                   help="minutes per class in guided mode (default 3)")
    p.add_argument("--label", help="record a single custom class instead of the guided run")
    p.add_argument("--out", default="data", help="output folder (default: data)")
    args = p.parse_args()

    print("=" * 60)
    print("  Wi-Fi Motion Detector - training data recorder")
    print("=" * 60)

    wait_for_device(args.ip)
    wait_until_warm(args.ip)

    secs = int(args.minutes * 60)

    if args.label:
        countdown(5, f"Recording '{args.label}' for {args.minutes:g} minutes.")
        rows = record_session(args.ip, args.label, secs)
    else:
        print("\nTwo recordings, about %g minutes each." % args.minutes)
        print("Press Ctrl+C at any time to stop early.")

        countdown(10, ">>> PART 1 of 2: STILL\n"
                      "    Leave the room, or sit completely still.\n"
                      "    Do not move at all.")
        still = record_session(args.ip, "still", secs)

        countdown(10, ">>> PART 2 of 2: MOVING\n"
                      "    Walk around the room the whole time.\n"
                      "    Move between the board and the router.")
        moving = record_session(args.ip, "moving", secs)

        rows = still + moving

    path = save(rows, args.out)
    if path:
        sanity_check(rows)
        print(f"\nNext: python train.py {path}")


if __name__ == "__main__":
    main()
