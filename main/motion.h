/*
 * motion.h - Feature extraction and motion detection.
 *
 * Takes the raw CSI amplitude stream from csi_capture and turns it into a
 * yes/no motion decision, plus a feature vector that Step 5's machine
 * learning model will be trained on.
 *
 * Two ideas do the heavy lifting:
 *
 *   1. NORMALISE EVERY SAMPLE.
 *      The ESP32's automatic gain control constantly rescales the raw CSI
 *      magnitudes. If you skip this, you measure the AGC, not the room.
 *      Dividing each sample's 52 amplitudes by their own mean removes the
 *      gain entirely and leaves only the *shape* of the channel response -
 *      which is the part motion actually changes.
 *
 *   2. LEARN THE ROOM, DON'T HARDCODE A THRESHOLD.
 *      Every room has a different amount of baseline fluctuation. So instead
 *      of a fixed cut-off, the detector continuously estimates what "quiet"
 *      looks like here and flags departures from it in units of standard
 *      deviation. The same firmware works in a small bedroom and a large hall
 *      with no retuning.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "csi_capture.h"

/* ---------------------------------------------------------------------------
 * Tunables
 * ------------------------------------------------------------------------ */

/* Analysis window: one second's worth of samples. One second comfortably
 * contains a footstep. Longer windows smooth more but react slower.
 *
 * These must be integer constant expressions - the ring buffer is a
 * file-scope array, and anything involving a float cast would make it a
 * variable-length array, which is not valid there. */
#define MOTION_WINDOW_N          (TRAFFIC_RATE_HZ)

/* Decision every ~0.1 s, i.e. 10 decisions per second. Rounds up so it is
 * never zero even at the minimum configurable rate. */
#define MOTION_HOP_N             (((TRAFFIC_RATE_HZ) + 9) / 10)

/* The same quantities in seconds, for logging and time-constant maths. */
#define MOTION_WINDOW_SEC        ((float)MOTION_WINDOW_N / (float)TRAFFIC_RATE_HZ)
#define MOTION_HOP_SEC           ((float)MOTION_HOP_N / (float)TRAFFIC_RATE_HZ)

/* Detection thresholds, in standard deviations above the learned quiet level.
 * Two different values give hysteresis: it takes a strong signal to trigger,
 * but a weaker one to stay triggered. Without this the LED chatters on and
 * off at the boundary. */
#define MOTION_Z_ON              4.0f
#define MOTION_Z_OFF             2.0f

/* Guard band. The baseline stops learning above this z-score, even though it
 * is below the detection threshold.
 *
 * Without it, weak-but-sustained motion is quietly absorbed into the baseline:
 * the "quiet" level creeps up to match it, the score decays back to zero, and
 * the detector goes blind to exactly the subtle activity you most want to
 * catch. Measured on synthetic data, this guard is the difference between
 * detecting disturbances down to ~40% of a walking body and needing ~90%. */
#define MOTION_Z_FREEZE          1.5f

/* Consecutive windows required to change state. ON is deliberately short
 * (fast response); OFF is long so brief stillness mid-walk does not clear it. */
#define MOTION_CONSEC_ON         2
#define MOTION_CONSEC_OFF        10

/* Windows of quiet needed before the detector will fire at all. At 10 Hz this
 * is a 10-second warm-up while it learns the room. */
#define MOTION_WARMUP_WINDOWS    100

/* If motion stays asserted this long, something is wrong (the baseline was
 * learned during motion, or the environment changed permanently). Force a
 * re-learn rather than latching on forever. */
#define MOTION_STUCK_SEC         120.0f

/* ---------------------------------------------------------------------------
 * Feature vector
 *
 * These eight numbers are what Step 5 trains on. Order matters and must stay
 * in sync with the Python tooling.
 * ------------------------------------------------------------------------ */
#define MOTION_N_FEATURES 8

typedef struct {
    float mean_var;      /* [0] mean across subcarriers of within-window variance */
    float max_var;       /* [1] the single most disturbed subcarrier             */
    float mean_absdiff;  /* [2] mean |x[t]-x[t-1]| - the primary motion statistic */
    float baseline_dist; /* [3] 1 - cosine similarity vs the learned quiet profile */
    float amp_mean;      /* [4] mean raw amplitude (link strength context)        */
    float amp_var;       /* [5] variance of per-sample bulk power                 */
    float rssi_std;      /* [6] RSSI spread over the window                       */
    float zcr;           /* [7] zero-crossing rate - crude motion-speed proxy     */
} motion_features_t;

/* Current detector state, safe to read from any task (dashboard, logging). */
typedef struct {
    bool     motion;            /* the decision                                */
    float    score;             /* z-score of the primary statistic            */
    float    baseline_level;    /* learned quiet level                         */
    float    baseline_spread;   /* learned quiet variability                   */
    bool     warmed_up;         /* false while still learning the room         */
    uint32_t windows;           /* analysis windows processed                  */
    uint32_t events;            /* off->on transitions since boot              */
    int64_t  last_change_us;    /* when the state last flipped                 */
    motion_features_t f;        /* most recent feature vector                  */
} motion_state_t;

/* Called once at startup. */
void motion_init(void);

/*
 * Feed one CSI sample. Call this from the csi_capture callback - it is cheap
 * and only does real work once every MOTION_HOP_N samples.
 *
 * Returns true on the samples where a new decision was produced.
 */
bool motion_push(const csi_sample_t *s);

/* Snapshot the current state. */
void motion_get_state(motion_state_t *out);

/* Throw away the learned baseline and start over. Useful after moving the
 * device or changing rooms. */
void motion_reset_baseline(void);

/*
 * Copy the latest normalised amplitude vector and the learned quiet profile,
 * for display. Both arrays must hold CSI_SC_COUNT floats.
 *
 * Normalised values hover around 1.0. Where the live trace departs from the
 * baseline trace, that subcarrier is being disturbed - which is exactly what
 * the dashboard draws.
 */
void motion_get_display(float *amp_out, float *base_out);

/* Flatten the most recent feature vector into an array, for logging and for
 * the model. Order matches motion_features_t exactly. */
void motion_features_to_array(const motion_features_t *f, float *out);

/* Human-readable feature names, MOTION_N_FEATURES entries. */
extern const char *motion_feature_names[MOTION_N_FEATURES];
