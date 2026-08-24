# Step 4 — Record training data

## What this does

Machine learning needs examples. This script collects them.

It asks you to do two things:

1. **Stay still** for a few minutes — this teaches the model what an empty room
   looks like.
2. **Walk around** for a few minutes — this teaches it what a person looks like.

It saves both into one CSV file. Step 5 trains the model on that file.

## How to run it

The board must be powered on and connected to Wi-Fi. You need its address —
the same one you type into your browser.

Open a terminal in your project folder and run:

```
python tools\record.py --ip 192.168.1.47
```

Replace the numbers with your board's address.

That's it. The script talks you through the rest.

## What you will see

```
Looking for the detector at http://192.168.1.47/ ...
  found it. Sample rate 49 Hz.

Two recordings, about 3 minutes each.

>>> PART 1 of 2: STILL
    Leave the room, or sit completely still.
    Do not move at all.
  starting in 10...
  [##########....................]    62/180s   620 samples   score    0.4
```

Then it counts you into Part 2 and asks you to walk around.

At the end:

```
Saved 3600 rows to data\session_20260825_021400.csv

--- Quick check -------------------------------------------
  still         1800 samples   signal 0.01480 +/- 0.00030
  moving        1800 samples   signal 0.04310 +/- 0.00840

  Separation between 'still' and 'moving': 4.7
  GOOD - the two classes look clearly different.
-----------------------------------------------------------
```

**That separation number is the important part.** It says whether the two
recordings actually look different from each other.

- Above 2 → good, go to Step 5
- 1 to 2 → usable, but move around more next time
- Below 1 → something is wrong, tell me and we will fix it

## Tips for good data

- During **still**, actually be still. Leaving the room is best.
- During **moving**, keep moving the whole time. Walk between the board and
  the router — that path is where the signal is most affected.
- Do both in the same room, in the same session. Don't record "still" today
  and "moving" tomorrow.
- If you want more data later, just run the script again. Step 5 can use
  several files.

## Recording extra situations

You can record other things separately, for example a fan running, so the
model learns not to confuse it with a person:

```
python tools\record.py --ip 192.168.1.47 --label fan_on --minutes 2
```

## Why Wi-Fi and not the USB cable

The script reads data over Wi-Fi from the same web address you open in your
browser. This means:

- No serial port to pick, no baud rate to get wrong
- No conflict with `idf.py monitor`
- You can unplug the USB cable and power the board from a phone charger

It also needs **no extra Python packages** — everything it uses comes with
Python itself.

## What is in the file

Each row is one decision from the board, ten times a second:

| Columns | What |
|---|---|
| `t`, `label` | time, and whether you were still or moving |
| `device_motion`, `score` | what the board's own detector thought |
| `f_*` (8 columns) | the eight features the board calculates |
| `sc00`–`sc51` (52 columns) | how far each subcarrier moved from normal |

64 columns in total. The 52 subcarrier columns are the raw material — the model
in Step 5 can find patterns in them that the eight hand-written features miss.

## Verified

The script was run end-to-end against a simulated detector. It correctly
produced 64 columns, both labels, no duplicate or missing decisions, and the
sanity check correctly reported "POOR" when the simulated data had no real
difference between classes — proving the check actually works rather than
always saying yes.

## Next

Step 5: train the model in Python and see how accurate it is.
