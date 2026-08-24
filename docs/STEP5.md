# Step 5 — Train the model

## What this does

It reads the file you recorded in Step 4, tries five different machine learning
models, measures how good each one is, and saves the best one.

## Before you run it

You need two Python packages. Install them once:

```
pip install numpy scikit-learn
```

## How to run it

```
python tools\train.py data\session_20260825_021400.csv
```

Use your own file name. If you recorded more than once, you can use them all:

```
python tools\train.py data\*.csv
```

## The important thing this script does

There is a very common mistake in machine learning that makes projects look
brilliant and be useless. This script is built to avoid it, and to show you
that it did.

**The mistake:** your data is recorded over time. Two rows recorded a tenth of
a second apart are almost identical. If you shuffle all the rows and randomly
split them into "training" and "testing", then almost every test row has a
near-twin in the training set. The model does not have to *understand* motion.
It only has to *remember*. You get 99% accuracy and a detector that fails
immediately in real life.

**The fix:** split by blocks of time, not by rows. Five seconds of recording
goes entirely into training, or entirely into testing. The model is always
tested on moments it has genuinely never seen.

The script prints both numbers:

```
model                   HONEST                              leaky
Logistic Regression      0.736  ##################......    0.751
Random Forest            0.720  #################.......    0.769
Gradient Boosting        0.712  #################.......    0.778
```

**HONEST is the real number.** The `leaky` column is what you would have
believed if you did the normal thing. The gap between them is the size of the
lie.

If someone asks about your project, this is the part worth talking about.

## What you will see

```
Best model: Random Forest   accuracy 91.4%

What it got right and wrong:
                    predicted
              moving     still
  actually moving        1652       148
  actually still           62      1738

  label         precision   recall   f1
  moving            0.964    0.918   0.940
  still             0.921    0.966   0.943
```

Reading the table: of 1800 real "moving" samples, it correctly called 1652 and
missed 148.

- **precision** — when it says motion, how often it is right
- **recall** — of all the real motion, how much it caught

Then it shows which measurements mattered most:

```
Most useful measurements:
  hist_max        ####################
  dev_rough       #################...
  f_mean_absdiff  #################...
```

Finally it saves `model.pkl`.

## What score to expect

| Accuracy | Meaning |
|---|---|
| Above 90% | Very good |
| 80–90% | Good, normal for a first try |
| 75–80% | Works, but record more data |
| Below 75% | Something is wrong — tell me |
| Above 99.5% | Suspicious. Probably you moved during the "still" recording |

## What the model looks at

20 numbers per decision, in three groups:

1. **The 8 features the board already calculates.**
2. **8 summary numbers about the 52 subcarriers** — the average disturbance,
   the worst one, how spread out it is, and so on.
3. **4 numbers about the last second** — so a brief pause while walking does
   not look like an empty room.

Note it does *not* feed the 52 raw subcarrier values in directly. That pattern
depends on exactly where your furniture and router are. A model trained on it
memorises your specific room instead of learning what motion looks like.

## Verified

The script was tested end-to-end on two synthetic datasets:

- **Easy data** (clearly separated classes) — all five models reached 100%, and
  the script correctly warned that a perfect score is suspicious.
- **Hard data** (heavily overlapping classes) — the honest score came out lower
  than the leaky score for every single model, with gaps up to 6.6 points.
  This proves the time-block splitting is genuinely doing its job and not just
  printing a second number that happens to match.

## Next

Step 6: run the trained model live, so you can see it making predictions in
real time instead of just reading a score from a file.
