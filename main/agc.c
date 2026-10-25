#include <math.h>
#include "agc.h"

#define AGC_HI     0.30f                     /* -10.5 dBFS: step down above this */
#define AGC_AIM    0.22f                     /* -13 dBFS: where a step lands */
#define AGC_LO     0.11f                     /* -19 dBFS: step up after AGC_RISE_MS below this */
#define AGC_CLIP   0.98f                     /* at the rails the real level is unknown: drop 10 dB */
#define AGC_MAX_UP 100                       /* tenths: the most one step up adds */

void agc_reset(agc_t *a, int64_t now_ms)
{
    a->t_change = now_ms;
    a->t_low = -1;
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
    if (peak > AGC_HI) {
        a->t_low = -1;
        int drop = peak >= AGC_CLIP ? 100 : (int)ceilf(200 * log10f(peak / AGC_AIM));
        int want = tenths[cur] - drop;       /* drop is at least 2.7 dB above AGC_HI */
        while (next > 0 && tenths[next] > want) next--;
    } else if (peak < AGC_LO) {
        if (a->t_low < 0) a->t_low = now_ms;
        if (now_ms - a->t_low < AGC_RISE_MS) return cur;
        float room = peak > 1e-3f ? 200 * log10f(AGC_AIM / peak) : AGC_MAX_UP;
        int want = tenths[cur] + (int)fminf(room, AGC_MAX_UP);
        while (next + 1 < n && tenths[next + 1] <= want) next++;
    } else {
        a->t_low = -1;
    }
    if (next != cur) agc_reset(a, now_ms);
    return next;
}
