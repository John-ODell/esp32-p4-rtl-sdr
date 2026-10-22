#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rpi_lcd.h"
#include "rpi_text.h"
#include "rpi_canvas.h"
#include "pins.h"
#include "view.h"
#include "panel.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#else
#define BIG_ALLOC(n) calloc(1, (n))
#endif

/* ------------------------------------------------------------------------- tables -- */

static const uint32_t s_zoom_hz[PANEL_ZOOMS] = {2400000, 1200000, 600000, 300000};
static const char *s_zoom_lbl[PANEL_ZOOMS] = {"2.4 MHz", "1.2 MHz", "600 kHz", "300 kHz"};
static const char *s_zoom_sub[PANEL_ZOOMS] = {"whole span", "2x", "4x", "8x"};
static const uint32_t s_bw_hz[PANEL_BWS] = {200000, 25000, 12500, 8000};
static const char *s_bw_lbl[PANEL_BWS] = {"200k", "25k", "12.5k", "8k"};
static const char *s_bw_sub[PANEL_BWS] = {"broadcast FM", "wide NFM", "narrow FM", "AM airband"};

typedef struct { const char *name, *sub; uint32_t freq; int bw, zoom; } band_t;
static const band_t s_band[PANEL_BANDS] = {
    {"FM", "98.0", 98000000, 0, 0},        {"AIR", "121.5", 121500000, 3, 0},
    {"MARINE", "156.8", 156800000, 1, 1},  {"NOAA", "162.475", 162475000, 1, 2},
    {"2M", "145.0", 145000000, 2, 0},      {"PMR", "446.1", 446100000, 2, 2},
    {"70CM", "433.92", 433920000, 1, 0},   {"ADS-B", "1090", 1090000000, 0, 0},
};

uint32_t panel_zoom_hz(int i) { return s_zoom_hz[i >= 0 && i < PANEL_ZOOMS ? i : 0]; }
uint32_t panel_bw_hz(int i) { return s_bw_hz[i >= 0 && i < PANEL_BWS ? i : 0]; }

const char *panel_band(int i, uint32_t *freq, int *bw, int *zoom)
{
    const band_t *b = &s_band[i >= 0 && i < PANEL_BANDS ? i : 0];
    *freq = b->freq; *bw = b->bw; *zoom = b->zoom;
    return b->name;
}

/* --------------------------------------------------------------------------- keys -- */

enum { P_MENU, P_GAIN, P_RANGE, P_ZOOM, P_BW, P_BAND, P_SCAN, P_LOOK };
static const char *s_title[] = {"CONTROLS", "GAIN", "RANGE", "ZOOM", "FILTER", "BAND", "SCAN", "LOOK"};
#define LOOKS 5
static const char *s_look_sub[LOOKS] = {"clean blue", "Mayhem style", "green CRT", "walnut and amber", "synthwave"};

enum {
    K_LABEL = -1,                            /* text only, not touchable */
    K_BACK, K_DONE, K_WIFI, K_BLE, K_TURN, K_TILE,   /* K_TILE + page: menu tiles */
    K_GAIN_DN = K_TILE + 8, K_GAIN_UP, K_GAIN_P,   /* K_GAIN_P + 0..3: presets */
    K_TOP_DN = K_GAIN_P + 4, K_TOP_UP, K_SPAN_DN, K_SPAN_UP, K_AUTO,
    K_ZOOM, K_BW = K_ZOOM + PANEL_ZOOMS, K_BAND = K_BW + PANEL_BWS, K_LOOK = K_BAND + PANEL_BANDS,
};

typedef struct {
    int x, y, w, h, id;
    char l1[16], l2[32];                     /* l1 large, l2 small underneath */
    bool sel, rep, big;                      /* selected, repeats while held, 2x title */
} pkey_t;

#define MAXK 16
#define GAP 6
static pkey_t s_k[MAXK];
static int s_nk, s_page, s_hot = -1;          /* s_hot: index of the key under the finger */
static int64_t s_next_rep;
static uint16_t *s_px;
static int s_h;                               /* panel height */
static view_colors_t K;

void panel_init(void)
{
    s_px = BIG_ALLOC(LCD_W * LCD_H * 2);
    if (!s_px) abort();
}

static pkey_t *add(int x, int y, int w, int h, int id, const char *l1, const char *l2)
{
    pkey_t *k = &s_k[s_nk++];
    memset(k, 0, sizeof *k);
    k->x = x; k->y = y; k->w = w; k->h = h; k->id = id;
    snprintf(k->l1, sizeof k->l1, "%s", l1 ? l1 : "");
    snprintf(k->l2, sizeof k->l2, "%s", l2 ? l2 : "");
    return k;
}

/* cols x rows cells over a rectangle, GAP apart; returns the first key */
static pkey_t *grid(int x, int y, int w, int h, int cols, int rows, int i, int id, const char *l1, const char *l2)
{
    int cw = (w - (cols - 1) * GAP) / cols, ch = (h - (rows - 1) * GAP) / rows;
    return add(x + (i % cols) * (cw + GAP), y + (i / cols) * (ch + GAP), cw, ch, id, l1, l2);
}

static void gain_text(const ctl_t *c, char *b, size_t n)
{
    snprintf(b, n, "%s", c->gain_idx ? c->gain_label(c->gain_idx) : "AGC");
}

/* three keys on a row: [-] value [+] */
static void stepper(int y, int h, int dn, int up, const char *value, const char *caption)
{
    const int kw = LCD_PORTRAIT ? 80 : 120;
    add(GAP, y, kw, h, dn, "-", NULL)->rep = true;
    pkey_t *v = add(GAP + kw + GAP, y, LCD_W - 4 * GAP - 2 * kw, h, K_LABEL, value, caption);
    v->big = true;
    add(LCD_W - GAP - kw, y, kw, h, up, "+", NULL)->rep = true;
}

static void build(const ctl_t *c)
{
    s_nk = 0;
    char v[24];
    /* portrait turns the wide grids on their side */
    const bool p = LCD_PORTRAIT;
    const int c4 = p ? 2 : 4, r2 = p ? 4 : 2;
    if (s_page == P_MENU) {
        int y = GAP, h = s_h - 2 * GAP, w = LCD_W - 2 * GAP;
        gain_text(c, v, sizeof v);
        grid(GAP, y, w, h, c4, r2, 0, K_TILE + P_GAIN, "GAIN", v)->big = true;
        if (c->range_auto) snprintf(v, sizeof v, "auto");
        else snprintf(v, sizeof v, "%d/%d dB", c->ref_db, c->range_db);
        grid(GAP, y, w, h, c4, r2, 1, K_TILE + P_RANGE, "RANGE", v)->big = true;
        grid(GAP, y, w, h, c4, r2, 2, K_TILE + P_ZOOM, "ZOOM", s_zoom_lbl[c->zoom])->big = true;
        grid(GAP, y, w, h, c4, r2, 3, K_TILE + P_BW, "FILTER", s_bw_lbl[c->bw])->big = true;
        grid(GAP, y, w, h, c4, r2, 4, K_TILE + P_BAND, "BAND", c->band >= 0 ? s_band[c->band].name : "presets")->big = true;
        grid(GAP, y, w, h, c4, r2, 5, K_TILE + P_SCAN, "SCAN", "Wi-Fi, BLE")->big = true;
        grid(GAP, y, w, h, c4, r2, 6, K_TILE + P_LOOK, "LOOK", view_theme_name(view_theme()))->big = true;
        grid(GAP, y, w, h, c4, r2, 7, K_DONE, "DONE", "to radio")->big = true;
        return;
    }
    /* sub page: BACK + title, then the body */
    const int bar = 44, by = GAP + bar + GAP, bh = s_h - by - GAP, bw = LCD_W - 2 * GAP;
    add(GAP, GAP, 120, bar, K_BACK, "< BACK", NULL)->big = true;
    add(GAP + 120 + GAP, GAP, LCD_W - 3 * GAP - 120, bar, K_LABEL, s_title[s_page], NULL)->big = true;
    switch (s_page) {
    case P_GAIN: {
        int h1 = (bh - GAP) * 55 / 100;
        gain_text(c, v, sizeof v);
        stepper(by, h1, K_GAIN_DN, K_GAIN_UP, v, c->gain_idx ? "tuner gain" : "tuner AGC");
        static const char *pl[4] = {"AGC", "15 dB", "30 dB", "MAX"};
        int t = c->gain_tenths(c->gain_idx);
        for (int i = 0; i < 4; i++) {
            pkey_t *k = grid(GAP, by + h1 + GAP, bw, bh - h1 - GAP, p ? 2 : 4, p ? 2 : 1, i, K_GAIN_P + i, pl[i], NULL);
            k->sel = (i == 0 && c->gain_idx == 0) || (i == 3 && c->gain_idx == c->gain_n - 1 && c->gain_n > 1) ||
                     (i == 1 && t == 150) || (i == 2 && t == 300);
        }
        break;
    }
    case P_RANGE: {
        int h = (bh - 2 * GAP) / 3;
        snprintf(v, sizeof v, "%d dBFS", c->ref_db);
        stepper(by, h, K_TOP_DN, K_TOP_UP, v, c->range_auto ? "top (auto now)" : "top of scale");
        snprintf(v, sizeof v, "%d dB", c->range_db);
        stepper(by + h + GAP, h, K_SPAN_DN, K_SPAN_UP, v, c->range_auto ? "span (auto now)" : "scale height");
        add(GAP, by + 2 * (h + GAP), bw, h, K_AUTO, "AUTO", c->range_auto ? "follows the noise floor" : "off: tap to follow the floor")
            ->sel = c->range_auto;
        break;
    }
    case P_ZOOM:
        for (int i = 0; i < PANEL_ZOOMS; i++)
            grid(GAP, by, bw, bh, 2, 2, i, K_ZOOM + i, s_zoom_lbl[i], s_zoom_sub[i])->sel = i == c->zoom;
        break;
    case P_BW:
        for (int i = 0; i < PANEL_BWS; i++)
            grid(GAP, by, bw, bh, 2, 2, i, K_BW + i, s_bw_lbl[i], s_bw_sub[i])->sel = i == c->bw;
        break;
    case P_BAND:
        for (int i = 0; i < PANEL_BANDS; i++)
            grid(GAP, by, bw, bh, c4, r2, i, K_BAND + i, s_band[i].name, s_band[i].sub)->sel = i == c->band;
        break;
    case P_SCAN:                             /* on the module's ESP32-C6 */
        grid(GAP, by, bw, bh, p ? 1 : 2, p ? 2 : 1, 0, K_WIFI, "WIFI", "access points, channels")->big = true;
        grid(GAP, by, bw, bh, p ? 1 : 2, p ? 2 : 1, 1, K_BLE, "BLE", "nearby devices")->big = true;
        break;
    case P_LOOK:
        for (int i = 0; i < LOOKS && i < view_themes(); i++) {
            char up[16];
            snprintf(up, sizeof up, "%s", view_theme_name(i));
            for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
            pkey_t *k = grid(GAP, by, bw, bh, 2, 3, i, K_LOOK + i, up, s_look_sub[i]);
            k->big = true;
            k->sel = i == view_theme();
        }
        grid(GAP, by, bw, bh, 2, 3, 5, K_TURN, "TURN", p ? "back to landscape" : "portrait, USB up")->big = true;
        break;
    }
}

/* ------------------------------------------------------------------------- drawing -- */

static void draw_key(const rpi_canvas_t *c, const pkey_t *k, bool hot)
{
    int r = K.square ? 0 : 8;
    bool label = k->id == K_LABEL;
    uint16_t fill = k->sel ? K.btn_on : hot ? rpi_mix565(K.btn, K.btn_on, 110) : K.btn;
    uint16_t fg = k->sel ? K.txt_on : K.key_txt, sub = rpi_mix565(fill, fg, 170);
    if (label) { fill = K.bg; fg = K.accent; sub = K.dim; }
    else {
        rpi_cv_rrect(c, k->x, k->y, k->w, k->h, r, hot ? K.btn_on : K.edge);
        rpi_cv_rrect(c, k->x + (hot ? 2 : 1), k->y + (hot ? 2 : 1), k->w - (hot ? 4 : 2), k->h - (hot ? 4 : 2),
                     r > 2 ? r - 2 : 0, fill);
    }
    int sc = (k->big || k->h >= 50) && rpi_cv_text_w(k->l1, 2) <= k->w - 12 ? 2 : 1;
    int th = 16 * sc + (k->l2[0] ? 18 : 0);
    int ty = k->y + (k->h - th) / 2;
    rpi_cv_text(c, k->x + (k->w - rpi_cv_text_w(k->l1, sc)) / 2, ty, k->l1, fg, sc);
    if (k->l2[0]) {
        int w = rpi_cv_text_w(k->l2, 1);
        rpi_cv_text(c, k->x + (k->w - w) / 2, ty + 16 * sc + 2, k->l2, sub, 1);
    }
}

static void render(void)
{
    rpi_canvas_t c = {s_px, LCD_W, s_h};
    rpi_cv_fill(&c, 0, 0, LCD_W, s_h, K.bg);
    for (int i = 0; i < s_nk; i++) draw_key(&c, &s_k[i], i == s_hot);
}

static void blit_rows(int y0, int h)
{
    if (y0 < 0) { h += y0; y0 = 0; }
    if (y0 + h > s_h) h = s_h - y0;
    if (h > 0) rpi_lcd_blit(0, K.top + y0, LCD_W, h, s_px + y0 * LCD_W);
}

static void show_all(const ctl_t *c)
{
    view_colors(&K);
    s_h = LCD_H - K.top;
    build(c);
    render();
    blit_rows(0, s_h);
}

/* redraw and push only the rows of keys a and b (indexes, or -1) */
static void show_keys(int a, int b)
{
    render();
    int y0 = s_h, y1 = 0;
    for (int i = 0; i < 2; i++) {
        int k = i ? b : a;
        if (k < 0 || k >= s_nk) continue;
        if (s_k[k].y < y0) y0 = s_k[k].y;
        if (s_k[k].y + s_k[k].h > y1) y1 = s_k[k].y + s_k[k].h;
    }
    if (y1 > y0) blit_rows(y0, y1 - y0);
}

void panel_open(const ctl_t *c)
{
    s_page = P_MENU;
    s_hot = -1;
    show_all(c);
}

void panel_redraw(const ctl_t *c) { show_all(c); }

/* -------------------------------------------------------------------------- touch -- */

static int hit(int x, int y)
{
    y -= K.top;
    for (int i = 0; i < s_nk; i++) {
        const pkey_t *k = &s_k[i];
        if (k->id == K_LABEL) continue;
        if (x >= k->x - GAP / 2 - 1 && x < k->x + k->w + GAP / 2 + 1 &&
            y >= k->y - GAP / 2 - 1 && y < k->y + k->h + GAP / 2 + 1) return i;
    }
    return -1;
}

static int nearest_gain(const ctl_t *c, int tenths)
{
    int best = 1;
    for (int i = 1; i < c->gain_n; i++)
        if (abs(c->gain_tenths(i) - tenths) < abs(c->gain_tenths(best) - tenths)) best = i;
    return best;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* run key id; returns CTL_* bits. *page_change is set when the page must be rebuilt. */
static int act(ctl_t *c, int id, bool *page_change)
{
    if (id == K_BACK) { s_page = P_MENU; *page_change = true; return 0; }
    if (id == K_DONE) return CTL_CLOSE;
    if (id == K_WIFI) return CTL_WIFI;
    if (id == K_BLE) return CTL_BLE;
    if (id == K_TURN) return CTL_TURN;
    if (id >= K_TILE && id < K_TILE + 8) { s_page = id - K_TILE; *page_change = true; return 0; }
    if (c->gain_n > 1 && (id == K_GAIN_DN || id == K_GAIN_UP)) {
        int g = c->gain_idx;
        if (g == 0) g = nearest_gain(c, 300);            /* leave AGC from the middle */
        else g = clampi(g + (id == K_GAIN_UP ? 1 : -1), 1, c->gain_n - 1);
        if (g == c->gain_idx) return 0;
        c->gain_idx = g;
        return CTL_GAIN;
    }
    if (id >= K_GAIN_P && id < K_GAIN_P + 4 && c->gain_n > 1) {
        int i = id - K_GAIN_P;
        c->gain_idx = i == 0 ? 0 : i == 3 ? c->gain_n - 1 : nearest_gain(c, i == 1 ? 150 : 300);
        *page_change = true;
        return CTL_GAIN;
    }
    if (id >= K_TOP_DN && id <= K_SPAN_UP) {
        if (id == K_TOP_DN) c->ref_db = clampi(c->ref_db - 5, -120, 0);
        if (id == K_TOP_UP) c->ref_db = clampi(c->ref_db + 5, -120, 0);
        if (id == K_SPAN_DN) c->range_db = clampi(c->range_db - 5, 20, 100);
        if (id == K_SPAN_UP) c->range_db = clampi(c->range_db + 5, 20, 100);
        if (c->range_auto) { c->range_auto = false; *page_change = true; }
        return CTL_RANGE;
    }
    if (id == K_AUTO) { c->range_auto = !c->range_auto; *page_change = true; return CTL_RANGE; }
    if (id >= K_ZOOM && id < K_ZOOM + PANEL_ZOOMS) {
        c->zoom = id - K_ZOOM; *page_change = true; return CTL_ZOOM;
    }
    if (id >= K_BW && id < K_BW + PANEL_BWS) {
        c->bw = id - K_BW; *page_change = true; return CTL_BW;
    }
    if (id >= K_LOOK && id < K_LOOK + LOOKS) {
        c->look = id - K_LOOK;
        *page_change = true;
        return CTL_LOOK;
    }
    if (id >= K_BAND && id < K_BAND + PANEL_BANDS) {
        uint32_t f;
        c->band = id - K_BAND;
        panel_band(c->band, &f, &c->bw, &c->zoom);
        *page_change = true;
        return CTL_BAND;
    }
    return 0;
}

int panel_touch(ctl_t *c, int ev, int x, int y, int64_t now_us)
{
    int k = hit(x, y), old = s_hot, ch = 0;
    bool page = false;
    if (ev == PANEL_PRESS) {
        s_hot = k;
        if (k >= 0 && s_k[k].rep) {
            ch = act(c, s_k[k].id, &page);
            s_next_rep = now_us + 450000;
        }
    } else if (ev == PANEL_MOVE) {
        if (s_hot >= 0 && s_k[s_hot].rep) {                /* steppers: hold to repeat */
            if (k == s_hot && now_us >= s_next_rep) {
                ch = act(c, s_k[k].id, &page);
                s_next_rep = now_us + 150000;
            }
        } else {
            s_hot = k;                                     /* follow the finger */
        }
    } else {
        if (s_hot >= 0 && !s_k[s_hot].rep && s_hot == k) ch = act(c, s_k[s_hot].id, &page);
        s_hot = -1;
    }
    if (ch & CTL_CLOSE) return ch;
    if (page) {
        if (ev != PANEL_RELEASE) s_hot = -1;
        show_all(c);
    } else if (ch) {
        build(c);                                          /* same page: same keys, new text */
        show_keys(old, s_hot);                             /* a stepper's row holds its value */
    } else if (old != s_hot) {
        show_keys(old, s_hot);
    }
    return ch;
}
