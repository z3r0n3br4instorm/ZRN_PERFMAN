/*
 * neural_predict.c  -  Neural network data collection + placeholder inference
 *
 * Phase 1 (now): Collects feature vectors into SWITCH_DATA_PATH CSV for
 *   future training.  The CSV captures:
 *     - from_comm, to_comm (hashed to integer IDs)
 *     - switch_rate_1min
 *     - time_of_day (0..1439 minutes)
 *     - duration_on_prev (milliseconds)
 *
 * Phase 2 (--allow-experimental-features + trained weights):
 *   Loads a small feedforward NN from MODEL_WEIGHTS_PATH and predicts the
 *   most likely next window given the current context.  The predicted window
 *   gets a reduced tick rate (NN_PREWARM_TICK_MS) so it "wakes up" faster
 *   when the user switches to it.
 *
 * Network architecture:
 *   Input(NN_INPUT_DIM) -> Dense(NN_HIDDEN1, ReLU)
 *                       -> Dense(NN_HIDDEN2, ReLU)
 *                       -> Dense(NN_OUTPUT_DIM, Softmax)
 *
 * The weights file is a raw float32 binary blob laid out as:
 *   W1[NN_HIDDEN1 * NN_INPUT_DIM], b1[NN_HIDDEN1],
 *   W2[NN_HIDDEN2 * NN_HIDDEN1],  b2[NN_HIDDEN2],
 *   W3[NN_OUTPUT_DIM * NN_HIDDEN2], b3[NN_OUTPUT_DIM],
 *   app_name_table[NN_OUTPUT_DIM][64]  (null-terminated strings)
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"
#include <math.h>

/* NN weights (loaded once) */
static int    s_loaded = 0;
static float  s_W1[NN_HIDDEN1 * NN_INPUT_DIM];
static float  s_b1[NN_HIDDEN1];
static float  s_W2[NN_HIDDEN2 * NN_HIDDEN1];
static float  s_b2[NN_HIDDEN2];
static float  s_W3[NN_OUTPUT_DIM * NN_HIDDEN2];
static float  s_b3[NN_OUTPUT_DIM];
static char   s_app_names[NN_OUTPUT_DIM][64];
static int    s_n_apps = 0;

/* -------------------------------------------------------------------------- */
/* Simple hash to convert a comm name to a float feature                      */
/* -------------------------------------------------------------------------- */
static float hash_comm(const char *comm)
{
    unsigned long h = 5381;
    for (const char *p = comm; *p; p++)
        h = ((h << 5) + h) + (unsigned char)*p;
    /* Normalize to [0, 1] */
    return (float)(h % 10000) / 10000.0f;
}

/* -------------------------------------------------------------------------- */
/* Record feature vector (always, regardless of experimental mode)             */
/* -------------------------------------------------------------------------- */
void nn_record_features(const SwitchEvent *ev, int switch_rate_1min)
{
    /* Features are already saved to the CSV by switchlog.c.
     * Here we could add enriched features in a separate file,
     * but for now the CSV is sufficient for training. */
    (void)ev;
    (void)switch_rate_1min;
    /* Future: write a binary feature record for faster training ingestion */
}

/* -------------------------------------------------------------------------- */
/* Load model weights from disk                                                */
/* -------------------------------------------------------------------------- */
int nn_load_weights(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    size_t r = 0;
    r += fread(s_W1, sizeof(float), NN_HIDDEN1 * NN_INPUT_DIM, f);
    r += fread(s_b1, sizeof(float), NN_HIDDEN1, f);
    r += fread(s_W2, sizeof(float), NN_HIDDEN2 * NN_HIDDEN1, f);
    r += fread(s_b2, sizeof(float), NN_HIDDEN2, f);
    r += fread(s_W3, sizeof(float), NN_OUTPUT_DIM * NN_HIDDEN2, f);
    r += fread(s_b3, sizeof(float), NN_OUTPUT_DIM, f);

    /* Read app name table */
    if (fread(&s_n_apps, sizeof(int), 1, f) == 1) {
        if (s_n_apps > NN_OUTPUT_DIM) s_n_apps = NN_OUTPUT_DIM;
        for (int i = 0; i < s_n_apps; i++) {
            if (fread(s_app_names[i], 1, 64, f) != 64) {
                fclose(f);
                return -1;
            }
            s_app_names[i][63] = '\0';
        }
    }

    fclose(f);

    size_t expected = (size_t)(NN_HIDDEN1 * NN_INPUT_DIM + NN_HIDDEN1
                     + NN_HIDDEN2 * NN_HIDDEN1 + NN_HIDDEN2
                     + NN_OUTPUT_DIM * NN_HIDDEN2 + NN_OUTPUT_DIM);
    if (r != expected) return -1;

    s_loaded = 1;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Forward pass: predict next window comm name                                 */
/* Returns 1 if prediction is available, 0 otherwise                           */
/* -------------------------------------------------------------------------- */
int nn_predict_next(const char *current_comm, char *predicted_out, size_t n)
{
    if (!s_loaded || !g_experimental || s_n_apps == 0) return 0;

    /* Build input feature vector */
    float input[NN_INPUT_DIM];
    memset(input, 0, sizeof(input));

    /* Feature 0: hashed current comm */
    input[0] = hash_comm(current_comm);

    /* Feature 1: time of day normalised [0,1] */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    input[1] = (float)(tm->tm_hour * 60 + tm->tm_min) / 1440.0f;

    /* Feature 2: switch rate in last minute, normalised */
    int total = g_switch_count;
    time_t cutoff = now - 60;
    int rate = 0;
    for (int i = 0; i < total; i++) {
        int idx = (g_switch_head - 1 - i + SWITCH_HISTORY_MAX) % SWITCH_HISTORY_MAX;
        if (g_switch_history[idx].timestamp < cutoff) break;
        rate++;
    }
    input[2] = (float)rate / 60.0f;

    /* Features 3..15: recent switch history (hashed comms) */
    for (int i = 0; i < 13 && i < total; i++) {
        int idx = (g_switch_head - 1 - i + SWITCH_HISTORY_MAX) % SWITCH_HISTORY_MAX;
        input[3 + i] = hash_comm(g_switch_history[idx].to_comm);
    }

    /* Layer 1: h1 = ReLU(W1 * input + b1) */
    float h1[NN_HIDDEN1];
    for (int i = 0; i < NN_HIDDEN1; i++) {
        float sum = s_b1[i];
        for (int j = 0; j < NN_INPUT_DIM; j++)
            sum += s_W1[i * NN_INPUT_DIM + j] * input[j];
        h1[i] = sum > 0 ? sum : 0;  /* ReLU */
    }

    /* Layer 2: h2 = ReLU(W2 * h1 + b2) */
    float h2[NN_HIDDEN2];
    for (int i = 0; i < NN_HIDDEN2; i++) {
        float sum = s_b2[i];
        for (int j = 0; j < NN_HIDDEN1; j++)
            sum += s_W2[i * NN_HIDDEN1 + j] * h1[j];
        h2[i] = sum > 0 ? sum : 0;
    }

    /* Layer 3: out = Softmax(W3 * h2 + b3) */
    float out[NN_OUTPUT_DIM];
    float max_val = -1e30f;
    for (int i = 0; i < s_n_apps; i++) {
        float sum = s_b3[i];
        for (int j = 0; j < NN_HIDDEN2; j++)
            sum += s_W3[i * NN_HIDDEN2 + j] * h2[j];
        out[i] = sum;
        if (sum > max_val) max_val = sum;
    }

    /* Find argmax (skip softmax normalisation, we only need the top-1) */
    int best = 0;
    for (int i = 1; i < s_n_apps; i++) {
        if (out[i] > out[best]) best = i;
    }

    /* Don't predict the same app as current */
    if (strcmp(s_app_names[best], current_comm) == 0) {
        /* Find second-best */
        float second = -1e30f;
        int second_idx = -1;
        for (int i = 0; i < s_n_apps; i++) {
            if (i == best) continue;
            if (out[i] > second) {
                second = out[i];
                second_idx = i;
            }
        }
        if (second_idx < 0) return 0;
        best = second_idx;
    }

    strncpy(predicted_out, s_app_names[best], n - 1);
    predicted_out[n - 1] = '\0';
    return 1;
}
