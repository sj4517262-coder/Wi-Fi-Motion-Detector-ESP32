#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "motion.h"
#include "led.h"

static const char *TAG = "motion";

const char *motion_feature_names[MOTION_N_FEATURES] = {
    "mean_var", "max_var", "mean_absdiff", "baseline_dist",
    "amp_mean", "amp_var", "rssi_std", "zcr",
};

/* ---------------------------------------------------------------------------
 * Ring buffer of normalised amplitude vectors.
 *
 * Statically allocated: at 50 Hz this is 50 x 52 x 4 = ~10 kB. Doing it
 * statically avoids heap fragmentation on a device meant to run for weeks.
 * ------------------------------------------------------------------------ */
static float    s_ring[MOTION_WINDOW_N][CSI_SC_COUNT];
static float    s_bulk[MOTION_WINDOW_N];   /* per-sample mean raw amplitude */
static int8_t   s_rssi[MOTION_WINDOW_N];
static int      s_head;                    /* next write position            */
static int      s_count;                   /* samples held, saturates at N   */
static int      s_since_hop;

/* Most recent normalised sample, kept purely so the dashboard has something
 * live to draw. Not used by the detector itself. */
static float    s_last_norm[CSI_SC_COUNT];

/* Long-term "quiet room" profile, and the running quiet statistics of the
 * primary detection statistic. */
static float    s_baseline_profile[CSI_SC_COUNT];
static bool     s_baseline_valid;
static float    s_quiet_level;     /* EMA of mean_absdiff while quiet        */
static float    s_quiet_dev;       /* EMA of |mean_absdiff - level|          */
static uint32_t s_quiet_updates;

/* Detector state machine. */
static bool     s_motion;
static int      s_consec_on;
static int      s_consec_off;
static float    s_score;
static uint32_t s_windows;
static uint32_t s_events;
static int64_t  s_last_change_us;
static motion_features_t s_features;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------------ */

void motion_init(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    motion_reset_baseline();
    s_head = s_count = s_since_hop = 0;
    s_motion = false;
    s_windows = s_events = 0;
    s_last_change_us = esp_timer_get_time();

    ESP_LOGI(TAG, "window=%d samples (%.1f s), hop=%d samples (%.0f decisions/s)",
             MOTION_WINDOW_N, MOTION_WINDOW_SEC, MOTION_HOP_N,
             1.0f / MOTION_HOP_SEC);
    ESP_LOGI(TAG, "learning the room - detection starts in ~%.0f s",
             MOTION_WARMUP_WINDOWS * MOTION_HOP_SEC);
}

void motion_reset_baseline(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(s_baseline_profile, 0, sizeof(s_baseline_profile));
    s_baseline_valid = false;
    s_quiet_level    = 0.0f;
    s_quiet_dev      = 0.0f;
    s_quiet_updates  = 0;
    s_consec_on      = 0;
    s_consec_off     = 0;
    portEXIT_CRITICAL(&s_lock);
}

/* ---------------------------------------------------------------------------
 * Ingest
 * ------------------------------------------------------------------------ */

bool motion_push(const csi_sample_t *s)
{
    /* --- normalise away the automatic gain control --------------------- */
    float sum = 0.0f;
    for (int k = 0; k < CSI_SC_COUNT; k++) {
        sum += s->amp[k];
    }
    const float mean = sum / CSI_SC_COUNT;

    /* A near-zero mean means a corrupt or empty record - drop it rather than
     * dividing by it and injecting a huge spike into the window. */
    if (mean < 1e-3f) {
        return false;
    }

    const float inv = 1.0f / mean;
    for (int k = 0; k < CSI_SC_COUNT; k++) {
        s_ring[s_head][k] = s->amp[k] * inv;
    }
    memcpy(s_last_norm, s_ring[s_head], sizeof(s_last_norm));
    s_bulk[s_head] = mean;
    s_rssi[s_head] = s->rssi;

    s_head = (s_head + 1) % MOTION_WINDOW_N;
    if (s_count < MOTION_WINDOW_N) {
        s_count++;
    }

    /* --- only analyse every hop, and only once the window is full ------- */
    if (++s_since_hop < MOTION_HOP_N || s_count < MOTION_WINDOW_N) {
        return false;
    }
    s_since_hop = 0;

    /* ------------------------------------------------------------------- *
     * Feature extraction over the full window.
     *
     * The ring is walked in chronological order: oldest sample is at s_head
     * (which now points one past the newest).
     * ------------------------------------------------------------------- */
    const int N = MOTION_WINDOW_N;
    motion_features_t f;
    memset(&f, 0, sizeof(f));

    float profile[CSI_SC_COUNT];   /* window-mean normalised profile */
    float var_sum = 0.0f;
    float var_max = 0.0f;
    float absdiff_sum = 0.0f;

    for (int k = 0; k < CSI_SC_COUNT; k++) {
        float sx = 0.0f, sxx = 0.0f, sad = 0.0f;
        float prev = 0.0f;

        for (int t = 0; t < N; t++) {
            const int idx = (s_head + t) % N;
            const float x = s_ring[idx][k];
            sx  += x;
            sxx += x * x;
            if (t > 0) {
                sad += fabsf(x - prev);
            }
            prev = x;
        }

        const float m = sx / N;
        /* max(0,...) guards against tiny negatives from float cancellation */
        float v = sxx / N - m * m;
        if (v < 0.0f) {
            v = 0.0f;
        }

        profile[k] = m;
        var_sum   += v;
        if (v > var_max) {
            var_max = v;
        }
        absdiff_sum += sad / (N - 1);
    }

    f.mean_var     = var_sum / CSI_SC_COUNT;
    f.max_var      = var_max;
    f.mean_absdiff = absdiff_sum / CSI_SC_COUNT;

    /* --- bulk power and RSSI statistics -------------------------------- */
    float bsum = 0.0f, bsq = 0.0f, rsum = 0.0f, rsq = 0.0f;
    for (int t = 0; t < N; t++) {
        const float b = s_bulk[t];
        const float r = (float)s_rssi[t];
        bsum += b;  bsq += b * b;
        rsum += r;  rsq += r * r;
    }
    const float bm = bsum / N;
    const float rm = rsum / N;
    f.amp_mean = bm;
    f.amp_var  = fmaxf(0.0f, bsq / N - bm * bm);
    f.rssi_std = sqrtf(fmaxf(0.0f, rsq / N - rm * rm));

    /* --- zero-crossing rate of the mean-centred aggregate --------------
     * A crude but cheap frequency proxy: how often the summed normalised
     * amplitude crosses its own mean. Slow body movement gives a low rate,
     * fast movement (or a fan, or interference) gives a high one. */
    {
        int crossings = 0;
        float prev_dev = 0.0f;
        for (int t = 0; t < N; t++) {
            const int idx = (s_head + t) % N;
            float agg = 0.0f;
            for (int k = 0; k < CSI_SC_COUNT; k++) {
                agg += s_ring[idx][k];
            }
            /* normalised vectors have mean 1.0 by construction, so the
             * expected aggregate is exactly CSI_SC_COUNT */
            const float dev = agg - (float)CSI_SC_COUNT;
            if (t > 0 && ((dev > 0.0f) != (prev_dev > 0.0f))) {
                crossings++;
            }
            prev_dev = dev;
        }
        f.zcr = (float)crossings / (float)(N - 1);
    }

    /* --- distance from the learned quiet profile ------------------------
     * Cosine similarity is scale-invariant, so this measures a change in the
     * channel's *shape*. It catches a person who has moved into the room and
     * then stopped - which the motion statistic alone would miss. */
    if (s_baseline_valid) {
        float dot = 0.0f, na = 0.0f, nb = 0.0f;
        for (int k = 0; k < CSI_SC_COUNT; k++) {
            dot += profile[k] * s_baseline_profile[k];
            na  += profile[k] * profile[k];
            nb  += s_baseline_profile[k] * s_baseline_profile[k];
        }
        const float denom = sqrtf(na) * sqrtf(nb);
        f.baseline_dist = (denom > 1e-9f) ? (1.0f - dot / denom) : 0.0f;
    } else {
        f.baseline_dist = 0.0f;
    }

    /* ------------------------------------------------------------------- *
     * Detection
     *
     * The primary statistic is mean_absdiff: the average sample-to-sample
     * change in each subcarrier. Variance would also work, but it counts slow
     * thermal drift as signal. A first difference is naturally high-pass and
     * therefore drift-immune, which matters a lot over long runs.
     * ------------------------------------------------------------------- */
    const float stat = f.mean_absdiff;

    float score = 0.0f;
    bool  warmed = s_quiet_updates >= MOTION_WARMUP_WINDOWS;

    if (warmed) {
        /* Robust spread estimate: mean absolute deviation scaled to be
         * comparable to a standard deviation for roughly normal data. A true
         * standard deviation is far too easily inflated by a single outlier. */
        const float sigma = s_quiet_dev * 1.4826f;
        score = (stat - s_quiet_level) / (sigma > 1e-9f ? sigma : 1e-9f);
    }

    const int64_t now = esp_timer_get_time();
    const int64_t prev_change_us = s_last_change_us;   /* for the duration log */
    bool state_changed = false;

    if (!s_motion) {
        if (warmed && score > MOTION_Z_ON) {
            if (++s_consec_on >= MOTION_CONSEC_ON) {
                s_motion = true;
                s_consec_on = 0;
                s_events++;
                s_last_change_us = now;
                state_changed = true;
            }
        } else {
            s_consec_on = 0;
        }
    } else {
        if (score < MOTION_Z_OFF) {
            if (++s_consec_off >= MOTION_CONSEC_OFF) {
                s_motion = false;
                s_consec_off = 0;
                s_last_change_us = now;
                state_changed = true;
            }
        } else {
            s_consec_off = 0;
        }

        /* Safety valve. If we have been "in motion" for two minutes straight,
         * the baseline is almost certainly wrong - most likely it was learned
         * while someone was already moving, or the AP changed channel. Relearn
         * rather than sitting latched on forever. */
        if ((now - s_last_change_us) > (int64_t)(MOTION_STUCK_SEC * 1e6f)) {
            ESP_LOGW(TAG, "motion asserted for %.0f s - relearning baseline",
                     MOTION_STUCK_SEC);
            s_motion = false;
            state_changed = true;
            s_last_change_us = now;
            motion_reset_baseline();
        }
    }

    /* --- baseline update ------------------------------------------------
     * Only learn while genuinely quiet. Updating during motion would let the
     * baseline chase the disturbance, the score would collapse to zero, and
     * the detector would go permanently blind - the classic failure mode of
     * adaptive thresholds.
     *
     * The guard band (score < MOTION_Z_FREEZE) extends that protection below
     * the detection threshold. Anything already looking mildly unusual is not
     * allowed to teach the baseline that it is normal. */
    const bool learning_ok = !s_motion &&
                             (s_quiet_updates < MOTION_WARMUP_WINDOWS ||
                              score < MOTION_Z_FREEZE);

    if (learning_ok) {
        if (s_quiet_updates == 0) {
            s_quiet_level = stat;
            s_quiet_dev   = stat * 0.1f;
            memcpy(s_baseline_profile, profile, sizeof(profile));
            s_baseline_valid = true;
        } else {
            /* ~10 s time constant at 10 Hz: slow enough to ignore a person
             * walking through, fast enough to track furniture being moved. */
            const float a = 0.01f;
            s_quiet_level = (1.0f - a) * s_quiet_level + a * stat;
            s_quiet_dev   = (1.0f - a) * s_quiet_dev   + a * fabsf(stat - s_quiet_level);
            for (int k = 0; k < CSI_SC_COUNT; k++) {
                s_baseline_profile[k] =
                    (1.0f - a) * s_baseline_profile[k] + a * profile[k];
            }
        }
        if (s_quiet_updates < 0xFFFFFFFFu) {
            s_quiet_updates++;
        }
    }

    /* --- publish -------------------------------------------------------- */
    portENTER_CRITICAL(&s_lock);
    s_features = f;
    s_score    = score;
    s_windows++;
    portEXIT_CRITICAL(&s_lock);

    if (s_motion) {
        /* Re-arm every window so the indicator stays lit for continuous
         * motion instead of flickering at the hop rate. */
        led_pulse((uint32_t)(MOTION_HOP_SEC * 1000.0f * 5.0f));
    }

    if (state_changed) {
        if (s_motion) {
            ESP_LOGW(TAG, ">>> MOTION  score=%.1f  (level=%.5f quiet=%.5f spread=%.5f)",
                     score, stat, s_quiet_level, s_quiet_dev);
        } else {
            ESP_LOGI(TAG, "    clear   after %.1f s of motion",
                     (now - prev_change_us) / 1e6f);
        }
    }

    if (!warmed && s_quiet_updates == MOTION_WARMUP_WINDOWS) {
        ESP_LOGI(TAG, "baseline learned: quiet=%.5f spread=%.5f - detection active",
                 s_quiet_level, s_quiet_dev);
    }

    return true;
}

/* ------------------------------------------------------------------------ */

void motion_get_state(motion_state_t *out)
{
    portENTER_CRITICAL(&s_lock);
    out->motion          = s_motion;
    out->score           = s_score;
    out->baseline_level  = s_quiet_level;
    out->baseline_spread = s_quiet_dev;
    out->warmed_up       = s_quiet_updates >= MOTION_WARMUP_WINDOWS;
    out->windows         = s_windows;
    out->events          = s_events;
    out->last_change_us  = s_last_change_us;
    out->f               = s_features;
    portEXIT_CRITICAL(&s_lock);
}

void motion_get_display(float *amp_out, float *base_out)
{
    portENTER_CRITICAL(&s_lock);
    memcpy(amp_out, s_last_norm, sizeof(s_last_norm));
    if (s_baseline_valid) {
        memcpy(base_out, s_baseline_profile, sizeof(s_baseline_profile));
    } else {
        for (int k = 0; k < CSI_SC_COUNT; k++) {
            base_out[k] = 1.0f;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

void motion_features_to_array(const motion_features_t *f, float *out)
{
    out[0] = f->mean_var;
    out[1] = f->max_var;
    out[2] = f->mean_absdiff;
    out[3] = f->baseline_dist;
    out[4] = f->amp_mean;
    out[5] = f->amp_var;
    out[6] = f->rssi_std;
    out[7] = f->zcr;
}
