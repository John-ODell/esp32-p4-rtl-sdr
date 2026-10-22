/* RDS: 57 kHz down-mix, bit recovery, block sync, PI / PS / RadioText. */
#include <math.h>
#include <string.h>
#include "dsp.h"
#include "rds.h"

#define MPX_RATE 480000
#define RATE     24000                        /* after /20 */
#define BIT_RATE 1187.5f
#define HALF     10                           /* samples in half a bit (10.1) */
#define CAP      1024                         /* MPX samples per filter pass */

static fir_t s_lp;                            /* complex, +-2.6 kHz, 480k -> 24k */
static float s_nco[160][2];                   /* 57 kHz at 480 kS/s repeats every 160 samples */
static int s_nco_i;
static float s_zr, s_zi;                      /* mean of x^2: twice the carrier phase */
static float s_c = 1, s_s;                    /* carrier phase as cos, sin */
static float s_x[2 * HALF];                   /* last bit of carrier-aligned samples, circular */
static int s_xi;
static float s_ph, s_tgt = 0.5f;              /* bit phase (0..1) now, and where bits are read */
static float s_y1, s_y2;                      /* correlator one and two samples ago */
static bool s_pending;                        /* read the bit at the next sample */
static int s_since;                           /* samples since the last bit */
static int s_last;                            /* previous raw bit, for differential decoding */

/* blocks */
static const uint16_t OFS[5] = {0x0FC, 0x198, 0x168, 0x350, 0x1B4};   /* A, B, C, C', D */
static uint32_t s_reg;
static int s_nbits, s_next, s_found_at = -1000, s_found_blk;
static uint32_t s_bitno;
static uint16_t s_grp[4];
static bool s_ok[4];
static int s_win, s_win_bad;

/* decoded */
static rds_info_t s_info;
static uint16_t s_pi_cand;
static char s_ps_cand[8], s_ps[8], s_rt[64], s_rt_cand[64];
static uint8_t s_ps_mask;
static uint16_t s_rt_mask;
static int s_rt_ab = -1, s_rt_end = 64, s_rt_seg = 4;   /* letters per segment: 4 (2A) or 2 (2B) */

/* 26-bit word mod x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1: equals the block's offset word when
 * the block came through clean */
static uint16_t syndrome(uint32_t w)
{
    for (int i = 25; i >= 10; i--)
        if (w & (1u << i)) w ^= 0x5B9u << (i - 10);
    return w & 0x3FF;
}

static char letter(int c) { return c >= 0x20 && c < 0x7F ? (char)c : c == 0x0D ? '\r' : '?'; }

static void publish(void)
{
    s_info.has_ps = s_ps_mask == 0xF;
    if (s_info.has_ps) { memcpy(s_info.ps, s_ps, 8); s_info.ps[8] = 0; }
    int n = 0;                               /* RadioText: the run of segments from the start */
    while (n < 16 && (s_rt_mask >> n & 1)) n++;
    int len = n * s_rt_seg < s_rt_end ? n * s_rt_seg : s_rt_end;
    s_info.has_rt = len > 0;
    memcpy(s_info.rt, s_rt, len);
    while (len > 0 && s_info.rt[len - 1] == ' ') len--;
    s_info.rt[len] = 0;
}

/* two letters a, b at position pos of the station name, accepted on their second showing */
static void ps_pair(int seg, uint16_t d)
{
    char a = letter(d >> 8), b = letter(d & 0xFF);
    if (s_ps_cand[2 * seg] == a && s_ps_cand[2 * seg + 1] == b) {
        s_ps[2 * seg] = a;
        s_ps[2 * seg + 1] = b;
        s_ps_mask |= 1 << seg;
    }
    s_ps_cand[2 * seg] = a;
    s_ps_cand[2 * seg + 1] = b;
}

static void rt_letters(int at, const char *c, int n, int seg)
{
    bool same = true;
    for (int i = 0; i < n; i++) if (s_rt_cand[at + i] != c[i]) same = false;
    memcpy(s_rt_cand + at, c, n);
    if (!same) return;
    memcpy(s_rt + at, c, n);
    s_rt_mask |= 1 << seg;
    for (int i = 0; i < n; i++)
        if (c[i] == '\r' && at + i < s_rt_end) s_rt_end = at + i;
}

static void group(void)
{
    uint16_t pi = s_ok[0] ? s_grp[0] : 0;
    if (s_ok[0]) {
        if (pi == s_pi_cand) { s_info.pi = pi; s_info.has_pi = true; }
        s_pi_cand = pi;
    }
    if (!s_ok[1]) return;
    uint16_t b = s_grp[1];
    int type = b >> 12, ver_b = b >> 11 & 1;
    if (type == 0 && s_ok[3]) ps_pair(b & 3, s_grp[3]);
    if (type == 2) {
        int ab = b >> 4 & 1, seg = b & 0xF;
        if (ab != s_rt_ab) {                 /* new text */
            s_rt_ab = ab;
            memset(s_rt, ' ', sizeof s_rt);
            memset(s_rt_cand, 0, sizeof s_rt_cand);
            s_rt_mask = 0;
            s_rt_end = 64;
        }
        if (s_rt_seg != (ver_b ? 2 : 4)) {   /* 2A and 2B don't mix in one text */
            s_rt_seg = ver_b ? 2 : 4;
            memset(s_rt, ' ', sizeof s_rt);
            s_rt_mask = 0;
            s_rt_end = ver_b ? 32 : 64;
        }
        char c[4];
        if (!ver_b && s_ok[2] && s_ok[3]) {
            c[0] = letter(s_grp[2] >> 8); c[1] = letter(s_grp[2] & 0xFF);
            c[2] = letter(s_grp[3] >> 8); c[3] = letter(s_grp[3] & 0xFF);
            rt_letters(4 * seg, c, 4, seg);
        } else if (ver_b && s_ok[3]) {
            c[0] = letter(s_grp[3] >> 8); c[1] = letter(s_grp[3] & 0xFF);
            rt_letters(2 * seg, c, 2, seg);  /* 2B: 32 letters, two per segment */
        }
    }
    publish();
}

/* one data bit (after differential decoding), MSB first */
static void bit(int d)
{
    s_reg = (s_reg << 1 | (unsigned)d) & 0x3FFFFFF;
    s_bitno++;
    if (!s_info.sync) {
        uint16_t s = syndrome(s_reg);
        for (int j = 0; j < 5; j++) {
            if (s != OFS[j]) continue;
            int blk = j == 4 ? 3 : j == 3 ? 2 : j;
            if ((int)(s_bitno - s_found_at) == 26 && blk == (s_found_blk + 1) % 4) {
                s_info.sync = true;          /* two blocks in a row, in order */
                s_next = (blk + 1) % 4;
                s_nbits = 0;
                s_win = s_win_bad = 0;
                memset(s_ok, 0, sizeof s_ok);
            }
            s_found_at = (int)s_bitno;
            s_found_blk = blk;
            break;
        }
        return;
    }
    if (++s_nbits < 26) return;
    s_nbits = 0;
    uint16_t s = syndrome(s_reg);
    int k = s_next;
    bool ok = s == OFS[k == 3 ? 4 : k] || (k == 2 && s == OFS[3]);   /* D's offset is last, after C' */
    s_grp[k] = s_reg >> 10;
    s_ok[k] = ok;
    s_info.blocks++;
    if (!ok) s_info.bad_blocks++;
    if (k == 3) {
        group();
        memset(s_ok, 0, sizeof s_ok);
    }
    s_next = (k + 1) % 4;
    s_win++;
    if (!ok) s_win_bad++;
    if (s_win == 50) {                       /* most of 50 blocks bad: look for sync again */
        if (s_win_bad > 35) s_info.sync = false;
        s_win = s_win_bad = 0;
    }
}

bool rds_init(void)
{
    for (int i = 0; i < 160; i++) {
        s_nco[i][0] = cosf(2 * PI_F * 57000.0f * i / MPX_RATE);
        s_nco[i][1] = -sinf(2 * PI_F * 57000.0f * i / MPX_RATE);
    }
    if (!fir_alloc(&s_lp, 129, MPX_RATE / RATE, 2, CAP)) return false;
    rds_reset();
    return true;
}

void rds_reset(void)
{
    fir_design(&s_lp, 129, MPX_RATE, 2600, 21400, 55);
    s_zr = s_zi = 0;
    s_c = 1;
    s_s = 0;
    memset(s_x, 0, sizeof s_x);
    s_ph = 0;
    s_tgt = 0.5f;
    s_y1 = s_y2 = 0;
    s_pending = false;
    s_since = 0;
    s_last = 0;
    s_reg = 0;
    s_bitno = 0;
    s_found_at = -1000;
    memset(&s_info, 0, sizeof s_info);
    s_pi_cand = 0;
    memset(s_ps, ' ', sizeof s_ps);
    memset(s_ps_cand, 0, sizeof s_ps_cand);
    s_ps_mask = 0;
    memset(s_rt, ' ', sizeof s_rt);
    memset(s_rt_cand, 0, sizeof s_rt_cand);
    s_rt_mask = 0;
    s_rt_ab = -1;
    s_rt_end = 64;
    s_rt_seg = 4;
}

/* One 24 kS/s complex sample: carrier phase, biphase correlation, bit timing. A bit is read one
 * sample after the read point, so the samples either side of it are known: if the later one
 * correlates better, the read point moves later (early-late gate), a fraction of a sample a bit. */
static void sample(float re, float im)
{
    s_zr += (re * re - im * im - s_zr) * (1.0f / 1200);    /* x^2 drops the BPSK sign; ~50 ms */
    s_zi += (2 * re * im - s_zi) * (1.0f / 1200);
    float mag = sqrtf(s_zr * s_zr + s_zi * s_zi) + 1e-20f, c2 = s_zr / mag;   /* cos and sin of half its angle */
    float c = sqrtf(fmaxf(0, (1 + c2) / 2)), s = copysignf(sqrtf(fmaxf(0, (1 - c2) / 2)), s_zi);
    if (c * s_c + s * s_s < 0) { c = -c; s = -s; }          /* half angle: stay on the same side */
    s_c = c;
    s_s = s;
    float x = re * c + im * s;
    s_x[s_xi] = x;
    s_xi = (s_xi + 1) % (2 * HALF);
    float y = 0;                             /* older half minus newer half: a biphase bit */
    for (int i = 0; i < HALF; i++) y += s_x[(s_xi + i) % (2 * HALF)] - s_x[(s_xi + HALF + i) % (2 * HALF)];

    if (s_pending) {
        s_pending = false;
        float early = fabsf(s_y2), late = fabsf(y);
        s_tgt += 0.15f * (late - early) / (late + early + 1e-12f) * (BIT_RATE / RATE);
        if (s_tgt >= 1) s_tgt -= 1;
        if (s_tgt < 0) s_tgt += 1;
        int raw = s_y1 > 0;
        bit(raw ^ s_last);
        s_last = raw;
    }
    float prev = s_ph;
    s_ph += BIT_RATE / RATE;
    if (s_ph >= 1) s_ph -= 1;
    s_pending = prev <= s_ph ? prev < s_tgt && s_tgt <= s_ph : prev < s_tgt || s_tgt <= s_ph;
    if (++s_since < 3 * HALF / 2) s_pending = false;      /* a read point that just moved later */
    if (s_pending) s_since = 0;
    s_y2 = s_y1;
    s_y1 = y;
}

void rds_process(const float *mpx, int n)
{
    float out[2 * (CAP / 20 + 2)];
    while (n > 0) {
        int m = n < CAP ? n : CAP;
        float *in = fir_in(&s_lp);
        for (int i = 0; i < m; i++) {
            in[2 * i] = mpx[i] * s_nco[s_nco_i][0];
            in[2 * i + 1] = mpx[i] * s_nco[s_nco_i][1];
            if (++s_nco_i == 160) s_nco_i = 0;
        }
        int k = fir_run(&s_lp, m, out);
        for (int i = 0; i < k; i++) sample(out[2 * i], out[2 * i + 1]);
        mpx += m;
        n -= m;
    }
}

void rds_get(rds_info_t *out) { *out = s_info; }
