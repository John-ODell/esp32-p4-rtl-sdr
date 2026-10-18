#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "rpi_lcd.h"
#include "rpi_text.h"
#include "rpi_canvas.h"
#include "pins.h"
#include "view.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#else
#define BIG_ALLOC(n) calloc(1, (n))
#endif


/* ------------------------------------------------------------------------- themes -- */

enum { HDR_STUDIO, HDR_PORTAPACK, HDR_PHOSPHOR, HDR_VINTAGE };
enum { SPEC_FILL, SPEC_BARS, SPEC_GLOW };
enum { GRID_DOTS, GRID_SCOPE, GRID_NONE };
enum { AXIS_PLAIN, AXIS_DIAL };

typedef struct { float pos; uint8_t r, g, b; } stop_t;

typedef struct {
    const char *name;
    uint8_t hdr, spec, grid, axis;
    int hdr_h, spec_h, axis_h;               /* waterfall gets the rest */
    int btn_y, btn_h;
    bool peak;                               /* draw peak hold */
    uint16_t bg, bg_pb;                      /* spectrum background, passband */
    uint16_t hdr_top, hdr_bot, hdr_line;     /* header gradient, bottom rule */
    uint16_t btn, btn_hi, btn_on, btn_edge;  /* button fill, sheen, held, outline */
    uint16_t txt, dim, freq;
    uint16_t grid_c, fill_t, fill_b, trace, trace2, peak_c, center, label, plate;
    uint16_t axis_bg, axis_bg2, axis_fg, axis_tick;
    float wf_db;                             /* waterfall span above the floor */
    stop_t wf[8];
} theme_t;

#define C RGB565
static const theme_t s_themes[] = {
    {
        .name = "Studio", .hdr = HDR_STUDIO, .spec = SPEC_FILL, .grid = GRID_DOTS, .axis = AXIS_PLAIN,
        .hdr_h = 40, .spec_h = 112, .axis_h = 16, .btn_y = 4, .btn_h = 32, .peak = true,
        .bg = C(7, 10, 17), .bg_pb = C(16, 24, 42),
        .hdr_top = C(30, 38, 60), .hdr_bot = C(12, 16, 26), .hdr_line = C(28, 92, 130),
        .btn = C(36, 46, 70), .btn_hi = C(52, 66, 98), .btn_on = C(40, 150, 210), .btn_edge = 0,
        .txt = C(222, 228, 238), .dim = C(110, 122, 148), .freq = C(255, 196, 60),
        .grid_c = C(30, 40, 60), .fill_t = C(34, 128, 180), .fill_b = C(6, 26, 46),
        .trace = C(205, 245, 255), .trace2 = C(90, 180, 230), .peak_c = C(84, 100, 140),
        .center = C(240, 64, 64), .label = C(92, 108, 138), .plate = C(14, 20, 34),
        .axis_bg = C(10, 14, 23), .axis_bg2 = C(10, 14, 23), .axis_fg = C(150, 162, 186), .axis_tick = C(110, 122, 148),
        .wf_db = 48,
        .wf = {{0, 0, 0, 12}, {.18f, 0, 8, 70}, {.36f, 0, 60, 170}, {.52f, 0, 170, 220},
               {.66f, 90, 230, 140}, {.78f, 250, 230, 40}, {.90f, 255, 110, 0}, {1, 255, 20, 30}},
    },
    {
        /* HackRF PortaPack (Mayhem) look: flat black, title bar, outlined buttons, 8x16 font */
        .name = "PortaPack", .hdr = HDR_PORTAPACK, .spec = SPEC_BARS, .grid = GRID_NONE, .axis = AXIS_PLAIN,
        .hdr_h = 54, .spec_h = 86, .axis_h = 16, .btn_y = 19, .btn_h = 32, .peak = false,
        .bg = C(0, 0, 0), .bg_pb = C(0, 0, 40),
        .hdr_top = C(0, 0, 0), .hdr_bot = C(0, 0, 0), .hdr_line = C(64, 64, 64),
        .btn = C(0, 0, 0), .btn_hi = C(0, 0, 0), .btn_on = C(255, 255, 255), .btn_edge = C(200, 200, 200),
        .txt = C(255, 255, 255), .dim = C(160, 160, 160), .freq = C(255, 255, 255),
        .grid_c = C(40, 40, 40), .fill_t = C(0, 120, 255), .fill_b = C(0, 120, 255),
        .trace = C(255, 255, 0), .trace2 = C(0, 90, 200), .peak_c = 0,
        .center = C(255, 0, 0), .label = C(128, 128, 128), .plate = C(0, 0, 0),
        .axis_bg = C(0, 0, 0), .axis_bg2 = C(0, 0, 0), .axis_fg = C(255, 255, 0), .axis_tick = C(160, 160, 160),
        .wf_db = 40,
        .wf = {{0, 0, 0, 0}, {.20f, 0, 0, 160}, {.40f, 0, 200, 255}, {.55f, 0, 255, 0},
               {.70f, 255, 255, 0}, {.85f, 255, 128, 0}, {.95f, 255, 0, 0}, {1, 255, 255, 255}},
    },
    {
        /* green CRT: oscilloscope graticule, glowing trace, phosphor waterfall */
        .name = "Phosphor", .hdr = HDR_PHOSPHOR, .spec = SPEC_GLOW, .grid = GRID_SCOPE, .axis = AXIS_PLAIN,
        .hdr_h = 40, .spec_h = 128, .axis_h = 16, .btn_y = 5, .btn_h = 30, .peak = true,
        .bg = C(0, 12, 5), .bg_pb = C(0, 24, 10),
        .hdr_top = C(0, 10, 4), .hdr_bot = C(0, 4, 2), .hdr_line = C(0, 110, 40),
        .btn = C(0, 10, 4), .btn_hi = C(0, 10, 4), .btn_on = C(60, 255, 120), .btn_edge = C(0, 150, 60),
        .txt = C(110, 255, 150), .dim = C(0, 140, 60), .freq = C(80, 255, 130),
        .grid_c = C(0, 60, 24), .fill_t = 0, .fill_b = 0,
        .trace = C(200, 255, 210), .trace2 = C(40, 220, 100), .peak_c = C(0, 90, 36),
        .center = C(255, 90, 60), .label = C(0, 130, 55), .plate = C(0, 20, 8),
        .axis_bg = C(0, 8, 3), .axis_bg2 = C(0, 8, 3), .axis_fg = C(80, 230, 120), .axis_tick = C(0, 130, 55),
        .wf_db = 44,
        .wf = {{0, 0, 0, 0}, {.25f, 0, 30, 10}, {.45f, 0, 80, 30}, {.62f, 0, 150, 55},
               {.76f, 40, 220, 90}, {.88f, 140, 255, 160}, {.96f, 220, 255, 225}, {1, 255, 255, 255}},
    },
    {
        /* valve radio: walnut, amber readout in a glass window, cream tuning dial */
        .name = "Vintage", .hdr = HDR_VINTAGE, .spec = SPEC_FILL, .grid = GRID_DOTS, .axis = AXIS_DIAL,
        .hdr_h = 40, .spec_h = 104, .axis_h = 24, .btn_y = 5, .btn_h = 30, .peak = false,
        .bg = C(16, 10, 5), .bg_pb = C(32, 20, 9),
        .hdr_top = C(82, 48, 24), .hdr_bot = C(40, 22, 10), .hdr_line = C(150, 110, 60),
        .btn = C(150, 112, 62), .btn_hi = C(196, 160, 100), .btn_on = C(255, 190, 80), .btn_edge = C(60, 36, 16),
        .txt = C(30, 18, 8), .dim = C(120, 90, 50), .freq = C(255, 170, 40),
        .grid_c = C(46, 30, 14), .fill_t = C(200, 120, 30), .fill_b = C(40, 20, 6),
        .trace = C(255, 220, 140), .trace2 = C(230, 150, 50), .peak_c = 0,
        .center = C(220, 30, 20), .label = C(130, 90, 45), .plate = C(30, 18, 8),
        .axis_bg = C(238, 226, 190), .axis_bg2 = C(206, 190, 148), .axis_fg = C(40, 24, 10), .axis_tick = C(40, 24, 10),
        .wf_db = 46,
        .wf = {{0, 0, 0, 0}, {.22f, 30, 4, 0}, {.42f, 110, 20, 0}, {.58f, 190, 60, 0},
               {.72f, 240, 120, 10}, {.84f, 255, 190, 60}, {.94f, 255, 235, 160}, {1, 255, 255, 240}},
    },
};
#undef C
#define NTHEMES ((int)(sizeof s_themes / sizeof s_themes[0]))
#define MAX_SPEC_H 128
#define MAX_HDR_H  54
#define MAX_AXIS_H 24
#define MAX_WF_H   (LCD_H - 40 - 86 - 16)    /* PortaPack has the tallest waterfall */

static const theme_t *T = &s_themes[0];
static int s_spec_y, s_axis_y, s_wf_y, s_wf_h;

/* header buttons: x and width; y/height come from the theme */
typedef struct { int x, w; const char *label; } button_t;
static const button_t s_btn[VIEW_NBTN] = {
    [VIEW_DN1M] = {4, 44, "-1M"},    [VIEW_DN100K] = {52, 54, "-100k"},
    [VIEW_UP100K] = {318, 54, "+100k"}, [VIEW_UP1M] = {376, 44, "+1M"},
    [VIEW_GAIN] = {424, 52, NULL},   [VIEW_THEME] = {110, 204, NULL},   /* the frequency readout */
};

static uint32_t s_rate, s_span, s_pb_hz = 200000;   /* sample rate, shown width, shaded passband */
static int s_hz_px, s_major, s_minor;                  /* Hz per column, axis steps */
static bool s_auto = true;                             /* dB range follows the noise floor */
static float s_ref = -20, s_range = 60;                /* manual: top of the scale, height in dB */
static uint16_t *s_hdr, *s_axis, *s_spec;
static uint16_t *s_wf;                       /* 2*wf_h rows, each line stored twice */
static int s_wf_head;
static float s_col[LCD_W], s_smooth[LCD_W], s_peak[LCD_W];
static void set_steps(void);
static uint16_t s_lut[256], s_fill[MAX_SPEC_H];
static float s_floor = -100;
static bool s_have;

static void build_tables(void)
{
    const stop_t *st = T->wf;
    for (int i = 0; i < 256; i++) {
        float t = i / 255.0f;
        int k = 0;
        while (k < 6 && t > st[k + 1].pos) k++;
        float u = (t - st[k].pos) / (st[k + 1].pos - st[k].pos);
        s_lut[i] = RGB565((int)(st[k].r + u * (st[k + 1].r - st[k].r)),
                          (int)(st[k].g + u * (st[k + 1].g - st[k].g)),
                          (int)(st[k].b + u * (st[k + 1].b - st[k].b)));
    }
    for (int y = 0; y < T->spec_h; y++) s_fill[y] = rpi_mix565(T->fill_t, T->fill_b, y * 256 / T->spec_h);
}

static void build_segments(void);
static uint16_t btn_text(bool on);

void view_init(uint32_t sample_rate)
{
    s_rate = s_span = sample_rate;
    set_steps();
    s_hdr = BIG_ALLOC(LCD_W * MAX_HDR_H * 2);
    s_axis = BIG_ALLOC(LCD_W * MAX_AXIS_H * 2);
    s_spec = BIG_ALLOC(LCD_W * MAX_SPEC_H * 2);
    s_wf = BIG_ALLOC(2 * MAX_WF_H * LCD_W * 2);
    if (!s_hdr || !s_axis || !s_spec || !s_wf) abort();
    build_segments();
    view_set_theme(0);
}

int view_themes(void) { return NTHEMES; }
const char *view_theme_name(int i) { return (i >= 0 && i < NTHEMES) ? s_themes[i].name : "?"; }
int view_theme(void) { return (int)(T - s_themes); }
int view_tune_y(void) { return s_spec_y; }

void view_colors(view_colors_t *c)
{
    c->bg = T->hdr == HDR_VINTAGE ? RGB565(30, 18, 8) : T->bg;
    c->btn = T->hdr == HDR_PHOSPHOR ? T->bg : T->btn;
    c->btn_on = T->btn_on;
    c->edge = T->hdr == HDR_STUDIO ? T->btn_hi : T->btn_edge;
    c->key_txt = T->txt;
    c->txt_on = btn_text(true);
    c->txt = T->hdr == HDR_VINTAGE ? T->trace : T->txt;     /* vintage keys are light, the panel dark */
    c->dim = T->hdr == HDR_VINTAGE ? RGB565(170, 130, 80) : T->dim;
    c->accent = T->freq;
    c->track = T->grid_c;
    c->square = T->hdr == HDR_PORTAPACK;
    c->top = T->hdr_h;
}
int view_hz_per_px(void) { return s_hz_px; }
uint32_t view_span(void) { return s_span; }

static void set_steps(void)
{
    s_hz_px = s_span / LCD_W;
    static const int major[] = {500000, 200000, 100000, 50000, 20000, 10000};
    int i = 0;
    while (i < 5 && s_span / major[i] < 4) i++;
    s_major = major[i];
    s_minor = s_major == 200000 || s_major == 20000 ? s_major / 4 : s_major / 5;
}

void view_set_span(uint32_t span)
{
    if (span > s_rate) span = s_rate;
    if (span < LCD_W) span = LCD_W;
    s_span = span;
    set_steps();
    s_have = false;
    s_wf_head = 0;
    memset(s_wf, 0, 2 * MAX_WF_H * LCD_W * 2);
}

void view_set_passband(uint32_t hz) { s_pb_hz = hz; }

void view_set_range(bool auto_range, float ref_db, float range_db)
{
    s_auto = auto_range;
    s_ref = ref_db;
    s_range = range_db < 10 ? 10 : range_db;
}

/* scale for the spectrum and the waterfall, dBFS */
static void scale(float *lo, float *hi, float *wlo, float *whi)
{
    if (s_auto) {
        *lo = s_floor - 8; *hi = s_floor + 52;                  /* 60 dB tall */
        *wlo = s_floor - 3; *whi = s_floor + T->wf_db - 3;
    } else {
        *hi = s_ref; *lo = s_ref - s_range;
        *wlo = *lo; *whi = *lo + s_range * 0.8f;               /* waterfall a little hotter */
    }
}
float view_floor(void) { return s_floor; }

void view_set_theme(int i)
{
    T = &s_themes[(i >= 0 && i < NTHEMES) ? i : 0];
    s_spec_y = T->hdr_h;
    s_axis_y = s_spec_y + T->spec_h;
    s_wf_y = s_axis_y + T->axis_h;
    s_wf_h = LCD_H - s_wf_y;
    s_wf_head = 0;
    memset(s_wf, 0, 2 * MAX_WF_H * LCD_W * 2);
    build_tables();
    rpi_lcd_fill_rect(0, 0, LCD_W, LCD_H, T->bg);
}

void view_reset(void)
{
    s_have = false;
    rpi_lcd_fill_rect(0, s_spec_y, LCD_W, T->spec_h, T->bg);
    rpi_lcd_fill_rect(0, s_wf_y, LCD_W, s_wf_h, 0);
}

/* ---------------------------------------------------------------- seven segments -- */

/* Slanted seven-segment digits, anti-aliased by 4x4 supersampling, with the unlit segments
 * drawn faintly the way a real LCD shows them. */
#define SEG_W     17
#define SEG_H     30
#define SEG_PITCH 20
#define SEG_DOT   7
#define SEG_T     3.4f                       /* stroke */
#define SEG_SLANT 0.10f

static const uint8_t s_seg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};  /* gfedcba */
static uint8_t (*s_seg_lit)[SEG_H][SEG_W], (*s_seg_ghost)[SEG_H][SEG_W];   /* [11], in PSRAM */

static bool in_seg(int k, float x, float y)
{
    const float w = SEG_W - 4, h = SEG_H, t = SEG_T, m = 2;       /* m: margin for the slant */
    float cx, cy, len;
    bool horiz = k == 0 || k == 3 || k == 6;
    switch (k) {
    case 0: cx = m + w / 2; cy = t / 2; len = w; break;                     /* a */
    case 1: cx = m + w - t / 2; cy = h / 4 + t / 4; len = h / 2; break;     /* b */
    case 2: cx = m + w - t / 2; cy = 3 * h / 4 - t / 4; len = h / 2; break; /* c */
    case 3: cx = m + w / 2; cy = h - t / 2; len = w; break;                 /* d */
    case 4: cx = m + t / 2; cy = 3 * h / 4 - t / 4; len = h / 2; break;     /* e */
    case 5: cx = m + t / 2; cy = h / 4 + t / 4; len = h / 2; break;         /* f */
    default: cx = m + w / 2; cy = h / 2; len = w; break;                    /* g */
    }
    float along = horiz ? fabsf(x - cx) : fabsf(y - cy), across = horiz ? fabsf(y - cy) : fabsf(x - cx);
    return across <= t / 2 && along <= len / 2 - 0.6f - across;
}

static void build_segments(void)
{
    s_seg_lit = BIG_ALLOC(11 * SEG_H * SEG_W);
    s_seg_ghost = BIG_ALLOC(11 * SEG_H * SEG_W);
    if (!s_seg_lit || !s_seg_ghost) abort();
    for (int d = 0; d < 11; d++) {
        uint8_t on = d < 10 ? s_seg[d] : 0;
        for (int py = 0; py < SEG_H; py++)
            for (int px = 0; px < SEG_W; px++) {
                int lit = 0, ghost = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        float y = py + (sy + 0.5f) / 4, x = px + (sx + 0.5f) / 4;
                        x -= (SEG_H / 2.0f - y) * SEG_SLANT;           /* lean right */
                        for (int k = 0; k < 7; k++)
                            if (in_seg(k, x, y)) { if (on >> k & 1) lit++; else ghost++; break; }
                    }
                s_seg_lit[d][py][px] = lit;
                s_seg_ghost[d][py][px] = ghost;
            }
    }
}

/* glow: also paint a faint halo around lit segments (CRT / valve looks) */
static void seg_digit(const rpi_canvas_t *c, int x0, int y0, char ch, uint16_t col, int ghost_amt, bool glow)
{
    int d = (ch >= '0' && ch <= '9') ? ch - '0' : 10;
    if (glow)
        for (int py = 0; py < SEG_H; py++)
            for (int px = 0; px < SEG_W; px++) {
                if (!s_seg_lit[d][py][px]) continue;
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++) {
                        int yy = y0 + py + dy, xx = x0 + px + dx;
                        if (yy < 0 || yy >= c->h || xx < 0 || xx >= c->w) continue;
                        uint16_t *p = &c->px[yy * c->w + xx];
                        *p = rpi_mix565(*p, col, 14);
                    }
            }
    for (int py = 0; py < SEG_H; py++) {
        uint16_t *p = c->px + (y0 + py) * c->w + x0;
        for (int px = 0; px < SEG_W; px++) {
            if (s_seg_ghost[d][py][px]) p[px] = rpi_mix565(p[px], col, s_seg_ghost[d][py][px] * ghost_amt / 16);
            if (s_seg_lit[d][py][px]) p[px] = rpi_mix565(p[px], col, s_seg_lit[d][py][px] * 16);
        }
    }
}

/* MHz.kHz with four MHz digits; returns the width drawn */
static int seg_freq(const rpi_canvas_t *c, int x, int y, uint32_t freq, int ghost_amt, bool glow)
{
    char f[8];
    snprintf(f, sizeof f, "%4" PRIu32 "%03" PRIu32, freq / 1000000 % 10000, (freq / 1000) % 1000);
    int x0 = x;
    for (int i = 0; i < 7; i++) {
        if (i == 4) { rpi_cv_rrect(c, x + 1, y + SEG_H - 4, 4, 4, 1, T->freq); x += SEG_DOT; }
        seg_digit(c, x, y, f[i], T->freq, ghost_amt, glow);
        x += SEG_PITCH;
    }
    return x - x0;
}
#define SEG_FREQ_W (7 * SEG_PITCH + SEG_DOT)

/* ------------------------------------------------------------------------- header -- */

static void outline(const rpi_canvas_t *c, int x, int y, int w, int h, int r, uint16_t edge, uint16_t fill)
{
    rpi_cv_rrect(c, x, y, w, h, r, edge);
    rpi_cv_rrect(c, x + 1, y + 1, w - 2, h - 2, r > 0 ? r - 1 : 0, fill);
}

static void button(const rpi_canvas_t *c, const button_t *b, bool on)
{
    int y = T->btn_y, h = T->btn_h;
    switch (T->hdr) {
    case HDR_PORTAPACK:                      /* square, 1 px outline, inverted while held */
        outline(c, b->x, y, b->w, h, 0, T->btn_edge, on ? T->btn_on : T->btn);
        break;
    case HDR_PHOSPHOR:                       /* rounded outline only */
        outline(c, b->x, y, b->w, h, 5, on ? T->btn_on : T->btn_edge, on ? T->btn_on : T->btn);
        break;
    case HDR_VINTAGE:                        /* brass key with a dark edge and a lit top */
        outline(c, b->x, y, b->w, h, 6, T->btn_edge, on ? T->btn_on : T->btn);
        if (!on) rpi_cv_fill(c, b->x + 4, y + 2, b->w - 8, 2, T->btn_hi);
        break;
    default:                                 /* studio: rounded, soft sheen */
        rpi_cv_rrect(c, b->x, y, b->w, h, 6, on ? T->btn_on : T->btn);
        if (!on) {
            rpi_cv_rrect(c, b->x + 1, y + 1, b->w - 2, 12, 5, T->btn_hi);
            rpi_cv_fill(c, b->x + 1, y + 9, b->w - 2, 6, T->btn);
        }
    }
}

static uint16_t btn_text(bool on)
{
    if (!on) return T->txt;
    return T->hdr == HDR_STUDIO ? T->txt : T->bg;   /* light keys get dark text */
}

void view_header(uint32_t freq, const char *gain, int pressed)
{
    rpi_canvas_t c = {s_hdr, LCD_W, T->hdr_h};
    for (int y = 0; y < T->hdr_h - 1; y++)
        rpi_cv_fill(&c, 0, y, LCD_W, 1, rpi_mix565(T->hdr_top, T->hdr_bot, y * 256 / T->hdr_h));
    if (T->hdr == HDR_PHOSPHOR)              /* scan lines */
        for (int y = 0; y < T->hdr_h - 1; y += 2) rpi_cv_fill(&c, 0, y, LCD_W, 1, rpi_mix565(T->hdr_top, T->dim, 20));
    rpi_cv_fill(&c, 0, T->hdr_h - 1, LCD_W, 1, T->hdr_line);

    if (T->hdr == HDR_PORTAPACK) {           /* title bar */
        rpi_cv_fill(&c, 0, 0, LCD_W, 16, RGB565(48, 48, 64));
        rpi_cv_text(&c, 4, 0, "<", T->txt, 1);
        rpi_cv_text(&c, 20, 0, "Spectrum  RTL-SDR V4", T->txt, 1);
        const char *r = "2.4 MS/s";
        rpi_cv_text(&c, LCD_W - 4 - rpi_cv_text_w(r, 1), 0, r, T->dim, 1);
    }

    for (int i = 0; i < VIEW_THEME; i++) {
        const button_t *b = &s_btn[i];
        bool on = i == pressed;
        button(&c, b, on);
        if (i == VIEW_GAIN) {                    /* opens Controls; shows the gain underneath */
            int ty = T->btn_y + (T->btn_h - 32) / 2;
            rpi_cv_text(&c, b->x + (b->w - rpi_cv_text_w("MENU", 1)) / 2, ty, "MENU", btn_text(on), 1);
            rpi_cv_text(&c, b->x + (b->w - rpi_cv_text_w(gain, 1)) / 2, ty + 15, gain,
                        on || T->hdr == HDR_VINTAGE ? btn_text(on) : T->dim, 1);
        } else {
            rpi_cv_text(&c, b->x + (b->w - rpi_cv_text_w(b->label, 1)) / 2, T->btn_y + (T->btn_h - 16) / 2,
                        b->label, btn_text(on), 1);
        }
    }

    int x0 = s_btn[VIEW_DN100K].x + s_btn[VIEW_DN100K].w, x1 = s_btn[VIEW_UP100K].x;
    int mhz_w = 4 + rpi_cv_text_w("MHz", 1);
    if (T->hdr == HDR_PORTAPACK) {           /* plain 16x32 digits, zero padded */
        char f[12];
        snprintf(f, sizeof f, "%4" PRIu32 ".%03" PRIu32, freq / 1000000 % 10000, (freq / 1000) % 1000);
        int w = rpi_cv_text_w(f, 2) + mhz_w, x = x0 + (x1 - x0 - w) / 2;
        x = rpi_cv_text(&c, x, T->btn_y, f, T->freq, 2);
        rpi_cv_text(&c, x + 4, T->btn_y + 14, "MHz", T->dim, 1);
    } else {
        int y = T->btn_y + (T->btn_h - SEG_H) / 2;
        int x = x0 + (x1 - x0 - SEG_FREQ_W - mhz_w) / 2;
        if (T->hdr == HDR_VINTAGE) {         /* dark glass window */
            outline(&c, x - 6, y - 3, SEG_FREQ_W + mhz_w + 12, SEG_H + 6, 4, T->btn_edge, RGB565(14, 8, 3));
            rpi_cv_fill(&c, x - 3, y - 1, SEG_FREQ_W + mhz_w + 6, 1, RGB565(46, 30, 16));
        }
        bool glow = T->hdr == HDR_PHOSPHOR || T->hdr == HDR_VINTAGE;
        x += seg_freq(&c, x, y, freq, T->hdr == HDR_PHOSPHOR ? 16 : 22, glow);
        rpi_cv_text(&c, x + 2, y + SEG_H - 15, "MHz", T->dim, 1);
    }
    rpi_lcd_blit(0, 0, LCD_W, T->hdr_h, s_hdr);
}

int view_button_at(int x, int y)
{
    if (y < T->btn_y - 4 || y >= T->hdr_h) return -1;
    for (int i = 0; i < VIEW_NBTN; i++)
        if (x >= s_btn[i].x - 2 && x < s_btn[i].x + s_btn[i].w + 2) return i;
    return -1;
}

/* --------------------------------------------------------------------------- axis -- */

/* x of the i-th multiple of step inside the span, or -1 past the right edge */
static int grid_x(int i, uint32_t fc, int step)
{
    int64_t left = (int64_t)fc - s_span / 2;
    int64_t first = ((left + step - 1) / step) * step;
    int64_t f = first + (int64_t)i * step;
    if (f >= (int64_t)fc + s_span / 2) return -1;
    return (int)((f - left) / s_hz_px);
}

void view_axis(uint32_t freq)
{
    const int h = T->axis_h;
    rpi_canvas_t c = {s_axis, LCD_W, h};
    for (int y = 0; y < h; y++) rpi_cv_fill(&c, 0, y, LCD_W, 1, rpi_mix565(T->axis_bg, T->axis_bg2, y * 256 / h));
    bool dial = T->axis == AXIS_DIAL;
    int64_t left = (int64_t)freq - s_span / 2;
    for (int i = 0;; i++) {
        int x = grid_x(i, freq, s_minor);
        if (x < 0) break;
        rpi_cv_fill(&c, x, 0, 1, dial ? 5 : 2, T->axis_tick);
    }
    for (int i = 0;; i++) {
        int x = grid_x(i, freq, s_major);
        if (x < 0) break;
        char l[12];
        int64_t hz = ((left + (int64_t)x * s_hz_px + s_major / 2) / s_major) * s_major;
        snprintf(l, sizeof l, s_major >= 100000 ? "%.1f" : s_major >= 10000 ? "%.2f" : "%.3f", hz / 1e6);
        int tw = rpi_cv_text_w(l, 1), tx = x - tw / 2;
        rpi_cv_fill(&c, x, 0, dial ? 2 : 1, dial ? 8 : 4, T->axis_tick);
        if (tx >= 0 && tx + tw <= LCD_W && abs(x - LCD_W / 2) > tw / 2 + 8)
            rpi_cv_text(&c, tx, h - 16, l, T->axis_fg, 1);
    }
    if (dial) {                              /* needle and a shadow line along the top */
        rpi_cv_fill(&c, 0, 0, LCD_W, 1, RGB565(120, 100, 70));
        rpi_cv_fill(&c, LCD_W / 2 - 1, 0, 3, h, T->center);
    } else {
        for (int k = 0; k < 4; k++) rpi_cv_fill(&c, LCD_W / 2 - 3 + k, k, 7 - 2 * k, 1, T->center);
    }
    rpi_lcd_blit(0, s_axis_y, LCD_W, h, s_axis);
}

/* ---------------------------------------------------------------------- spectrum -- */

static void update_floor(void)
{
    /* 20th percentile of the columns, 1 dB histogram from -160 to 0 dBFS */
    uint16_t h[160] = {0};
    for (int i = 0; i < LCD_W; i++) {
        int b = (int)(s_col[i] + 160);
        h[b < 0 ? 0 : b > 159 ? 159 : b]++;
    }
    int acc = 0, b = 0;
    for (; b < 160; b++) { acc += h[b]; if (acc >= LCD_W / 5) break; }
    float f = b - 160 + 0.5f;
    s_floor = s_have ? s_floor + 0.05f * (f - s_floor) : f;
}

static inline void px_mix(int x, int y, uint16_t col, int t)
{
    if (y < 0 || y >= T->spec_h) return;
    uint16_t *p = &s_spec[y * LCD_W + x];
    *p = rpi_mix565(*p, col, t);
}

static void render_spectrum(uint32_t fc)
{
    const int H = T->spec_h;
    float lo, hi, wlo, whi;
    scale(&lo, &hi, &wlo, &whi);
    const float px_db = (H - 1) / (hi - lo);
    int half = (int)(s_pb_hz / 2 / s_hz_px);
    if (half > LCD_W / 2 - 1) half = LCD_W / 2 - 1;
    const int pb0 = LCD_W / 2 - half, pb1 = LCD_W / 2 + half;
    rpi_canvas_t c = {s_spec, LCD_W, H};

    for (int y = 0; y < H; y++) {
        uint16_t *row = s_spec + y * LCD_W;
        for (int x = 0; x < LCD_W; x++) row[x] = (x >= pb0 && x <= pb1) ? T->bg_pb : T->bg;
    }
    int first10 = (int)ceilf(lo / 10.0f) * 10;
    if (T->grid == GRID_DOTS) {              /* 10 dB rows, 500 kHz columns, dotted */
        for (int d = first10; d <= hi; d += 10) {
            int y = (int)((hi - d) * px_db + 0.5f);
            if (y < 0 || y >= H) continue;
            for (int x = 0; x < LCD_W; x += 2) s_spec[y * LCD_W + x] = T->grid_c;
        }
        for (int i = 0;; i++) {
            int x = grid_x(i, fc, s_major);
            if (x < 0) break;
            for (int y = 0; y < H; y += 2) s_spec[y * LCD_W + x] = T->grid_c;
        }
    } else if (T->grid == GRID_SCOPE) {      /* 10 x 8 divisions, ticked centre cross */
        for (int k = 0; k <= 10; k++) {
            int x = k * (LCD_W - 1) / 10;
            for (int y = 0; y < H; y += 3) s_spec[y * LCD_W + x] = T->grid_c;
        }
        for (int k = 0; k <= 8; k++) {
            int y = k * (H - 1) / 8;
            for (int x = 0; x < LCD_W; x += 3) s_spec[y * LCD_W + x] = T->grid_c;
        }
        for (int x = 0; x < LCD_W; x += LCD_W / 50)
            for (int d = -2; d <= 2; d++) s_spec[(H / 2 + d) * LCD_W + x] = T->grid_c;
    }

    int ys[LCD_W];
    for (int x = 0; x < LCD_W; x++) {
        int y = (int)((hi - s_smooth[x]) * px_db);
        ys[x] = y < 0 ? 0 : y > H - 1 ? H - 1 : y;
    }
    if (T->spec == SPEC_FILL)
        for (int x = 0; x < LCD_W; x++)
            for (int yy = ys[x] + 1; yy < H; yy++) s_spec[yy * LCD_W + x] = s_fill[yy];
    else if (T->spec == SPEC_BARS)
        for (int x = 0; x < LCD_W; x++)
            for (int yy = ys[x] + 1; yy < H; yy++) s_spec[yy * LCD_W + x] = T->trace2;

    if (T->peak)
        for (int x = 0; x < LCD_W; x++) {
            int y = (int)((hi - s_peak[x]) * px_db);
            if (y >= 0 && y < H && y < ys[x] - 1) s_spec[y * LCD_W + x] = T->peak_c;
        }

    for (int x = 0; x < LCD_W; x++) {
        int a = x ? ys[x - 1] : ys[0], b = ys[x];
        if (a > b) { int t = a; a = b; b = t; }
        if (T->spec == SPEC_GLOW) {          /* phosphor halo, then the beam */
            for (int yy = a - 3; yy <= b + 3; yy++) {
                int d = yy < a ? a - yy : yy > b ? yy - b : 0;
                if (d) px_mix(x, yy, T->trace2, d == 1 ? 150 : d == 2 ? 70 : 30);
            }
            for (int yy = a; yy <= b; yy++) s_spec[yy * LCD_W + x] = T->trace;
        } else {
            for (int yy = a; yy <= b; yy++) s_spec[yy * LCD_W + x] = T->trace;
            if (T->spec == SPEC_FILL && b + 1 < H) s_spec[(b + 1) * LCD_W + x] = T->trace2;
        }
    }
    for (int y = 0; y < H; y++) s_spec[y * LCD_W + LCD_W / 2] = T->center;

    int lstep = px_db * 10 >= 18 ? 10 : 20;   /* labels need 16 px */
    for (int d = (int)ceilf(lo / lstep) * lstep; d <= hi; d += lstep) {
        int y = (int)((hi - d) * px_db + 0.5f);
        if (y - 16 < 0) continue;
        char l[8];
        snprintf(l, sizeof l, "%d", d);
        rpi_cv_text(&c, 3, y - 15, l, T->label, 1);
    }

    float pk = -200;
    for (int x = pb0; x <= pb1; x++) if (s_smooth[x] > pk) pk = s_smooth[x];
    char snr[16];
    snprintf(snr, sizeof snr, "S/N %2d dB", (int)lrintf(pk - s_floor));
    int tw = rpi_cv_text_w(snr, 1);
    uint16_t fg = T->hdr == HDR_VINTAGE ? T->trace : T->txt;
    if (T->hdr == HDR_PORTAPACK) outline(&c, LCD_W - tw - 12, 3, tw + 8, 18, 0, T->btn_edge, T->plate);
    else rpi_cv_rrect(&c, LCD_W - tw - 12, 4, tw + 8, 18, 4, T->plate);
    rpi_cv_text(&c, LCD_W - tw - 8, T->hdr == HDR_PORTAPACK ? 4 : 5, snr, fg, 1);
}

static void push_waterfall_line(void)
{
    float slo, shi, lo, hi;
    scale(&slo, &shi, &lo, &hi);
    s_wf_head = (s_wf_head + s_wf_h - 1) % s_wf_h;
    uint16_t *a = s_wf + (size_t)s_wf_head * LCD_W, *b = a + (size_t)s_wf_h * LCD_W;
    for (int x = 0; x < LCD_W; x++) {
        int v = (int)((s_col[x] - lo) / (hi - lo) * 255);
        a[x] = b[x] = s_lut[v < 0 ? 0 : v > 255 ? 255 : v];
    }
}

void view_render(const float *db, int n, uint32_t freq)
{
    int vis = (int)((int64_t)n * s_span / s_rate), first = n / 2 - vis / 2;   /* bins on screen */
    for (int x = 0; x < LCD_W; x++) {
        int b0 = first + x * vis / LCD_W, b1 = first + (x + 1) * vis / LCD_W;
        if (b1 <= b0) b1 = b0 + 1;
        float m = db[b0];
        for (int b = b0 + 1; b < b1; b++) if (db[b] > m) m = db[b];
        s_col[x] = m;
        s_smooth[x] = s_have ? s_smooth[x] + 0.5f * (m - s_smooth[x]) : m;
        s_peak[x] = s_have && s_peak[x] - 0.25f > s_smooth[x] ? s_peak[x] - 0.25f : s_smooth[x];
    }
    update_floor();
    s_have = true;
    render_spectrum(freq);
    push_waterfall_line();
}

void view_blit_spectrum(void) { rpi_lcd_blit(0, s_spec_y, LCD_W, T->spec_h, s_spec); }
void view_blit_waterfall(void) { rpi_lcd_blit(0, s_wf_y, LCD_W, s_wf_h, s_wf + (size_t)s_wf_head * LCD_W); }

void view_blit(void)
{
    view_blit_spectrum();
    view_blit_waterfall();
}

void view_message(const char *a, const char *b)
{
    uint16_t bg = T->bg, fg = T->hdr == HDR_VINTAGE ? T->trace : T->txt;
    rpi_lcd_fill_rect(0, s_spec_y, LCD_W, LCD_H - s_spec_y, bg);
    rpi_lcd_text(LCD_W / 2 - (int)strlen(a) * 4, 180, a, RGB565(255, 96, 72), bg, 1);
    if (b) rpi_lcd_text(LCD_W / 2 - (int)strlen(b) * 4, 200, b, fg, bg, 1);
}
