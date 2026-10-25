#include <math.h>
#include "agc.h"

#define AGC_HI     0.63f                     /* -4 dBFS: step down when the second's loudest frame passes this */
#define AGC_AIM    0.45f                     /* -7 dBFS: where a step aims, 3 dB under AGC_HI */
#define AGC_LO     0.22f                     /* -13 dBFS: step up when the loudest frame stays under this. On
                                              * the kit, RDS never locked with the peak at -22 dBFS, where the
                                              * 8-bit samples are too coarse, and did best around -6 */
#define AGC_CLIP   0.98f                     /* at the rails the real level is unknown: drop 10 dB */
#define AGC_MAX_UP 100                       /* tenths: the most one step up adds */

void agc_reset(agc_t *a, int64_t now_ms)
{
    a->t_change = now_ms;
    a->n = 0;
}

float agc_peak(const uint8_t *iq, size_t n)
{
    int lo = 255, hi = 0;
    for (size_t i = 0; i + 1 < n; i += 4) {  /* every other I/Q pair is plenty for a peak */
        int a = iq[i], b = iq[i + 1];
        if (a < lo) lo = a;
        if (a > hi) hi = a;
        if (b < lo) lo = b;
        if (b > hi) hi = b;
    }
    if (hi < lo) return 0;
    return fmaxf(hi - 127.4f, 127.4f - lo) / 128;
}

int agc_next(agc_t *a, float peak, int cur, const int *tenths, int n, int64_t now_ms)
{
    if (n < 1) return cur;
    if (cur < 0) cur = 0;
    if (cur >= n) cur = n - 1;
    if (now_ms - a->t_change < AGC_SETTLE_MS) return cur;
    int next = cur;
    if (peak >= AGC_CLIP) {                  /* at the rails the real level is unknown: no waiting */
        int want = tenths[cur] - 100;
        while (next > 0 && tenths[next] > want) next--;
    } else {
        a->pk[a->n++ % AGC_HOLD] = peak;
        if (a->n >= 2 * AGC_HOLD) a->n -= AGC_HOLD;   /* stays a count past AGC_HOLD, and the same ring slot */
        if (a->n < AGC_HOLD_DOWN) return cur;
        float recent = 0, held = 0;           /* loudest of the last AGC_HOLD_DOWN frames, and of all */
        for (int i = 0; i < AGC_HOLD && i < a->n; i++) {
            float p = a->pk[(a->n - 1 - i) % AGC_HOLD];
            if (i < AGC_HOLD_DOWN) recent = fmaxf(recent, p);
            held = fmaxf(held, p);
        }
        if (recent > AGC_HI) {
            int want = tenths[cur] - (int)ceilf(200 * log10f(recent / AGC_AIM));   /* at least 3 dB */
            while (next > 0 && tenths[next] > want) next--;
        } else if (a->n >= AGC_HOLD && held < AGC_LO) {
            /* Half the room: some of the R828D's steps do twice what their labels say, and going
             * too far up means coming back down, which is how an AGC hunts. */
            float room = held > 1e-3f ? 200 * log10f(AGC_AIM / held) : AGC_MAX_UP;
            int want = tenths[cur] + (int)fminf(room / 2, AGC_MAX_UP);
            while (next + 1 < n && tenths[next + 1] <= want) next++;
        }
    }
    if (next != cur) agc_reset(a, now_ms);
    return next;
}
