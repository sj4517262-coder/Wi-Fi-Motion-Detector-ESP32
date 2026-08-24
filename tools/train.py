#!/usr/bin/env python3
"""
train.py - Train a motion classifier on data recorded by record.py.

    python train.py data\session_20260825_021400.csv
    python train.py data\*.csv                       (several files at once)

It compares five different models, measures them honestly, prints the results,
and saves the best one to model.pkl.

------------------------------------------------------------------------------
THE ONE THING THAT MATTERS MOST

Data recorded over time is not like a pile of independent photos. Two rows
recorded a tenth of a second apart are almost identical.

If you shuffle all the rows and split them randomly into training and testing
sets - the normal thing to do - then for nearly every test row there is an
almost identical twin sitting in the training set. The model does not have to
learn anything about motion. It just has to remember. You get 99% accuracy and
a detector that fails the moment you use it in real life.

The fix is to split by TIME BLOCKS, never by individual rows. Whole chunks of
several seconds go entirely into training or entirely into testing, so the
model is always tested on moments it has never seen.

This script reports both numbers so you can see the size of the lie.
------------------------------------------------------------------------------
"""

import argparse
import glob
import pickle
import sys
from collections import Counter

try:
    import numpy as np
    from sklearn.ensemble import GradientBoostingClassifier, RandomForestClassifier
    from sklearn.linear_model import LogisticRegression
    from sklearn.metrics import confusion_matrix, precision_recall_fscore_support
    from sklearn.model_selection import GroupKFold, KFold, cross_val_score
    from sklearn.neural_network import MLPClassifier
    from sklearn.pipeline import make_pipeline
    from sklearn.preprocessing import StandardScaler
    from sklearn.svm import SVC
except ImportError:
    print("This step needs two Python packages. Install them with:\n")
    print("    pip install numpy scikit-learn\n")
    sys.exit(1)


DEVICE_FEATURES = ["f_mean_var", "f_max_var", "f_mean_absdiff", "f_baseline_dist",
                   "f_amp_mean", "f_amp_var", "f_rssi_std", "f_zcr"]

BLOCK_SECONDS = 5.0     # size of a time block for honest splitting
HISTORY = 10            # rows of context for the "recent past" features (~1 s)


# --------------------------------------------------------------------------
# Loading
# --------------------------------------------------------------------------

def load(paths):
    """Read the CSVs. Each file is its own recording session."""
    import csv as csvmod

    rows, session_of = [], []
    for session_id, path in enumerate(paths):
        with open(path, newline="") as fh:
            got = list(csvmod.DictReader(fh))
        if not got:
            print(f"  {path}: empty, skipped")
            continue
        rows.extend(got)
        session_of.extend([session_id] * len(got))
        print(f"  {path}: {len(got)} rows")
    return rows, session_of


# --------------------------------------------------------------------------
# Feature building
# --------------------------------------------------------------------------

def build_features(rows):
    """
    Turn raw CSV rows into the numbers the model sees.

    Three groups:
      1. The eight features the board already computes.
      2. Summary statistics of the 52 subcarrier deviations. Summaries rather
         than the 52 raw values, because the raw pattern is specific to where
         the furniture and the router happen to be - a model trained on those
         memorises your living room instead of learning what motion looks like.
      3. A short view of the recent past, so a brief still moment in the middle
         of walking does not look like an empty room.
    """
    sc_cols = [f"sc{i:02d}" for i in range(52)]
    n = len(rows)

    dev = np.array([[float(r[c]) for c in sc_cols] for r in rows])   # (n, 52)
    devabs = np.abs(dev)

    base = np.array([[float(r[c]) for c in DEVICE_FEATURES] for r in rows])

    summary = np.column_stack([
        devabs.mean(axis=1),                       # overall disturbance
        devabs.std(axis=1),                        # is it spread out or focused
        devabs.max(axis=1),                        # worst single subcarrier
        np.percentile(devabs, 90, axis=1),         # robust version of the max
        np.percentile(devabs, 50, axis=1),         # typical subcarrier
        (devabs > devabs.mean() * 2).sum(axis=1),  # how many are badly hit
        dev.mean(axis=1),                          # net bias up or down
        np.abs(np.diff(dev, axis=1)).mean(axis=1),  # roughness across frequency
    ])

    energy = devabs.mean(axis=1)
    hist = np.zeros((n, 4))
    for i in range(n):
        lo = max(0, i - HISTORY + 1)
        w = energy[lo:i + 1]
        hist[i] = [w.mean(), w.std(), w.max(), w.max() - w.min()]

    names = (DEVICE_FEATURES
             + ["dev_mean", "dev_std", "dev_max", "dev_p90", "dev_p50",
                "dev_nhit", "dev_bias", "dev_rough"]
             + ["hist_mean", "hist_std", "hist_max", "hist_range"])

    X = np.column_stack([base, summary, hist])
    X = np.nan_to_num(X, nan=0.0, posinf=0.0, neginf=0.0)
    return X, names


def build_groups(rows, session_of):
    """
    Assign every row to a time block. Blocks are the unit that gets split into
    training or testing, and a block never straddles a session or a label.
    """
    groups, gid, last_key = [], -1, None
    for r, sess in zip(rows, session_of):
        key = (sess, r["label"], int(float(r["t"]) // BLOCK_SECONDS))
        if key != last_key:
            gid += 1
            last_key = key
        groups.append(gid)
    return np.array(groups)


# --------------------------------------------------------------------------
# Evaluation
# --------------------------------------------------------------------------

def models():
    return {
        "Logistic Regression": make_pipeline(
            StandardScaler(), LogisticRegression(max_iter=2000)),
        "Random Forest": RandomForestClassifier(
            n_estimators=200, min_samples_leaf=3, random_state=0, n_jobs=-1),
        "Gradient Boosting": GradientBoostingClassifier(random_state=0),
        "SVM (RBF)": make_pipeline(
            StandardScaler(), SVC(C=2.0, gamma="scale")),
        "Neural Net": make_pipeline(
            StandardScaler(),
            MLPClassifier(hidden_layer_sizes=(32, 16), max_iter=1500,
                          random_state=0)),
    }


def bar(value, width=24):
    filled = int(round(value * width))
    return "#" * filled + "." * (width - filled)


def main():
    ap = argparse.ArgumentParser(description="Train the motion classifier.")
    ap.add_argument("files", nargs="+", help="CSV files from record.py")
    ap.add_argument("--out", default="model.pkl", help="where to save the model")
    args = ap.parse_args()

    paths = []
    for pattern in args.files:
        paths.extend(sorted(glob.glob(pattern)) or [pattern])

    print("=" * 62)
    print("  Training the motion classifier")
    print("=" * 62)
    print("\nLoading:")
    rows, session_of = load(paths)

    if not rows:
        print("\nNo data found.")
        sys.exit(1)

    counts = Counter(r["label"] for r in rows)
    print("\nLabels found:")
    for k, v in counts.items():
        print(f"  {k:<12} {v} samples")

    if len(counts) < 2:
        print("\nOnly one label. The model needs at least two different "
              "situations to tell apart. Record again with record.py.")
        sys.exit(1)

    smallest = min(counts.values())
    if smallest < 200:
        print(f"\nWARNING: '{min(counts, key=counts.get)}' has only {smallest} "
              "samples. Results will be unreliable. Aim for 1500+ per label.")

    X, names = build_features(rows)
    y = np.array([r["label"] for r in rows])
    groups = build_groups(rows, session_of)

    n_blocks = len(set(groups))
    print(f"\nBuilt {X.shape[0]} samples x {X.shape[1]} features, "
          f"in {n_blocks} time blocks of {BLOCK_SECONDS:g}s.")

    if n_blocks < 10:
        print("WARNING: very few time blocks. Record for longer.")

    n_splits = min(5, n_blocks // 2)
    if n_splits < 2:
        print("Not enough data to test properly.")
        sys.exit(1)

    # ---- the honest comparison ------------------------------------------
    print("\n" + "-" * 62)
    print("  Scores  (higher is better, 1.00 is perfect)")
    print("-" * 62)
    print(f"{'model':<22}{'HONEST':>8}  {'':<26}{'leaky':>7}")

    gkf = GroupKFold(n_splits=n_splits)
    kf = KFold(n_splits=5, shuffle=True, random_state=0)

    results = {}
    for name, model in models().items():
        honest = cross_val_score(model, X, y, cv=gkf, groups=groups,
                                 scoring="accuracy", n_jobs=1).mean()
        leaky = cross_val_score(model, X, y, cv=kf,
                                scoring="accuracy", n_jobs=1).mean()
        results[name] = (honest, leaky, model)
        print(f"{name:<22}{honest:>8.3f}  {bar(honest):<26}{leaky:>7.3f}")

    best_name = max(results, key=lambda k: results[k][0])
    best_honest, best_leaky, best_model = results[best_name]

    print("-" * 62)
    print(f"\n  HONEST = tested on time periods the model never saw. Trust this.")
    print(f"  leaky  = tested on shuffled rows. Looks better, means nothing.")
    gap = best_leaky - best_honest
    if gap > 0.05:
        print(f"\n  The gap for the best model is {gap:.3f}. That gap is exactly")
        print("  how much a shuffled split would have flattered you.")

    # ---- detail on the winner -------------------------------------------
    print(f"\nBest model: {best_name}   accuracy {best_honest:.1%}")

    labels = sorted(set(y))
    y_pred = np.empty_like(y)
    for tr, te in gkf.split(X, y, groups):
        m = models()[best_name]
        m.fit(X[tr], y[tr])
        y_pred[te] = m.predict(X[te])

    cm = confusion_matrix(y, y_pred, labels=labels)
    print("\nWhat it got right and wrong:")
    width = max(len(l) for l in labels) + 2
    print(" " * (width + 12) + "predicted")
    print(" " * (width + 2) + "".join(f"{l:>10}" for l in labels))
    for i, l in enumerate(labels):
        print(f"  actually {l:<{width}}" + "".join(f"{v:>10}" for v in cm[i]))

    prec, rec, f1, _ = precision_recall_fscore_support(
        y, y_pred, labels=labels, zero_division=0)
    print("\n  label         precision   recall   f1")
    for i, l in enumerate(labels):
        print(f"  {l:<12}  {prec[i]:>9.3f} {rec[i]:>8.3f} {f1[i]:>7.3f}")

    print("\n  precision = when it says motion, how often it is right")
    print("  recall    = of all the real motion, how much it caught")

    # ---- what the model actually pays attention to -----------------------
    fitted = models()[best_name]
    fitted.fit(X, y)

    importances = None
    if hasattr(fitted, "feature_importances_"):
        importances = fitted.feature_importances_
    elif hasattr(fitted, "steps") and hasattr(fitted.steps[-1][1], "coef_"):
        importances = np.abs(fitted.steps[-1][1].coef_).ravel()

    if importances is not None:
        order = np.argsort(importances)[::-1][:8]
        top = importances[order[0]] or 1.0
        print("\nMost useful measurements:")
        for i in order:
            print(f"  {names[i]:<16}{bar(importances[i] / top, 20)}")

    # ---- save -------------------------------------------------------------
    with open(args.out, "wb") as fh:
        pickle.dump({
            "model": fitted,
            "feature_names": names,
            "labels": labels,
            "accuracy": float(best_honest),
            "model_name": best_name,
            "history": HISTORY,
        }, fh)

    print(f"\nSaved to {args.out}")

    if best_honest < 0.75:
        print("\nThis accuracy is low. Usually that means the two recordings")
        print("did not look different enough. Try recording again, moving more")
        print("and staying closer to the line between board and router.")
    elif best_honest > 0.995:
        print("\nThis is suspiciously perfect. Check that you really did stay")
        print("still during the 'still' recording.")


if __name__ == "__main__":
    main()
